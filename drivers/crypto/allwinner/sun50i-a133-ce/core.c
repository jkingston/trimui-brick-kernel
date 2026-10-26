// SPDX-License-Identifier: GPL-2.0-only
/* A133 CE v4 AES/SHA-256: isolated DMA storage and coordinated four-flow recovery. */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <crypto/aes.h>
#include <crypto/internal/skcipher.h>
#include <crypto/internal/hash.h>
#include <crypto/sha2.h>
#include "recovery.h"
#include "wire.h"

#define CE_TDQ 0x00
#define CE_ICR 0x08
#define CE_ISR 0x0c
#define CE_TLR 0x10
#define CE_ESR 0x18
#define CE_RATE 400000000
#define CE_TIMEOUT_MS 1000
#define CE_BYTES 65536

struct a133_ce;
struct ce_flow {
	struct a133_ce *ce;
	struct workqueue_struct *queue;
	struct completion done;
	struct ce_storage *storage;
	dma_addr_t dma;
	unsigned int index, errors;
	bool notified;
};

struct a133_ce {
	struct device *dev;
	struct kref refs;
	void __iomem *base;
	resource_size_t start, size;
	struct clk_bulk_data clocks[3];
	struct reset_control *reset;
	struct mutex lifecycle;
	spinlock_t irq_lock;
	struct ce_recovery recovery;
	struct ce_flow flows[4];
	atomic_t next_flow;
	unsigned int armed;
	int irq, registered;
	bool irq_owned, irq_disabled, clocks_on, clocks_acquired, rate_held, pm_held;
	bool stopping, suspending, suspended, unexpected, hash_registered;
};

struct ce_tfm {
	struct a133_ce *ce;
	struct crypto_sync_skcipher *fallback;
	u8 key[AES_MAX_KEY_SIZE];
	unsigned int keylen;
};

struct ce_request {
	struct work_struct work;
	struct skcipher_request *request;
	struct ce_flow *flow;
	bool decrypt;
};

/* The SoC has one CE. Static algorithm descriptors outlive device removal. */
static DEFINE_MUTEX(ce_device_lock);
static struct a133_ce *device_ce;

static void ce_release(struct kref *ref)
{
	struct a133_ce *ce = container_of(ref, struct a133_ce, refs);

	put_device(ce->dev);
	kfree(ce);
}

static irqreturn_t ce_irq(int irq, void *data)
{
	struct a133_ce *ce = data;
	unsigned long flags;
	u32 status, error;
	int i;

	spin_lock_irqsave(&ce->irq_lock, flags);
	status = readl(ce->base + CE_ISR) & 0xff;
	if (!status) {
		spin_unlock_irqrestore(&ce->irq_lock, flags);
		return IRQ_NONE;
	}
	error = readl(ce->base + CE_ESR);
	writel(status, ce->base + CE_ISR);
	for (i = 0; i < 4; i++) {
		struct ce_flow *flow = &ce->flows[i];

		if (!(status & (3U << (2 * i))))
			continue;
		if (!(ce->armed & BIT(i)) || flow->notified)
			ce->unexpected = true;
		flow->errors |= (error >> (8 * i)) & 0xff;
		flow->notified = true;
		complete(&flow->done);
	}
	if (ce->unexpected)
		for (i = 0; i < 4; i++)
			complete(&ce->flows[i].done);
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	return IRQ_HANDLED;
}

static void ce_irq_drain(void *data)
{
	struct a133_ce *ce = data;

	if (!ce->irq_disabled) {
		disable_irq(ce->irq);
		ce->irq_disabled = true;
	}
}

static void ce_mask(void *data)
{
	struct a133_ce *ce = data;

	writel(0, ce->base + CE_ICR);
	readl(ce->base + CE_ICR);
}

static int ce_reset_assert(void *data)
{
	return reset_control_assert(((struct a133_ce *)data)->reset);
}

static int ce_reset_status(void *data)
{
	return reset_control_status(((struct a133_ce *)data)->reset);
}

static void ce_reset_hold(void *data)
{
	udelay(10);
}

static int ce_reset_deassert(void *data)
{
	return reset_control_deassert(((struct a133_ce *)data)->reset);
}

static void ce_clear(void *data)
{
	struct a133_ce *ce = data;
	unsigned long flags;

	writel(0xff, ce->base + CE_ISR);
	readl(ce->base + CE_ISR);
	spin_lock_irqsave(&ce->irq_lock, flags);
	ce->armed = 0;
	ce->unexpected = false;
	spin_unlock_irqrestore(&ce->irq_lock, flags);
}

static void ce_irq_resume(void *data)
{
	struct a133_ce *ce = data;

	if (ce->irq_disabled) {
		ce->irq_disabled = false;
		enable_irq(ce->irq);
	}
}

static const struct ce_recovery_ops recovery_ops = {
	.irq_drain = ce_irq_drain,
	.mask = ce_mask,
	.assert_reset = ce_reset_assert,
	.reset_status = ce_reset_status,
	.hold_reset = ce_reset_hold,
	.deassert_reset = ce_reset_deassert,
	.clear_pending = ce_clear,
	.irq_resume = ce_irq_resume,
};

/* Caller holds lifecycle. Only publication/retirement is serialized, not DMA. */
static int ce_launch(struct ce_flow *flow, unsigned int task_class)
{
	struct a133_ce *ce = flow->ce;
	unsigned long flags;
	int ret;

	ret = ce_recovery_submit(&ce->recovery, flow->index);
	if (ret)
		return ret;
	/* An uncontained transaction retains this reference, even after unbind. */
	__module_get(THIS_MODULE);
	spin_lock_irqsave(&ce->irq_lock, flags);
	reinit_completion(&flow->done);
	flow->errors = 0;
	flow->notified = false;
	ce->armed |= BIT(flow->index);
	writel(3U << (2 * flow->index), ce->base + CE_ISR);
	writel(ce->armed, ce->base + CE_ICR);
	dma_wmb();
	writel(flow->dma >> 2, ce->base + CE_TDQ);
	writel(task_class, ce->base + CE_TLR);
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	return 0;
}

static int ce_wait(struct ce_flow *flow, bool *release)
{
	struct a133_ce *ce = flow->ce;
	unsigned long flags;
	long waited;
	int cause = 0, result, i;

	waited = wait_for_completion_interruptible_timeout(&flow->done,
					 msecs_to_jiffies(CE_TIMEOUT_MS));
	mutex_lock(&ce->lifecycle);
	/* A peer may already have retired/quarantined this transaction. */
	if (ce->recovery.active & BIT(flow->index)) {
		spin_lock_irqsave(&ce->irq_lock, flags);
		if (waited < 0)
			cause = waited;
		else if (!waited)
			cause = -ETIMEDOUT;
		else if (!flow->notified || flow->errors || ce->unexpected)
			cause = -EIO;
		spin_unlock_irqrestore(&ce->irq_lock, flags);
		if (cause) {
			ce_recovery_fault(&ce->recovery, flow->index, cause,
					  &recovery_ops, ce);
			for (i = 0; i < 4; i++)
				complete(&ce->flows[i].done);
			if (ce->recovery.quarantine)
				dev_crit(ce->dev, "CE reset uncontained; DMA storage and resources retained until cold power cycle\n");
		} else {
			ce_recovery_complete(&ce->recovery, flow->index);
			spin_lock_irqsave(&ce->irq_lock, flags);
			ce->armed &= ~BIT(flow->index);
			writel(ce->armed, ce->base + CE_ICR);
			spin_unlock_irqrestore(&ce->irq_lock, flags);
		}
	}
	*release = !ce_recovery_take(&ce->recovery, flow->index, &result);
	if (!*release)
		result = (ce->recovery.quarantine & BIT(flow->index)) ?
			ce->recovery.result[flow->index] : -EIO;
	mutex_unlock(&ce->lifecycle);
	if (*release)
		module_put(THIS_MODULE);
	return result;
}

/* Larger requests stay in software, without exposing caller memory to CE DMA. */
static int ce_software(struct skcipher_request *req, struct ce_tfm *ctx,
		       bool decrypt)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback);
	struct scatterlist *output, *sg;
	u8 iv[AES_BLOCK_SIZE], *buffer;
	unsigned int ivsize = crypto_skcipher_ivsize(crypto_skcipher_reqtfm(req));
	unsigned int nents, offset, bytes, i;
	int ret;

	output = sgl_alloc(req->cryptlen, GFP_KERNEL, &nents);
	if (!output)
		return -ENOMEM;
	buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buffer) {
		sgl_free(output);
		return -ENOMEM;
	}
	if (ivsize)
		memcpy(iv, req->iv, ivsize);
	skcipher_request_set_sync_tfm(subreq, ctx->fallback);
	skcipher_request_set_callback(subreq, CRYPTO_TFM_REQ_MAY_SLEEP, NULL, NULL);
	skcipher_request_set_crypt(subreq, req->src, output, req->cryptlen, iv);
	ret = decrypt ? crypto_skcipher_decrypt(subreq) : crypto_skcipher_encrypt(subreq);
	if (!ret) {
		for (offset = 0; offset < req->cryptlen; offset += bytes) {
			bytes = min_t(unsigned int, PAGE_SIZE, req->cryptlen - offset);
			sg_pcopy_to_buffer(output, nents, buffer, bytes, offset);
			sg_pcopy_from_buffer(req->dst, sg_nents(req->dst), buffer, bytes, offset);
		}
		if (ivsize)
			memcpy(req->iv, iv, ivsize);
	}
	skcipher_request_zero(subreq);
	memzero_explicit(iv, sizeof(iv));
	kfree_sensitive(buffer);
	for_each_sg(output, sg, nents, i)
		memzero_explicit(sg_virt(sg), sg->length);
	sgl_free(output);
	return ret;
}

static void ce_work(struct work_struct *work)
{
	struct ce_request *rctx = container_of(work, struct ce_request, work);
	struct skcipher_request *req = rctx->request;
	struct ce_tfm *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	struct ce_flow *flow = rctx->flow;
	struct a133_ce *ce = ctx->ce;
	struct ce_storage *s = flow->storage;
	unsigned int cbc = crypto_skcipher_ivsize(crypto_skcipher_reqtfm(req)) != 0;
	bool release = true, prepared = false;
	int ret;

	mutex_lock(&ce->lifecycle);
	if (READ_ONCE(ce->suspending)) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (ce->stopping || ce->recovery.dead) {
		ret = -ENODEV;
		goto unlock;
	}
	if (req->cryptlen > CE_BYTES) {
		mutex_unlock(&ce->lifecycle);
		ret = ce_software(req, ctx, rctx->decrypt);
		goto finish;
	}
	prepared = true;
	memcpy(s->key, ctx->key, ctx->keylen);
	if (cbc)
		memcpy(s->iv, req->iv, AES_BLOCK_SIZE);
	sg_copy_to_buffer(req->src, sg_nents(req->src), s->input, req->cryptlen);
	ret = ce_encode_aes(s, flow->dma, flow->index, ctx->keylen,
			    cbc, rctx->decrypt, req->cryptlen);
	if (!ret)
		ret = ce_launch(flow, BIT(0));
unlock:
	mutex_unlock(&ce->lifecycle);
	if (ret)
		goto finish;
	ret = ce_wait(flow, &release);
	if (!ret) {
		dma_rmb();
		sg_copy_from_buffer(req->dst, sg_nents(req->dst), s->output, req->cryptlen);
		if (cbc)
			memcpy(req->iv, (rctx->decrypt ? s->input : s->output) +
			       req->cryptlen - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
	}
finish:
	if (prepared && release)
		memzero_explicit(s, sizeof(*s));
	skcipher_request_complete(req, ret);
}

static int ce_crypt(struct skcipher_request *req, bool decrypt)
{
	struct ce_tfm *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	struct a133_ce *ce = ctx->ce;
	struct ce_request *rctx = skcipher_request_ctx(req);
	unsigned int flow;
	unsigned long flags;
	int ret = -EINPROGRESS;

	if (!ctx->keylen || (req->cryptlen & (AES_BLOCK_SIZE - 1)))
		return -EINVAL;
	if (!req->cryptlen)
		return 0;
	if (sg_nents_for_len(req->src, req->cryptlen) < 0 ||
	    sg_nents_for_len(req->dst, req->cryptlen) < 0)
		return -EINVAL;
	spin_lock_irqsave(&ce->irq_lock, flags);
	if (READ_ONCE(ce->suspending)) {
		ret = -EAGAIN;
		goto out;
	}
	if (ce->stopping || READ_ONCE(ce->recovery.dead)) {
		ret = -ENODEV;
		goto out;
	}
	flow = (unsigned int)atomic_inc_return(&ce->next_flow) % 4;
	rctx->request = req;
	rctx->decrypt = decrypt;
	rctx->flow = &ce->flows[flow];
	INIT_WORK(&rctx->work, ce_work);
	queue_work(rctx->flow->queue, &rctx->work);
out:
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	return ret;
}

static int ce_encrypt(struct skcipher_request *req)
{
	return ce_crypt(req, false);
}

static int ce_decrypt(struct skcipher_request *req)
{
	return ce_crypt(req, true);
}

static int ce_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int len)
{
	struct ce_tfm *ctx = crypto_skcipher_ctx(tfm);
	int ret;

	ctx->keylen = 0;
	if (len != AES_KEYSIZE_128 && len != AES_KEYSIZE_192 && len != AES_KEYSIZE_256)
		return -EINVAL;
	ret = crypto_sync_skcipher_setkey(ctx->fallback, key, len);
	if (ret)
		return ret;
	memcpy(ctx->key, key, len);
	ctx->keylen = len;
	return 0;
}

static int ce_tfm_init(struct crypto_skcipher *tfm)
{
	struct ce_tfm *ctx = crypto_skcipher_ctx(tfm);

	ctx->fallback = crypto_alloc_sync_skcipher(crypto_tfm_alg_name(&tfm->base),
						  0, CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback))
		return PTR_ERR(ctx->fallback);
	mutex_lock(&ce_device_lock);
	ctx->ce = device_ce;
	if (ctx->ce)
		kref_get(&ctx->ce->refs);
	mutex_unlock(&ce_device_lock);
	if (!ctx->ce) {
		crypto_free_sync_skcipher(ctx->fallback);
		return -ENODEV;
	}
	crypto_skcipher_set_reqsize(tfm, sizeof(struct ce_request));
	return 0;
}

static void ce_tfm_exit(struct crypto_skcipher *tfm)
{
	struct ce_tfm *ctx = crypto_skcipher_ctx(tfm);

	crypto_free_sync_skcipher(ctx->fallback);
	memzero_explicit(ctx->key, sizeof(ctx->key));
	kref_put(&ctx->ce->refs, ce_release);
}

#define CE_ALG(mode, ivbytes) { \
	.base.cra_name = mode "(aes)", \
	.base.cra_driver_name = mode "-aes-a133-ce", \
	.base.cra_priority = 300, \
	.base.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK, \
	.base.cra_blocksize = AES_BLOCK_SIZE, \
	.base.cra_ctxsize = sizeof(struct ce_tfm), \
	.base.cra_module = THIS_MODULE, \
	.min_keysize = AES_MIN_KEY_SIZE, .max_keysize = AES_MAX_KEY_SIZE, \
	.ivsize = ivbytes, .setkey = ce_setkey, .encrypt = ce_encrypt, \
	.decrypt = ce_decrypt, .init = ce_tfm_init, .exit = ce_tfm_exit, \
}

static struct skcipher_alg algorithms[] = {
	CE_ALG("ecb", 0),
	CE_ALG("cbc", AES_BLOCK_SIZE),
};

/* Streaming and large digests use a synchronous fallback, as in sun8i-ce.
 * Hardware digest requests share the same four queues and reset coordinator as
 * AES. No hash descriptor ever points outside a retained per-flow allocation.
 */
struct ce_hash_tfm {
	struct a133_ce *ce;
	struct crypto_ahash *fallback;
};
struct ce_hash_request {
	struct work_struct work;
	struct ahash_request *request;
	struct ce_flow *flow;
	u8 digest[SHA256_DIGEST_SIZE];
	struct ahash_request fallback;
};

static struct ahash_request *ce_hash_fallback(struct ahash_request *req, bool input)
{
	struct ce_hash_tfm *ctx = crypto_ahash_ctx(crypto_ahash_reqtfm(req));
	struct ce_hash_request *rctx = ahash_request_ctx(req);
	struct ahash_request *fb = &rctx->fallback;

	ahash_request_set_tfm(fb, ctx->fallback);
	ahash_request_set_callback(fb, req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   NULL, NULL);
	if (!input)
		ahash_request_set_crypt(fb, NULL, rctx->digest, 0);
	else if (ahash_request_isvirt(req))
		ahash_request_set_virt(fb, req->svirt, rctx->digest, req->nbytes);
	else
		ahash_request_set_crypt(fb, req->src, rctx->digest, req->nbytes);
	return fb;
}

static int ce_hash_commit(struct ahash_request *req, int ret)
{
	struct ce_hash_request *rctx = ahash_request_ctx(req);

	if (!ret)
		memcpy(req->result, rctx->digest, SHA256_DIGEST_SIZE);
	memzero_explicit(rctx->digest, sizeof(rctx->digest));
	return ret;
}

static int ce_hash_init(struct ahash_request *req)
{
	return crypto_ahash_init(ce_hash_fallback(req, false));
}

static int ce_hash_update(struct ahash_request *req)
{
	return crypto_ahash_update(ce_hash_fallback(req, true));
}

static int ce_hash_final(struct ahash_request *req)
{
	return ce_hash_commit(req, crypto_ahash_final(ce_hash_fallback(req, false)));
}

static int ce_hash_finup(struct ahash_request *req)
{
	return ce_hash_commit(req, crypto_ahash_finup(ce_hash_fallback(req, true)));
}

static int ce_hash_export(struct ahash_request *req, void *out)
{
	return crypto_ahash_export(ce_hash_fallback(req, false), out);
}

static int ce_hash_import(struct ahash_request *req, const void *in)
{
	return crypto_ahash_import(ce_hash_fallback(req, false), in);
}

static void ce_hash_work(struct work_struct *work)
{
	struct ce_hash_request *rctx = container_of(work, struct ce_hash_request, work);
	struct ahash_request *req = rctx->request;
	struct ce_flow *flow = rctx->flow;
	struct a133_ce *ce = flow->ce;
	struct ce_storage *s = flow->storage;
	bool release = true, prepared = false;
	int ret;

	mutex_lock(&ce->lifecycle);
	if (READ_ONCE(ce->suspending)) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (ce->stopping || ce->recovery.dead) {
		ret = -ENODEV;
		goto unlock;
	}
	prepared = true;
	if (req->nbytes) {
		if (ahash_request_isvirt(req)) {
			memcpy(s->input, req->svirt, req->nbytes);
		} else if (sg_copy_to_buffer(req->src, sg_nents(req->src),
					     s->input, req->nbytes) != req->nbytes) {
			ret = -EINVAL;
			goto unlock;
		}
	}
	ret = ce_encode_sha256(s, flow->dma, flow->index, req->nbytes);
	if (!ret)
		ret = ce_launch(flow, BIT(1));
unlock:
	mutex_unlock(&ce->lifecycle);
	if (ret)
		goto finish;
	ret = ce_wait(flow, &release);
	if (!ret) {
		dma_rmb();
		memcpy(req->result, s->output, SHA256_DIGEST_SIZE);
	}
finish:
	if (prepared && release)
		memzero_explicit(s, sizeof(*s));
	ahash_request_complete(req, ret);
}

static int ce_hash_digest(struct ahash_request *req)
{
	struct ce_hash_tfm *ctx = crypto_ahash_ctx(crypto_ahash_reqtfm(req));
	struct a133_ce *ce = ctx->ce;
	struct ce_hash_request *rctx = ahash_request_ctx(req);
	unsigned long flags;
	unsigned int flow;
	int ret = -EINPROGRESS;

	if (req->nbytes && !ahash_request_isvirt(req) &&
	    sg_nents_for_len(req->src, req->nbytes) < 0)
		return -EINVAL;
	/* This fallback cannot submit DMA or asynchronously retain the request. */
	if (req->nbytes > 65408)
		return ce_hash_commit(req, crypto_ahash_digest(ce_hash_fallback(req, true)));
	spin_lock_irqsave(&ce->irq_lock, flags);
	if (READ_ONCE(ce->suspending)) {
		ret = -EAGAIN;
		goto out;
	}
	if (ce->stopping || READ_ONCE(ce->recovery.dead)) {
		ret = -ENODEV;
		goto out;
	}
	flow = (unsigned int)atomic_inc_return(&ce->next_flow) % 4;
	rctx->request = req;
	rctx->flow = &ce->flows[flow];
	INIT_WORK(&rctx->work, ce_hash_work);
	queue_work(rctx->flow->queue, &rctx->work);
out:
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	return ret;
}

static int ce_hash_tfm_init(struct crypto_ahash *tfm)
{
	struct ce_hash_tfm *ctx = crypto_ahash_ctx(tfm);

	ctx->fallback = crypto_alloc_ahash("sha256", 0,
				CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback))
		return PTR_ERR(ctx->fallback);
	mutex_lock(&ce_device_lock);
	ctx->ce = device_ce;
	if (ctx->ce)
		kref_get(&ctx->ce->refs);
	mutex_unlock(&ce_device_lock);
	if (!ctx->ce) {
		crypto_free_ahash(ctx->fallback);
		return -ENODEV;
	}
	crypto_ahash_set_reqsize(tfm, sizeof(struct ce_hash_request) +
				crypto_ahash_reqsize(ctx->fallback));
	crypto_ahash_set_statesize(tfm, crypto_ahash_statesize(ctx->fallback));
	return 0;
}

static void ce_hash_tfm_exit(struct crypto_ahash *tfm)
{
	struct ce_hash_tfm *ctx = crypto_ahash_ctx(tfm);

	crypto_free_ahash(ctx->fallback);
	kref_put(&ctx->ce->refs, ce_release);
}

static struct ahash_alg hash_algorithm = {
	.init = ce_hash_init, .update = ce_hash_update,
	.final = ce_hash_final, .finup = ce_hash_finup, .digest = ce_hash_digest,
	.export = ce_hash_export, .import = ce_hash_import,
	.init_tfm = ce_hash_tfm_init, .exit_tfm = ce_hash_tfm_exit,
	.halg = {
		.digestsize = SHA256_DIGEST_SIZE,
		.statesize = sizeof(struct sha256_state),
		.base = {
			.cra_name = "sha256", .cra_driver_name = "sha256-a133-ce",
			.cra_priority = 300,
			.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK |
				     CRYPTO_ALG_REQ_VIRT,
			.cra_blocksize = SHA256_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct ce_hash_tfm),
			.cra_module = THIS_MODULE,
		},
	},
};

static void ce_cleanup(struct a133_ce *ce)
{
	int i;

	if (ce->irq_owned)
		free_irq(ce->irq, ce);
	for (i = 0; i < 4; i++) {
		if (ce->flows[i].queue)
			destroy_workqueue(ce->flows[i].queue);
		if (ce->flows[i].storage) {
			memzero_explicit(ce->flows[i].storage, sizeof(struct ce_storage));
			dma_free_coherent(ce->dev, sizeof(struct ce_storage),
					  ce->flows[i].storage, ce->flows[i].dma);
		}
	}
	if (ce->pm_held) {
		pm_runtime_put_noidle(ce->dev);
		pm_runtime_disable(ce->dev);
	}
	if (ce->reset) {
		if (ce->clocks_on)
			reset_control_assert(ce->reset);
		reset_control_put(ce->reset);
	}
	if (ce->clocks_on)
		clk_bulk_disable_unprepare(3, ce->clocks);
	if (ce->rate_held)
		clk_rate_exclusive_put(ce->clocks[1].clk);
	if (ce->clocks_acquired)
		clk_bulk_put(3, ce->clocks);
	if (ce->base)
		iounmap(ce->base);
	if (ce->size)
		release_mem_region(ce->start, ce->size);
	kref_put(&ce->refs, ce_release);
}

static void ce_quiesce(struct a133_ce *ce)
{
	unsigned long flags;
	int i;

	mutex_lock(&ce_device_lock);
	if (device_ce == ce)
		device_ce = NULL;
	mutex_unlock(&ce_device_lock);
	mutex_lock(&ce->lifecycle);
	spin_lock_irqsave(&ce->irq_lock, flags);
	ce->stopping = true;
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	mutex_unlock(&ce->lifecycle);
	if (ce->hash_registered) {
		crypto_unregister_ahash(&hash_algorithm);
		ce->hash_registered = false;
	}
	while (ce->registered)
		crypto_unregister_skcipher(&algorithms[--ce->registered]);
	for (i = 0; i < 4; i++)
		if (ce->flows[i].queue)
			flush_workqueue(ce->flows[i].queue);
	if (ce->irq_owned && ce->clocks_on) {
		ce_irq_drain(ce);
		ce_mask(ce);
	}
}

static void ce_stop(struct a133_ce *ce)
{
	ce_quiesce(ce);
	if (ce->recovery.quarantine) {
		/*
		 * No devm resources: removal cannot revoke these allocations.
		 * Quarantined flows retain module references, and the device,
		 * PM lease, clocks, reset handle, IRQ and MMIO remain owned.
		 */
		dev_crit(ce->dev, "Retaining CE resources after uncontained DMA; cold power cycle required\n");
		return;
	}
	ce_cleanup(ce);
}

static int ce_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a133_ce *ce;
	struct resource *res;
	int ret, i;

	/* Retained coherent memory must not lose an IOMMU mapping on unbind. */
	if (device_iommu_mapped(dev))
		return -EOPNOTSUPP;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ce = kzalloc_obj(*ce);
	if (!ce)
		return -ENOMEM;
	ce->dev = get_device(dev);
	kref_init(&ce->refs);
	mutex_init(&ce->lifecycle);
	spin_lock_init(&ce->irq_lock);
	ce->clocks[0].id = "bus";
	ce->clocks[1].id = "mod";
	ce->clocks[2].id = "ram";
	for (i = 0; i < 4; i++) {
		ce->flows[i].ce = ce;
		ce->flows[i].index = i;
		init_completion(&ce->flows[i].done);
	}
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || !request_mem_region(res->start, resource_size(res), dev_name(dev))) {
		ret = -EBUSY;
		goto fail;
	}
	ce->start = res->start;
	ce->size = resource_size(res);
	ce->base = ioremap(ce->start, ce->size);
	if (!ce->base) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = clk_bulk_get(dev, 3, ce->clocks);
	if (ret)
		goto fail;
	ce->clocks_acquired = true;
	ret = clk_set_rate_exclusive(ce->clocks[1].clk, CE_RATE);
	if (ret)
		goto fail;
	ce->rate_held = true;
	if (clk_get_rate(ce->clocks[1].clk) != CE_RATE) {
		ret = -ERANGE;
		goto fail;
	}
	ce->reset = reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(ce->reset)) {
		ret = PTR_ERR(ce->reset);
		ce->reset = NULL;
		goto fail;
	}
	ret = clk_bulk_prepare_enable(3, ce->clocks);
	if (ret)
		goto fail;
	ce->clocks_on = true;
	/* Discard any bootloader transaction before publishing DMA storage. */
	ret = reset_control_assert(ce->reset);
	if (ret)
		goto fail;
	ret = reset_control_status(ce->reset);
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		goto fail;
	}
	udelay(10);
	ret = reset_control_deassert(ce->reset);
	if (ret)
		goto fail;
	ret = reset_control_status(ce->reset);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto fail;
	}
	ce_mask(ce);
	ce_clear(ce);
	ce->irq = platform_get_irq(pdev, 0);
	if (ce->irq < 0) {
		ret = ce->irq;
		goto fail;
	}
	ret = request_irq(ce->irq, ce_irq, 0, dev_name(dev), ce);
	if (ret)
		goto fail;
	ce->irq_owned = true;
	for (i = 0; i < 4; i++) {
		struct ce_flow *flow = &ce->flows[i];

		flow->storage = dma_alloc_coherent(dev, sizeof(*flow->storage),
						 &flow->dma, GFP_KERNEL);
		if (!flow->storage) {
			ret = -ENOMEM;
			goto fail;
		}
		flow->queue = alloc_ordered_workqueue("a133-ce-%d", WQ_MEM_RECLAIM, i);
		if (!flow->queue) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	ret = pm_runtime_set_active(dev);
	if (ret)
		goto fail;
	pm_runtime_enable(dev);
	pm_runtime_get_noresume(dev);
	ce->pm_held = true;
	mutex_lock(&ce_device_lock);
	if (device_ce) {
		mutex_unlock(&ce_device_lock);
		ret = -EBUSY;
		goto fail;
	}
	device_ce = ce;
	mutex_unlock(&ce_device_lock);
	platform_set_drvdata(pdev, ce);
	for (i = 0; i < ARRAY_SIZE(algorithms); i++) {
		ret = crypto_register_skcipher(&algorithms[i]);
		if (ret)
			goto fail;
		ce->registered++;
	}
	ret = crypto_register_ahash(&hash_algorithm);
	if (ret)
		goto fail;
	ce->hash_registered = true;
	return 0;
fail:
	ce_stop(ce);
	return ret;
}

static void ce_remove(struct platform_device *pdev)
{
	ce_stop(platform_get_drvdata(pdev));
}

static void ce_shutdown(struct platform_device *pdev)
{
	/* Keep ownership for a possible subsequent remove; this is idempotent. */
	ce_quiesce(platform_get_drvdata(pdev));
}

static int ce_suspend(struct device *dev)
{
	struct a133_ce *ce = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int i;
	int ret, rollback;

	/* Stop admission before draining work submitted by Crypto API callers. */
	spin_lock_irqsave(&ce->irq_lock, flags);
	if (ce->stopping || ce->recovery.dead) {
		spin_unlock_irqrestore(&ce->irq_lock, flags);
		return -EIO;
	}
	if (ce->suspended) {
		spin_unlock_irqrestore(&ce->irq_lock, flags);
		return 0;
	}
	ce->suspending = true;
	spin_unlock_irqrestore(&ce->irq_lock, flags);

	for (i = 0; i < ARRAY_SIZE(ce->flows); i++)
		flush_workqueue(ce->flows[i].queue);

	mutex_lock(&ce->lifecycle);
	if (ce->recovery.active || ce->recovery.retired ||
	    ce->recovery.quarantine) {
		ret = -EBUSY;
		goto admission_rollback;
	}

	ce_irq_drain(ce);
	ce_mask(ce);
	ret = reset_control_assert(ce->reset);
	if (ret)
		goto hardware_rollback;
	ret = reset_control_status(ce->reset);
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		goto hardware_rollback;
	}
	ce_reset_hold(ce);
	clk_bulk_disable_unprepare(ARRAY_SIZE(ce->clocks), ce->clocks);
	ce->clocks_on = false;
	ce->suspended = true;
	mutex_unlock(&ce->lifecycle);
	return 0;

hardware_rollback:
	/* Suspend was rejected: reconstruct the known idle, IRQ-enabled state. */
	rollback = reset_control_deassert(ce->reset);
	if (!rollback) {
		rollback = reset_control_status(ce->reset);
		if (rollback > 0)
			rollback = -EIO;
	}
	if (!rollback) {
		ce_clear(ce);
		ce_irq_resume(ce);
	} else {
		ce->recovery.dead = true;
		ce->recovery.reset_error = rollback;
	}
admission_rollback:
	spin_lock_irqsave(&ce->irq_lock, flags);
	if (!ce->recovery.dead)
		ce->suspending = false;
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	mutex_unlock(&ce->lifecycle);
	return ret;
}

static int ce_resume(struct device *dev)
{
	struct a133_ce *ce = dev_get_drvdata(dev);
	unsigned long flags;
	int ret, rollback;

	mutex_lock(&ce->lifecycle);
	if (!ce->suspended) {
		ret = ce->recovery.dead ? -EIO : 0;
		goto out;
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(ce->clocks), ce->clocks);
	if (ret)
		goto out;
	ce->clocks_on = true;
	ret = reset_control_deassert(ce->reset);
	if (ret)
		goto power_rollback;
	ret = reset_control_status(ce->reset);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto power_rollback;
	}
	ce_mask(ce);
	ce_clear(ce);
	ce_irq_resume(ce);
	ce->suspended = false;
	spin_lock_irqsave(&ce->irq_lock, flags);
	ce->suspending = false;
	spin_unlock_irqrestore(&ce->irq_lock, flags);
	goto out;

power_rollback:
	rollback = reset_control_assert(ce->reset);
	if (!rollback) {
		rollback = reset_control_status(ce->reset);
		if (rollback == 1)
			rollback = 0;
		else
			rollback = rollback < 0 ? rollback : -EIO;
	}
	if (rollback) {
		ce->recovery.dead = true;
		ce->recovery.reset_error = rollback;
		ce->suspended = false;
		goto out;
	}
	clk_bulk_disable_unprepare(ARRAY_SIZE(ce->clocks), ce->clocks);
	ce->clocks_on = false;
out:
	mutex_unlock(&ce->lifecycle);
	return ret;
}

static const struct dev_pm_ops ce_pm_ops = {
	.suspend = ce_suspend,
	.freeze = ce_suspend,
	.poweroff = ce_suspend,
	.resume = ce_resume,
	.thaw = ce_resume,
	.restore = ce_resume,
};

static const struct of_device_id ce_of_match[] = {
	{ .compatible = "allwinner,sun50i-a133-crypto" },
	{ }
};
MODULE_DEVICE_TABLE(of, ce_of_match);

static struct platform_driver ce_driver = {
	.probe = ce_probe,
	.remove = ce_remove,
	.shutdown = ce_shutdown,
	.driver = {
		.name = "sun50i-a133-ce",
		.of_match_table = ce_of_match,
		.pm = &ce_pm_ops,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ce_driver);
MODULE_DESCRIPTION("Allwinner A133 Crypto Engine AES and SHA-256 driver");
MODULE_LICENSE("GPL");

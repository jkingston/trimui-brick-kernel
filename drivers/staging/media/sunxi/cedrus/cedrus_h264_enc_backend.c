// SPDX-License-Identifier: GPL-2.0
/* Unexposed A133 H.264 encoder backend ownership. */

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "cedrus.h"
#include "cedrus_h264_enc_backend.h"

enum cedrus_a133_h264_allocation_id {
	CEDRUS_A133_H264_RECONSTRUCTION_0,
	CEDRUS_A133_H264_RECONSTRUCTION_1,
	CEDRUS_A133_H264_SUBPIXEL_0,
	CEDRUS_A133_H264_SUBPIXEL_1,
	CEDRUS_A133_H264_MOTION_VECTORS,
	CEDRUS_A133_H264_MACROBLOCK_INFO,
	CEDRUS_A133_H264_WORK_9C,
	CEDRUS_A133_H264_ALLOCATION_COUNT,
};

struct cedrus_a133_h264_allocation {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct cedrus_a133_h264_backend_ctx {
	struct list_head node;
	/* Serializes IRQ completion with abort, reset and suspend. */
	spinlock_t job_lock;
	struct cedrus_a133_h264_backend_job job;
	struct cedrus_a133_h264_allocation
		allocations[CEDRUS_A133_H264_ALLOCATION_COUNT];
};

struct cedrus_a133_h264_backend_dev {
	/* Protects the registry, device state and allocation lifetime. */
	struct mutex lock;
	struct list_head contexts;
	bool suspended;
	bool unbound;
};

static int cedrus_a133_h264_backend_bind(struct cedrus_dev *dev)
{
	struct cedrus_a133_h264_backend_dev *backend;

	if (!dev || dev->enc_priv)
		return -EINVAL;

	backend = kzalloc_obj(*backend);
	if (!backend)
		return -ENOMEM;
	mutex_init(&backend->lock);
	INIT_LIST_HEAD(&backend->contexts);
	dev->enc_priv = backend;
	return 0;
}

static int cedrus_a133_h264_backend_ctx_init(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_dev *backend;
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	int ret = 0;

	if (!ctx || !ctx->dev || ctx->enc_priv)
		return -EINVAL;
	backend = ctx->dev->enc_priv;
	if (!backend)
		return -ENODEV;

	backend_ctx = kzalloc_obj(*backend_ctx);
	if (!backend_ctx)
		return -ENOMEM;
	spin_lock_init(&backend_ctx->job_lock);
	cedrus_a133_h264_backend_job_init(&backend_ctx->job);

	mutex_lock(&backend->lock);
	if (backend->suspended || backend->unbound) {
		ret = -EBUSY;
	} else {
		list_add_tail(&backend_ctx->node, &backend->contexts);
		ctx->enc_priv = backend_ctx;
	}
	mutex_unlock(&backend->lock);
	if (ret)
		kfree(backend_ctx);
	return ret;
}

static void
cedrus_a133_h264_backend_free_allocations(struct cedrus_ctx *ctx,
					  struct cedrus_a133_h264_backend_ctx *backend_ctx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(backend_ctx->allocations); i++) {
		struct cedrus_a133_h264_allocation *allocation =
			&backend_ctx->allocations[i];

		if (!allocation->cpu)
			continue;
		dma_free_coherent(ctx->dev->dev, allocation->size,
				  allocation->cpu, allocation->dma);
		*allocation = (struct cedrus_a133_h264_allocation) {};
	}
}

static void cedrus_a133_h264_backend_ctx_cleanup(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_dev *backend;
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	unsigned long flags;
	int ret;

	if (!ctx || !ctx->dev || !ctx->enc_priv)
		return;
	backend = ctx->dev->enc_priv;
	backend_ctx = ctx->enc_priv;
	if (!backend)
		return;

	mutex_lock(&backend->lock);
	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	if (backend_ctx->job.buffers_owned) {
		ret = cedrus_a133_h264_backend_buffers_clear(&backend_ctx->job);
		WARN_ON(ret);
		cedrus_a133_h264_backend_job_init(&backend_ctx->job);
	}
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	cedrus_a133_h264_backend_free_allocations(ctx, backend_ctx);
	list_del(&backend_ctx->node);
	ctx->enc_priv = NULL;
	mutex_unlock(&backend->lock);
	kfree(backend_ctx);
}

static int cedrus_a133_h264_backend_allocate(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_dev *backend = ctx->dev->enc_priv;
	struct cedrus_a133_h264_backend_ctx *backend_ctx = ctx->enc_priv;
	struct cedrus_a133_h264_enc_buffer_layout layout;
	struct cedrus_a133_h264_job_buffers buffers = {};
	struct device *dev = ctx->dev->dev;
	size_t sizes[CEDRUS_A133_H264_ALLOCATION_COUNT];
	u32 width = ctx->src_fmt.width;
	u32 height = ctx->src_fmt.height;
	unsigned long flags;
	unsigned int i;
	bool owned;
	int ret;

	if (!backend || !backend_ctx)
		return -ENODEV;
	ret = cedrus_a133_h264_enc_buffer_layout(width, height, &layout);
	if (ret)
		return ret;
	sizes[CEDRUS_A133_H264_RECONSTRUCTION_0] =
		PAGE_ALIGN(layout.reconstruction);
	sizes[CEDRUS_A133_H264_RECONSTRUCTION_1] =
		PAGE_ALIGN(layout.reconstruction);
	sizes[CEDRUS_A133_H264_SUBPIXEL_0] = PAGE_ALIGN(layout.subpixel);
	sizes[CEDRUS_A133_H264_SUBPIXEL_1] = PAGE_ALIGN(layout.subpixel);
	sizes[CEDRUS_A133_H264_MOTION_VECTORS] =
		PAGE_ALIGN(layout.motion_vectors);
	sizes[CEDRUS_A133_H264_MACROBLOCK_INFO] =
		PAGE_ALIGN(layout.macroblock_info);
	sizes[CEDRUS_A133_H264_WORK_9C] = PAGE_ALIGN(layout.work_9c);

	mutex_lock(&backend->lock);
	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	owned = backend_ctx->job.buffers_owned;
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	if (backend->suspended || backend->unbound || owned) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(backend_ctx->allocations); i++) {
		struct cedrus_a133_h264_allocation *allocation =
			&backend_ctx->allocations[i];

		allocation->size = sizes[i];
		allocation->cpu = dma_alloc_coherent(dev, allocation->size,
						     &allocation->dma, GFP_KERNEL);
		if (!allocation->cpu) {
			ret = -ENOMEM;
			goto err_free;
		}
	}

	buffers.reconstruction[0] =
		backend_ctx->allocations[CEDRUS_A133_H264_RECONSTRUCTION_0].dma;
	buffers.reconstruction[1] =
		backend_ctx->allocations[CEDRUS_A133_H264_RECONSTRUCTION_1].dma;
	buffers.reconstruction_size[0] = sizes[CEDRUS_A133_H264_RECONSTRUCTION_0];
	buffers.reconstruction_size[1] = sizes[CEDRUS_A133_H264_RECONSTRUCTION_1];
	buffers.subpixel[0] =
		backend_ctx->allocations[CEDRUS_A133_H264_SUBPIXEL_0].dma;
	buffers.subpixel[1] =
		backend_ctx->allocations[CEDRUS_A133_H264_SUBPIXEL_1].dma;
	buffers.subpixel_size[0] = sizes[CEDRUS_A133_H264_SUBPIXEL_0];
	buffers.subpixel_size[1] = sizes[CEDRUS_A133_H264_SUBPIXEL_1];
	buffers.motion_vectors =
		backend_ctx->allocations[CEDRUS_A133_H264_MOTION_VECTORS].dma;
	buffers.motion_vectors_size = sizes[CEDRUS_A133_H264_MOTION_VECTORS];
	buffers.macroblock_info =
		backend_ctx->allocations[CEDRUS_A133_H264_MACROBLOCK_INFO].dma;
	buffers.macroblock_info_size = sizes[CEDRUS_A133_H264_MACROBLOCK_INFO];
	buffers.work_9c =
		backend_ctx->allocations[CEDRUS_A133_H264_WORK_9C].dma;
	buffers.work_9c_size = sizes[CEDRUS_A133_H264_WORK_9C];
	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	ret = cedrus_a133_h264_backend_buffers_set(&backend_ctx->job,
						   width, height, &buffers);
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	if (!ret)
		goto out;

err_free:
	cedrus_a133_h264_backend_free_allocations(ctx, backend_ctx);
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static void cedrus_a133_h264_backend_release(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_dev *backend = ctx->dev->enc_priv;
	struct cedrus_a133_h264_backend_ctx *backend_ctx = ctx->enc_priv;
	unsigned long flags;
	int ret;

	if (!backend || !backend_ctx)
		return;

	mutex_lock(&backend->lock);
	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	if (!backend_ctx->job.buffers_owned) {
		spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
		mutex_unlock(&backend->lock);
		return;
	}
	ret = cedrus_a133_h264_backend_buffers_clear(&backend_ctx->job);
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	if (WARN_ON(ret)) {
		mutex_unlock(&backend->lock);
		return;
	}
	cedrus_a133_h264_backend_free_allocations(ctx, backend_ctx);
	mutex_unlock(&backend->lock);
}

static int cedrus_a133_h264_backend_abort(struct cedrus_ctx *ctx,
					  enum cedrus_enc_abort_reason reason)
{
	struct cedrus_a133_h264_backend_dev *backend;
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	unsigned long flags;
	int ret;

	if (!ctx || !ctx->dev || !ctx->enc_priv ||
	    (unsigned int)reason > CEDRUS_ENC_ABORT_IOMMU_FAULT)
		return -EINVAL;
	backend = ctx->dev->enc_priv;
	backend_ctx = ctx->enc_priv;
	if (!backend)
		return -ENODEV;

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	ret = cedrus_a133_h264_backend_job_abort(&backend_ctx->job);
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	return ret;
}

static int cedrus_a133_h264_backend_ctx_reset_complete(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_dev *backend;
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	unsigned long flags;
	int ret;

	if (!ctx || !ctx->dev || !ctx->enc_priv)
		return -EINVAL;
	backend = ctx->dev->enc_priv;
	backend_ctx = ctx->enc_priv;
	if (!backend)
		return -ENODEV;

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	ret = cedrus_a133_h264_backend_reset_complete(&backend_ctx->job);
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	return ret;
}

static int cedrus_a133_h264_backend_dev_suspend(struct cedrus_dev *dev)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	struct cedrus_a133_h264_backend_dev *backend;
	unsigned long flags;
	int ret = 0;

	if (!dev || !dev->enc_priv)
		return -EINVAL;
	backend = dev->enc_priv;

	mutex_lock(&backend->lock);
	if (backend->suspended || backend->unbound) {
		ret = -EBUSY;
		goto out;
	}
	list_for_each_entry(backend_ctx, &backend->contexts, node) {
		spin_lock_irqsave(&backend_ctx->job_lock, flags);
		if (backend_ctx->job.lifecycle.state != CEDRUS_A133_H264_JOB_IDLE ||
		    backend_ctx->job.lifecycle.active_generation) {
			ret = -EBUSY;
			spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
			goto out;
		}
		spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	}
	list_for_each_entry(backend_ctx, &backend->contexts, node) {
		spin_lock_irqsave(&backend_ctx->job_lock, flags);
		ret = cedrus_a133_h264_backend_suspend(&backend_ctx->job);
		spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
		if (WARN_ON(ret))
			goto out;
	}
	backend->suspended = true;
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int cedrus_a133_h264_backend_dev_resume(struct cedrus_dev *dev)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	struct cedrus_a133_h264_backend_dev *backend;
	unsigned long flags;
	int ret = 0;

	if (!dev || !dev->enc_priv)
		return -EINVAL;
	backend = dev->enc_priv;

	mutex_lock(&backend->lock);
	if (!backend->suspended || backend->unbound) {
		ret = -EBUSY;
		goto out;
	}
	list_for_each_entry(backend_ctx, &backend->contexts, node) {
		spin_lock_irqsave(&backend_ctx->job_lock, flags);
		if (backend_ctx->job.lifecycle.state !=
		    CEDRUS_A133_H264_JOB_SUSPENDED ||
		    backend_ctx->job.lifecycle.active_generation) {
			ret = -EBUSY;
			spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
			goto out;
		}
		spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	}
	list_for_each_entry(backend_ctx, &backend->contexts, node) {
		spin_lock_irqsave(&backend_ctx->job_lock, flags);
		ret = cedrus_a133_h264_backend_resume(&backend_ctx->job);
		spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
		if (WARN_ON(ret))
			goto out;
	}
	backend->suspended = false;
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static void cedrus_a133_h264_backend_unbind(struct cedrus_dev *dev)
{
	struct cedrus_a133_h264_backend_dev *backend;

	if (!dev || !dev->enc_priv)
		return;
	backend = dev->enc_priv;

	mutex_lock(&backend->lock);
	if (WARN_ON(!list_empty(&backend->contexts))) {
		mutex_unlock(&backend->lock);
		return;
	}
	backend->unbound = true;
	dev->enc_priv = NULL;
	mutex_unlock(&backend->lock);
	kfree(backend);
}

#if IS_ENABLED(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE_KUNIT_TEST)
struct cedrus_a133_h264_backend_job *
cedrus_a133_h264_backend_test_job(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;

	if (!ctx || !ctx->enc_priv)
		return NULL;
	backend_ctx = ctx->enc_priv;
	return &backend_ctx->job;
}
#endif

const struct cedrus_enc_backend_ops cedrus_a133_h264_backend_candidate_ops = {
	.contract = CEDRUS_A133_H264_ENC_CONTRACT,
	.bind = cedrus_a133_h264_backend_bind,
	.ctx_init = cedrus_a133_h264_backend_ctx_init,
	.ctx_cleanup = cedrus_a133_h264_backend_ctx_cleanup,
	.start_streaming = cedrus_a133_h264_backend_allocate,
	.stop_streaming = cedrus_a133_h264_backend_release,
	.abort = cedrus_a133_h264_backend_abort,
	.reset_complete = cedrus_a133_h264_backend_ctx_reset_complete,
	.suspend = cedrus_a133_h264_backend_dev_suspend,
	.resume = cedrus_a133_h264_backend_dev_resume,
	.unbind = cedrus_a133_h264_backend_unbind,
};

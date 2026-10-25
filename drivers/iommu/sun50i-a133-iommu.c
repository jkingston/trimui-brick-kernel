// SPDX-License-Identifier: GPL-2.0-only
/* Allwinner A133: one page-table root shared by seven bus masters. */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#define A133_MASTERS 7
#define A133_ALL GENMASK(6, 0)
#define A133_ENABLE 0x020
#define A133_BYPASS 0x030
#define A133_GATING 0x040
#define A133_TTB 0x050
#define A133_PREFETCH 0x070
#define A133_FLUSH 0x080
#define A133_MODE 0x084
#define A133_TLB_ADDR 0x090
#define A133_TLB_MASK 0x094
#define A133_TLB_TRIGGER 0x098
#define A133_PTW_ADDR 0x0a0
#define A133_PTW_TRIGGER 0x0a8
#define A133_INT_ENABLE 0x100
#define A133_INT_CLEAR 0x104
#define A133_INT_STATUS 0x108
#define A133_DT_SIZE SZ_16K
#define A133_PT_SIZE SZ_1K
#define A133_VALID BIT(1)

struct a133_iommu;
struct a133_domain {
	struct iommu_domain domain;
	struct a133_iommu *iommu;
	u32 *dt;
	dma_addr_t dt_dma;
	u32 *pt[4096];
	dma_addr_t pt_dma[4096];
};

struct a133_client {
	struct a133_iommu *iommu;
	u32 masters;
	};

struct a133_iommu {
	struct iommu_device iommu;
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct dma_pool *pool;
	u32 *empty;
	dma_addr_t empty_dma;
	/* Register programming and table publication share this lock. */
	spinlock_t lock;
	struct a133_domain *installed;
	u32 owned;
	u32 attached;
};

static struct a133_domain *to_a133(struct iommu_domain *domain)
{
	return container_of(domain, struct a133_domain, domain);
}

static u32 a133_pte(phys_addr_t pa, int prot)
{
	/* Shipped A133 firmware uses direct R/W bits, not H6 authority indices. */
	return (pa & GENMASK(31, 12)) | ((prot << 2) & 0xc) | A133_VALID;
}

static int a133_poll(struct a133_iommu *iommu, unsigned int offset)
{
	u32 val;

	return readl_poll_timeout_atomic(iommu->base + offset, val, !val, 1, 2000);
}

static void a133_timeout(struct a133_iommu *iommu, int err)
{
	/* A void unmap-sync cannot return stale translations to the DMA API.
	 * Do not change masters to physical bypass or release any tables here.
	 * This stops software reuse, NOT bus traffic; hardware recovery needs
	 * a separately validated consumer/reset protocol.
	 */
	if (err)
		panic("%s: IOMMU invalidation failed (%d); DMA remains owned",
		      dev_name(iommu->dev), err);
}

static int a133_flush(struct a133_iommu *iommu)
{
	/* Exact stock full-flush value; bit 6 is not set by stock. */
	writel(0x3003f, iommu->base + A133_FLUSH);
	return a133_poll(iommu, A133_FLUSH);
}

static void a133_invalidate(struct a133_iommu *iommu, u32 iova)
{
	int err;

	writel(iova, iommu->base + A133_TLB_ADDR);
	writel(GENMASK(31, 12), iommu->base + A133_TLB_MASK);
	writel(1, iommu->base + A133_TLB_TRIGGER);
	err = a133_poll(iommu, A133_TLB_TRIGGER);
	a133_timeout(iommu, err);
	writel(iova, iommu->base + A133_PTW_ADDR);
	writel(1, iommu->base + A133_PTW_TRIGGER);
	a133_timeout(iommu, a133_poll(iommu, A133_PTW_TRIGGER));
}

static void a133_sync(struct iommu_domain *domain)
{
	struct a133_domain *d = to_a133(domain);
	struct a133_iommu *iommu = d->iommu;
	unsigned long flags;

	spin_lock_irqsave(&iommu->lock, flags);
	if (iommu->installed == d) {
		dma_wmb();
		a133_timeout(iommu, a133_flush(iommu));
	}
	spin_unlock_irqrestore(&iommu->lock, flags);
}

static void a133_iotlb_sync(struct iommu_domain *domain,
			    struct iommu_iotlb_gather *gather)
{
	a133_sync(domain);
}

static int a133_sync_map(struct iommu_domain *domain, unsigned long iova,
			 size_t size)
{
	struct a133_domain *d = to_a133(domain);
	struct a133_iommu *iommu = d->iommu;
	unsigned long flags;

	spin_lock_irqsave(&iommu->lock, flags);
	if (iommu->installed == d) {
		dma_wmb();
		writel(0, iommu->base + A133_GATING);
		/* Match stock's boundary and next-directory invalidations. */
		a133_invalidate(iommu, iova);
		a133_invalidate(iommu, iova + SZ_4K);
		a133_invalidate(iommu, iova + size);
		a133_invalidate(iommu, iova + size + SZ_4K);
		writel(iova + SZ_1M, iommu->base + A133_PTW_ADDR);
		writel(1, iommu->base + A133_PTW_TRIGGER);
		a133_timeout(iommu, a133_poll(iommu, A133_PTW_TRIGGER));
		writel(iova + size + SZ_1M, iommu->base + A133_PTW_ADDR);
		writel(1, iommu->base + A133_PTW_TRIGGER);
		a133_timeout(iommu, a133_poll(iommu, A133_PTW_TRIGGER));
		writel(1, iommu->base + A133_GATING);
	}
	spin_unlock_irqrestore(&iommu->lock, flags);
	return 0;
}

static int a133_map(struct iommu_domain *domain, unsigned long iova,
		    phys_addr_t pa, size_t size, size_t count, int prot,
		    gfp_t gfp, size_t *mapped)
{
	struct a133_domain *d = to_a133(domain);
	unsigned int di = iova >> 20, pi = (iova >> 12) & 255;
	unsigned long flags;
	int ret = 0;

	if (size != SZ_4K || !count || iova > U32_MAX || pa > U32_MAX ||
	    !IS_ALIGNED(iova | pa, SZ_4K))
		return -EINVAL;
	spin_lock_irqsave(&d->iommu->lock, flags);
	if (!d->pt[di]) {
		d->pt[di] = dma_pool_zalloc(d->iommu->pool, GFP_ATOMIC,
					  &d->pt_dma[di]);
		if (!d->pt[di]) {
			ret = -ENOMEM;
			goto out;
		}
		dma_wmb();
		WRITE_ONCE(d->dt[di], d->pt_dma[di] | 1);
	}
	if (d->pt[di][pi] & A133_VALID) {
		ret = -EEXIST;
		goto out;
	}
	WRITE_ONCE(d->pt[di][pi], a133_pte(pa, prot));
	*mapped = SZ_4K;
out:
	spin_unlock_irqrestore(&d->iommu->lock, flags);
	return ret;
}

static size_t a133_unmap(struct iommu_domain *domain, unsigned long iova,
			 size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	struct a133_domain *d = to_a133(domain);
	unsigned int di = iova >> 20, pi = (iova >> 12) & 255;
	unsigned long flags;
	size_t unmapped = 0;

	if (iova > U32_MAX || size != SZ_4K || !count ||
	    !IS_ALIGNED(iova, SZ_4K))
		return 0;
	spin_lock_irqsave(&d->iommu->lock, flags);
	if (d->pt[di] && (d->pt[di][pi] & A133_VALID)) {
		WRITE_ONCE(d->pt[di][pi], 0);
		unmapped = SZ_4K;
	}
	spin_unlock_irqrestore(&d->iommu->lock, flags);
	if (unmapped)
		iommu_iotlb_gather_add_range(gather, iova, unmapped);
	return unmapped;
}

static phys_addr_t a133_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct a133_domain *d = to_a133(domain);
	unsigned long flags;
	u32 pte = 0;

	if (iova > U32_MAX)
		return 0;
	spin_lock_irqsave(&d->iommu->lock, flags);
	if (d->pt[iova >> 20])
		pte = d->pt[iova >> 20][(iova >> 12) & 255];
	spin_unlock_irqrestore(&d->iommu->lock, flags);
	return pte & A133_VALID ? (pte & GENMASK(31, 12)) | (iova & 4095) : 0;
}

static struct iommu_domain *a133_alloc(struct device *dev)
{
	struct a133_client *c = dev_iommu_priv_get(dev);
	struct a133_domain *d;

	d = kvzalloc_obj(*d);
	if (!d)
		return ERR_PTR(-ENOMEM);
	d->iommu = c->iommu;
	d->dt = dma_alloc_coherent(c->iommu->dev, A133_DT_SIZE, &d->dt_dma,
				   GFP_KERNEL);
	if (!d->dt) {
		kvfree(d);
		return ERR_PTR(-ENOMEM);
	}
	d->domain.pgsize_bitmap = SZ_4K;
	d->domain.geometry.aperture_end = U32_MAX;
	d->domain.geometry.force_aperture = true;
	return &d->domain;
}

static void a133_free(struct iommu_domain *domain)
{
	struct a133_domain *d = to_a133(domain);
	unsigned int i;

	/* Core releases all clients before freeing an installed domain. */
	if (WARN_ON(d->iommu->installed == d))
		return;
	for (i = 0; i < ARRAY_SIZE(d->pt); i++)
		if (d->pt[i])
			dma_pool_free(d->iommu->pool, d->pt[i], d->pt_dma[i]);
	dma_free_coherent(d->iommu->dev, A133_DT_SIZE, d->dt, d->dt_dma);
	kvfree(d);
}

static int a133_attach(struct iommu_domain *domain, struct device *dev,
		       struct iommu_domain *old)
{
	struct a133_client *c = dev_iommu_priv_get(dev);
	struct a133_iommu *iommu = c->iommu;
	struct a133_domain *d = to_a133(domain);
	unsigned long flags;
	int ret;

	if (d->iommu != iommu)
		return -EINVAL;
	spin_lock_irqsave(&iommu->lock, flags);
	/* A singleton group shares one root. First callback switches every
	 * attached master; later callbacks only add newly attached master IDs.
	 * Both roots stay allocated through the core's group transaction.
	 */
	if (iommu->installed != d) {
		dma_wmb();
		writel(d->dt_dma, iommu->base + A133_TTB);
		ret = a133_flush(iommu);
		if (ret && !iommu->installed) {
			/* All masters are still bypassed; no page-table ownership
			 * escaped. Restore the permanent root before this domain
			 * can be freed; a later attach must flush before translation.
			 */
			writel(iommu->empty_dma, iommu->base + A133_TTB);
			spin_unlock_irqrestore(&iommu->lock, flags);
			return ret;
		}
		a133_timeout(iommu, ret);
		iommu->installed = d;
	}
	iommu->attached |= c->masters;
	writel(A133_ALL & ~iommu->attached, iommu->base + A133_BYPASS);
	spin_unlock_irqrestore(&iommu->lock, flags);
	return 0;
}

static int a133_identity_attach(struct iommu_domain *domain, struct device *dev,
				struct iommu_domain *old)
{
	struct a133_client *c = dev_iommu_priv_get(dev);
	struct a133_iommu *iommu = c->iommu;
	unsigned long flags;

	spin_lock_irqsave(&iommu->lock, flags);
	/* Identity is a deliberate group transition, never error recovery. */
	writel(A133_ALL, iommu->base + A133_BYPASS);
	writel(iommu->empty_dma, iommu->base + A133_TTB);
	a133_timeout(iommu, a133_flush(iommu));
	iommu->installed = NULL;
	iommu->attached |= c->masters;
	spin_unlock_irqrestore(&iommu->lock, flags);
	return 0;
}

static struct iommu_domain a133_identity = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev = a133_identity_attach,
	},
};

static struct iommu_device *a133_probe_device(struct device *dev)
{
	struct iommu_fwspec *spec = dev_iommu_fwspec_get(dev);
	struct platform_device *pdev;
	struct a133_iommu *iommu;
	struct a133_client *c;
	unsigned long flags;
	unsigned int i;
	u32 mask = 0;

	for (i = 0; i < spec->num_ids; i++) {
		if (spec->ids[i] >= A133_MASTERS || (mask & BIT(spec->ids[i])))
			return ERR_PTR(-EINVAL);
		mask |= BIT(spec->ids[i]);
	}
	if (!mask)
		return ERR_PTR(-EINVAL);
	pdev = of_find_device_by_node(to_of_node(spec->iommu_fwnode));
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	iommu = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	if (!iommu)
		return ERR_PTR(-EPROBE_DEFER);
	c = kzalloc_obj(*c);
	if (!c)
		return ERR_PTR(-ENOMEM);
	spin_lock_irqsave(&iommu->lock, flags);
	if (mask & iommu->owned) {
		spin_unlock_irqrestore(&iommu->lock, flags);
		kfree(c);
		return ERR_PTR(-EBUSY);
	}
	c->masters = mask;
	c->iommu = iommu;
	iommu->owned |= mask;
	spin_unlock_irqrestore(&iommu->lock, flags);
	dev_iommu_priv_set(dev, c);
	return &iommu->iommu;
}

static void a133_release_device(struct device *dev)
{
	struct a133_client *c = dev_iommu_priv_get(dev);
	struct a133_iommu *iommu = c->iommu;
	unsigned long flags;

	/* The consumer must have stopped DMA before device removal. */
	spin_lock_irqsave(&iommu->lock, flags);
	iommu->attached &= ~c->masters;
	iommu->owned &= ~c->masters;
	writel(iommu->installed ? A133_ALL & ~iommu->attached : A133_ALL,
	       iommu->base + A133_BYPASS);
	if (!iommu->attached)
		writel(iommu->empty_dma, iommu->base + A133_TTB);
	a133_timeout(iommu, a133_flush(iommu));
	if (!iommu->attached)
		iommu->installed = NULL;
	spin_unlock_irqrestore(&iommu->lock, flags);
	dev_iommu_priv_set(dev, NULL);
	kfree(c);
}

static int a133_xlate(struct device *dev, const struct of_phandle_args *args)
{
	if (args->args_count != 1 || args->args[0] >= A133_MASTERS)
		return -EINVAL;
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static const struct iommu_ops a133_ops = {
	.identity_domain = &a133_identity,
	.device_group = generic_single_device_group,
	.domain_alloc_paging = a133_alloc,
	.of_xlate = a133_xlate,
	.probe_device = a133_probe_device,
	.release_device = a133_release_device,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev = a133_attach,
		.map_pages = a133_map,
		.unmap_pages = a133_unmap,
		.iova_to_phys = a133_iova_to_phys,
		.flush_iotlb_all = a133_sync,
		.iotlb_sync = a133_iotlb_sync,
		.iotlb_sync_map = a133_sync_map,
		.free = a133_free,
	},
};

static irqreturn_t a133_irq(int irq, void *data)
{
	struct a133_iommu *iommu = data;
	u32 status = readl(iommu->base + A133_INT_STATUS);

	if (!(status & 0x3003f))
		return IRQ_NONE;
	/* Keep mappings and masters unchanged. Recovering faulted bus requests
	 * requires the consumer to own the stop/reset operation.
	 */
	panic("%s: IOMMU fault status %#x; DMA remains owned",
	      dev_name(iommu->dev), status);
}

static int a133_probe(struct platform_device *pdev)
{
	struct a133_iommu *iommu;
	int irq, ret;

	iommu = devm_kzalloc(&pdev->dev, sizeof(*iommu), GFP_KERNEL);
	if (!iommu)
		return -ENOMEM;
	iommu->dev = &pdev->dev;
	spin_lock_init(&iommu->lock);
	iommu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(iommu->base))
		return PTR_ERR(iommu->base);
	iommu->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(iommu->clk))
		return PTR_ERR(iommu->clk);
	/* Do not steal a live firmware page-table root or its display masters. */
	if ((readl(iommu->base + A133_ENABLE) & 1) &&
	    (readl(iommu->base + A133_BYPASS) & A133_ALL) != A133_ALL)
		return dev_err_probe(&pdev->dev, -EBUSY, "firmware translation active\n");
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	iommu->empty = dmam_alloc_coherent(&pdev->dev, A133_DT_SIZE,
					 &iommu->empty_dma, GFP_KERNEL);
	if (!iommu->empty)
		return -ENOMEM;
	iommu->pool = dma_pool_create(dev_name(&pdev->dev), &pdev->dev,
				     A133_PT_SIZE, A133_PT_SIZE, 0);
	if (!iommu->pool)
		return -ENOMEM;
	writel(A133_ALL, iommu->base + A133_BYPASS);
	dma_wmb();
	writel(iommu->empty_dma, iommu->base + A133_TTB);
	writel(0x5f, iommu->base + A133_PREFETCH);
	writel(0, iommu->base + A133_MODE);
	ret = a133_flush(iommu);
	if (ret)
		goto free_pool;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto free_pool;
	}
	/* Clear inherited status before installing an active handler. */
	writel(0, iommu->base + A133_INT_ENABLE);
	writel(0x3003f, iommu->base + A133_INT_CLEAR);
	ret = devm_request_irq(&pdev->dev, irq, a133_irq, 0, dev_name(&pdev->dev), iommu);
	if (ret)
		goto free_pool;
	writel(0x3003f, iommu->base + A133_INT_CLEAR);
	writel(0x3003f, iommu->base + A133_INT_ENABLE);
	writel(1, iommu->base + A133_GATING);
	writel(1, iommu->base + A133_ENABLE);
	ret = iommu_device_sysfs_add(&iommu->iommu, &pdev->dev, NULL,
				     dev_name(&pdev->dev));
	if (ret)
		goto disable;
	platform_set_drvdata(pdev, iommu);
	ret = iommu_device_register(&iommu->iommu, &a133_ops, &pdev->dev);
	if (!ret)
		return 0;
	platform_set_drvdata(pdev, NULL);
	iommu_device_sysfs_remove(&iommu->iommu);
disable:
	writel(0, iommu->base + A133_INT_ENABLE);
	writel(0, iommu->base + A133_ENABLE);
free_pool:
	dma_pool_destroy(iommu->pool);
	return ret;
}

static const struct of_device_id a133_match[] = {
	{ .compatible = "allwinner,sun50i-a133-iommu" },
	{ }
};

static struct platform_driver a133_driver = {
	.driver = {
		.name = "sun50i-a133-iommu",
		.of_match_table = a133_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver_probe(a133_driver, a133_probe);
MODULE_DESCRIPTION("Allwinner A133 IOMMU driver");
MODULE_LICENSE("GPL");

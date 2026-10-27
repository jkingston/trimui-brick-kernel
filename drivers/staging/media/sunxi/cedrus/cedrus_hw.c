// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/sunxi/sunxi_sram.h>
#include <linux/sun50i-a133-ve-domain.h>
#include <linux/sun50i-a133-iommu.h>

#include <media/videobuf2-core.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"
#include "cedrus_job.h"
#include "cedrus_enc.h"

int cedrus_engine_enable(struct cedrus_ctx *ctx)
{
	u32 reg = 0;

	/*
	 * FIXME: This is only valid on 32-bits DDR's, we should test
	 * it on the A13/A33.
	 */
	reg |= VE_MODE_REC_WR_MODE_2MB;
	reg |= VE_MODE_DDR_MODE_BW_128;

	switch (ctx->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_MPEG2_SLICE:
		reg |= VE_MODE_DEC_MPEG;
		break;

	/* H.264 and VP8 both use the same decoding mode bit. */
	case V4L2_PIX_FMT_H264_SLICE:
	case V4L2_PIX_FMT_VP8_FRAME:
		reg |= VE_MODE_DEC_H264;
		break;

	case V4L2_PIX_FMT_HEVC_SLICE:
		reg |= VE_MODE_DEC_H265;
		break;

	default:
		return -EINVAL;
	}

	if (ctx->src_fmt.width == 4096)
		reg |= VE_MODE_PIC_WIDTH_IS_4096;
	if (ctx->src_fmt.width > 2048)
		reg |= VE_MODE_PIC_WIDTH_MORE_2048;

	cedrus_write(ctx->dev, VE_MODE, reg);

	return 0;
}

int cedrus_encoder_engine_enable(struct cedrus_ctx *ctx)
{
	u32 reg = VE_MODE_REC_WR_MODE_2MB | VE_MODE_DDR_MODE_BW_128 |
		  VE_MODE_DISABLED | VE_MODE_ENCODER;

	if (!ctx || !ctx->dev || !ctx->dev->base)
		return -EINVAL;
	if (ctx->src_fmt.width == 4096)
		reg |= VE_MODE_PIC_WIDTH_IS_4096;
	if (ctx->src_fmt.width > 2048)
		reg |= VE_MODE_PIC_WIDTH_MORE_2048;

	/* Stock enables both the ISP and shared AVC/JPEG interrupt routes. */
	cedrus_write(ctx->dev, VE_MODE, reg);
	return 0;
}

void cedrus_encoder_engine_disable(struct cedrus_dev *dev)
{
	u32 reg = cedrus_read(dev, VE_MODE);

	/* Match stock's route disable while preserving the top-level mode. */
	cedrus_write(dev, VE_MODE, reg & ~VE_MODE_ENCODER);
}

void cedrus_engine_disable(struct cedrus_dev *dev)
{
	cedrus_write(dev, VE_MODE, VE_MODE_DISABLED);
}

void cedrus_dst_format_set(struct cedrus_dev *dev,
			   struct v4l2_pix_format *fmt)
{
	unsigned int width = fmt->width;
	unsigned int height = fmt->height;
	u32 chroma_size;
	u32 reg;

	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
		chroma_size = ALIGN(width, 16) * ALIGN(height, 16) / 2;

		switch (fmt->pixelformat) {
		case V4L2_PIX_FMT_NV12:
			reg = VE_PRIMARY_OUT_FMT_NV12;
			break;
		case V4L2_PIX_FMT_NV21:
			reg = VE_PRIMARY_OUT_FMT_NV21;
			break;
		case V4L2_PIX_FMT_YUV420:
			reg = VE_PRIMARY_OUT_FMT_YU12;
			break;
		case V4L2_PIX_FMT_YVU420:
		default:
			reg = VE_PRIMARY_OUT_FMT_YV12;
			break;
		}
		cedrus_write(dev, VE_PRIMARY_OUT_FMT, reg);

		reg = chroma_size / 2;
		cedrus_write(dev, VE_PRIMARY_CHROMA_BUF_LEN, reg);

		reg = VE_PRIMARY_FB_LINE_STRIDE_LUMA(ALIGN(width, 16)) |
		      VE_PRIMARY_FB_LINE_STRIDE_CHROMA(ALIGN(width, 16) / 2);
		cedrus_write(dev, VE_PRIMARY_FB_LINE_STRIDE, reg);

		break;
	case V4L2_PIX_FMT_NV12_32L32:
	default:
		reg = VE_PRIMARY_OUT_FMT_TILED_32_NV12;
		cedrus_write(dev, VE_PRIMARY_OUT_FMT, reg);

		reg = VE_SECONDARY_OUT_FMT_TILED_32_NV12;
		cedrus_write(dev, VE_CHROMA_BUF_LEN, reg);

		break;
	}
}

bool cedrus_iommu_fault_pending(struct cedrus_dev *dev)
{
	if (atomic_read(&dev->iommu_fault_state) != CEDRUS_IOMMU_READY)
		return true;

	return dev->requires_iommu &&
		sun50i_a133_iommu_fault_pending(dev->dev);
}

int cedrus_hw_acquire(struct cedrus_dev *dev)
{
	int ret;

	if (!dev->ve_domain)
		return 0;

	ret = sun50i_a133_ve_domain_acquire(dev->ve_domain, dev->dev);
	if (!ret)
		atomic_set(&dev->ve_domain_owned, 1);

	return ret;
}

void cedrus_hw_release(struct cedrus_dev *dev)
{
	if (!dev->ve_domain || atomic_cmpxchg(&dev->ve_domain_owned, 1, 0) != 1)
		return;

	sun50i_a133_ve_domain_release(dev->ve_domain, dev->dev);
}

int cedrus_hw_reset(struct cedrus_dev *dev)
{
	if (dev->ve_domain)
		return sun50i_a133_ve_domain_reset(dev->ve_domain, dev->dev);

	return reset_control_reset(dev->rstc);
}

int cedrus_hw_assert_reset(struct cedrus_dev *dev)
{
	if (dev->ve_domain)
		return sun50i_a133_ve_domain_assert(dev->ve_domain, dev->dev);

	return reset_control_assert(dev->rstc);
}

static irqreturn_t cedrus_irq(int irq, void *data)
{
	struct cedrus_dev *dev = data;
	struct cedrus_ctx *ctx;
	enum vb2_buffer_state state;
	enum cedrus_irq_status status;
	unsigned long deadline = dev->watchdog_work.timer.expires;
	unsigned long flags;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	ctx = dev->job_ctx;
	if (unlikely(cedrus_iommu_fault_pending(dev)) || !ctx ||
	    !dev->job_running || ctx != v4l2_m2m_get_curr_priv(dev->m2m_dev)) {
		spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
		return IRQ_NONE;
	}
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);

	if (!cancel_delayed_work(&dev->watchdog_work))
		return IRQ_NONE;
	if (!ctx->is_encoder) {
		if (unlikely(cedrus_iommu_fault_pending(dev)))
			return IRQ_NONE;
		status = ctx->current_codec->irq_status(ctx);
		if (status == CEDRUS_IRQ_NONE) {
			unsigned long now = jiffies;

			schedule_delayed_work(&dev->watchdog_work,
					      time_after(deadline, now) ? deadline - now : 1);
			return IRQ_NONE;
		}
		ctx->current_codec->irq_disable(ctx);
		ctx->current_codec->irq_clear(ctx);
		state = status == CEDRUS_IRQ_ERROR ? VB2_BUF_STATE_ERROR :
			VB2_BUF_STATE_DONE;
		cedrus_job_finish(ctx, state, false);
		return IRQ_HANDLED;
	}

	/*
	 * Keep fault publication excluded through terminal metadata consumption,
	 * reference promotion and generic job retirement.  Whichever path acquires
	 * this lock first owns the terminal outcome.
	 */
	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	if (unlikely(cedrus_iommu_fault_pending(dev)) || dev->job_ctx != ctx ||
	    !dev->job_running || ctx != v4l2_m2m_get_curr_priv(dev->m2m_dev)) {
		spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
		return IRQ_NONE;
	}
	switch (dev->enc_ops->irq_status(ctx)) {
	case CEDRUS_ENC_IRQ_OK:
		status = CEDRUS_IRQ_OK;
		break;
	case CEDRUS_ENC_IRQ_ERROR:
		status = CEDRUS_IRQ_ERROR;
		break;
	default:
		status = CEDRUS_IRQ_NONE;
		break;
	}

	if (status == CEDRUS_IRQ_NONE) {
		unsigned long now = jiffies;

		schedule_delayed_work(&dev->watchdog_work,
				      time_after(deadline, now) ? deadline - now : 1);
		spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
		return IRQ_NONE;
	}

	dev->enc_ops->irq_ack(ctx);
	if (status == CEDRUS_IRQ_ERROR) {
		schedule_work(&dev->encoder_error_work);
		spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
		return IRQ_HANDLED;
	}

	state = status == CEDRUS_IRQ_ERROR ? VB2_BUF_STATE_ERROR :
		VB2_BUF_STATE_DONE;
	cedrus_job_finish_locked(ctx, state, false);
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);

	return IRQ_HANDLED;
}

void cedrus_watchdog(struct work_struct *work)
{
	struct cedrus_dev *dev;
	struct cedrus_ctx *ctx;
	unsigned long flags;
	int ret;

	dev = container_of(to_delayed_work(work),
			   struct cedrus_dev, watchdog_work);
	mutex_lock(&dev->iommu_fault_mutex);
	if (unlikely(cedrus_iommu_fault_pending(dev)))
		goto out;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	ctx = dev->job_ctx;
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
	if (!ctx)
		goto out;

	v4l2_err(&dev->v4l2_dev, "frame processing timed out!\n");
	if (ctx->is_encoder) {
		ret = cedrus_enc_abort(dev->enc_ops, ctx,
				       CEDRUS_ENC_ABORT_WATCHDOG);
		if (ret)
			v4l2_warn(&dev->v4l2_dev,
				  "encoder timeout abort returned %d\n", ret);
	}
	ret = cedrus_hw_reset(dev);
	if (ret && dev->requires_iommu)
		panic("%s: VE reset failed (%d); DMA buffers remain owned",
		      dev_name(dev->dev), ret);
	if (!ret && ctx->is_encoder) {
		ret = cedrus_enc_reset_complete(dev->enc_ops, ctx);
		if (ret)
			v4l2_warn(&dev->v4l2_dev,
				  "encoder reset completion returned %d\n", ret);
	}

	cedrus_job_finish(ctx, VB2_BUF_STATE_ERROR, false);
out:
	mutex_unlock(&dev->iommu_fault_mutex);
}

void cedrus_encoder_error(struct work_struct *work)
{
	struct cedrus_dev *dev = container_of(work, struct cedrus_dev,
					      encoder_error_work);
	struct cedrus_ctx *ctx;
	unsigned long flags;
	int ret;

	mutex_lock(&dev->iommu_fault_mutex);
	if (unlikely(cedrus_iommu_fault_pending(dev)))
		goto out;

	disable_irq(dev->irq);
	if (unlikely(cedrus_iommu_fault_pending(dev)))
		goto enable;
	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	ctx = dev->job_ctx;
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
	if (!ctx || !ctx->is_encoder)
		goto enable;

	ret = cedrus_enc_abort(dev->enc_ops, ctx, CEDRUS_ENC_ABORT_IRQ_ERROR);
	if (ret)
		v4l2_warn(&dev->v4l2_dev,
			  "encoder error abort returned %d\n", ret);
	ret = cedrus_hw_reset(dev);
	if (ret && dev->requires_iommu)
		panic("%s: VE reset failed (%d); DMA buffers remain owned",
		      dev_name(dev->dev), ret);
	if (!ret) {
		ret = cedrus_enc_reset_complete(dev->enc_ops, ctx);
		if (ret)
			v4l2_warn(&dev->v4l2_dev,
				  "encoder reset completion returned %d\n", ret);
	}
	cedrus_job_finish(ctx, VB2_BUF_STATE_ERROR, false);

enable:
	enable_irq(dev->irq);
out:
	mutex_unlock(&dev->iommu_fault_mutex);
}

static void cedrus_iommu_fault(struct device *device,
			       const struct sun50i_a133_iommu_fault *fault,
			       void *data)
{
	struct cedrus_dev *dev = data;
	unsigned long flags;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	if (atomic_cmpxchg(&dev->iommu_fault_state, CEDRUS_IOMMU_READY,
			   CEDRUS_IOMMU_FAULT_PENDING) != CEDRUS_IOMMU_READY) {
		spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);

	v4l2_err(&dev->v4l2_dev,
		 "IOMMU fault at %pad (master %u, %s%s, status %#x)\n",
		 &fault->iova, fault->master,
		 fault->flags == IOMMU_FAULT_WRITE ? "write" : "read",
		 fault->flags_valid ? "" : " fallback",
		 fault->status);
	schedule_work(&dev->iommu_fault_work);
}

static void cedrus_iommu_fault_work(struct work_struct *work)
{
	struct cedrus_dev *dev = container_of(work, struct cedrus_dev,
					      iommu_fault_work);
	struct cedrus_ctx *ctx, *finish_ctx;
	unsigned long flags;
	int backend_ret = 0;
	bool recovered = false;
	int ret;

	/* The provider already holds the VE IOMMU master in reset. Synchronize
	 * both completion paths, then stop the engine before releasing it.
	 */
	cancel_work_sync(&dev->encoder_error_work);
	cancel_delayed_work_sync(&dev->watchdog_work);
	mutex_lock(&dev->iommu_fault_mutex);
	disable_irq(dev->irq);

	/* Snapshot software ownership before resetting the faulting DMA master. */
	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	ctx = dev->job_ctx;
	finish_ctx = ctx ?: v4l2_m2m_get_curr_priv(dev->m2m_dev);
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
	if (ctx && ctx->is_encoder) {
		backend_ret = cedrus_enc_abort(dev->enc_ops, ctx,
					       CEDRUS_ENC_ABORT_IOMMU_FAULT);
		if (backend_ret)
			v4l2_warn(&dev->v4l2_dev,
				  "encoder IOMMU abort returned %d\n",
				  backend_ret);
	}

	if (dev->ve_domain && !atomic_read(&dev->ve_domain_owned)) {
		ret = cedrus_hw_acquire(dev);
		if (ret) {
			v4l2_err(&dev->v4l2_dev,
				 "VE domain acquisition failed after IOMMU fault: %d\n",
				 ret);
			atomic_set(&dev->iommu_fault_state,
				   CEDRUS_IOMMU_FAULT_DEAD);
			goto complete;
		}
	}

	ret = cedrus_hw_reset(dev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "VE reset failed after IOMMU fault: %d\n", ret);
		atomic_set(&dev->iommu_fault_state, CEDRUS_IOMMU_FAULT_DEAD);
	} else {
		if (ctx && ctx->is_encoder) {
			ret = cedrus_enc_reset_complete(dev->enc_ops, ctx);
			if (ret) {
				v4l2_err(&dev->v4l2_dev,
					 "encoder reset completion failed: %d\n",
					 ret);
				backend_ret = ret;
			}
		}
		ret = sun50i_a133_iommu_fault_complete(dev->dev);
		if (ret) {
			v4l2_err(&dev->v4l2_dev,
				 "IOMMU fault retirement failed: %d\n", ret);
			atomic_set(&dev->iommu_fault_state,
				   CEDRUS_IOMMU_FAULT_DEAD);
		} else if (!backend_ret) {
			recovered = true;
		} else {
			atomic_set(&dev->iommu_fault_state,
				   CEDRUS_IOMMU_FAULT_DEAD);
		}
	}

	if (recovered)
		atomic_set(&dev->iommu_fault_state, CEDRUS_IOMMU_READY);

complete:
	if (!finish_ctx ||
	    !cedrus_job_finish(finish_ctx, VB2_BUF_STATE_ERROR, true))
		cedrus_hw_release(dev);
	if (recovered)
		enable_irq(dev->irq);
	mutex_unlock(&dev->iommu_fault_mutex);
}

int cedrus_iommu_fault_init(struct cedrus_dev *dev)
{
	if (!dev->requires_iommu)
		return 0;

	atomic_set(&dev->iommu_fault_state, CEDRUS_IOMMU_READY);
	INIT_WORK(&dev->iommu_fault_work, cedrus_iommu_fault_work);
	return sun50i_a133_iommu_register_fault_handler(dev->dev,
							cedrus_iommu_fault, dev);
}

static int cedrus_iommu_fault_retire_asserted(struct cedrus_dev *dev)
{
	int ret;

	if (atomic_read(&dev->iommu_fault_state) == CEDRUS_IOMMU_READY)
		return 0;

	/* A failed reset pulse or invalidation leaves the provider master held.
	 * Suspend and unbind get one final containment attempt with VE reset held.
	 */
	if (dev->ve_domain && !atomic_read(&dev->ve_domain_owned)) {
		ret = cedrus_hw_acquire(dev);
		if (ret)
			return ret;
	}

	ret = cedrus_hw_assert_reset(dev);
	if (ret)
		return ret;
	ret = sun50i_a133_iommu_fault_complete(dev->dev);
	if (!ret)
		atomic_set(&dev->iommu_fault_state, CEDRUS_IOMMU_READY);
	return ret;
}

void cedrus_iommu_fault_exit(struct cedrus_dev *dev)
{
	int ret;

	if (!dev->requires_iommu)
		return;

	sun50i_a133_iommu_unregister_fault_handler(dev->dev);
	flush_work(&dev->iommu_fault_work);
	ret = cedrus_iommu_fault_retire_asserted(dev);
	if (ret)
		v4l2_err(&dev->v4l2_dev,
			 "IOMMU master remains quarantined during remove: %d\n", ret);
}

int cedrus_iommu_fault_flush(struct cedrus_dev *dev)
{
	if (!dev->requires_iommu)
		return 0;

	flush_work(&dev->iommu_fault_work);
	return atomic_read(&dev->iommu_fault_state) == CEDRUS_IOMMU_READY ?
		0 : -EIO;
}

int cedrus_hw_suspend(struct device *device)
{
	struct cedrus_dev *dev = dev_get_drvdata(device);

	if (dev->ve_domain)
		return 0;

	clk_disable_unprepare(dev->ram_clk);
	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->ahb_clk);

	reset_control_assert(dev->rstc);

	return 0;
}

int cedrus_hw_resume(struct device *device)
{
	struct cedrus_dev *dev = dev_get_drvdata(device);
	int ret;

	if (dev->ve_domain)
		return 0;

	ret = cedrus_hw_reset(dev);
	if (ret) {
		dev_err(dev->dev, "Failed to apply reset\n");

		return ret;
	}

	ret = clk_prepare_enable(dev->ahb_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable AHB clock\n");

		goto err_rst;
	}

	ret = clk_prepare_enable(dev->mod_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable MOD clock\n");

		goto err_ahb_clk;
	}

	ret = clk_prepare_enable(dev->ram_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable RAM clock\n");

		goto err_mod_clk;
	}

	return 0;

err_mod_clk:
	clk_disable_unprepare(dev->mod_clk);
err_ahb_clk:
	clk_disable_unprepare(dev->ahb_clk);
err_rst:
	reset_control_assert(dev->rstc);

	return ret;
}

int cedrus_hw_probe(struct cedrus_dev *dev)
{
	const struct cedrus_variant *variant;
	int irq_dec;
	int ret;

	variant = of_device_get_match_data(dev->dev);
	if (!variant)
		return -EINVAL;

	dev->capabilities = variant->capabilities;
	dev->requires_iommu = variant->requires_iommu;
	atomic_set(&dev->ve_domain_owned, 0);
#if IS_ENABLED(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE)
	if (variant->enc_ops) {
#if IS_ENABLED(CONFIG_VIDEO_SUNXI_CEDRUS_A133_ENCODER_QUALIFICATION)
		const char *qualification;

		ret = of_property_read_string(dev->dev->of_node,
					      "allwinner,encoder-qualification",
					      &qualification);
		if (ret || strcmp(qualification,
				  "bounded-320x240-h264-qp30-jpeg-q95"))
			return dev_err_probe(dev->dev, -EPERM,
					     "missing encoder qualification marker\n");
#endif
		ret = cedrus_enc_backend_validate(variant->enc_ops);
		if (ret)
			return dev_err_probe(dev->dev, ret,
					     "incomplete encoder backend\n");
		dev->enc_ops = variant->enc_ops;
	}
#endif
	if (variant->requires_iommu) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(dev->dev);

		/* Identity/bypass must not silently replace the A133 DMA contract. */
		if (!domain || !iommu_is_dma_domain(domain))
			return dev_err_probe(dev->dev, -EINVAL,
					     "A133 requires a translated DMA domain\n");
		if (of_property_count_u32_elems(dev->dev->of_node, "allwinner,sram") != 4)
			return dev_err_probe(dev->dev, -EINVAL,
					     "A133 requires both SRAM selectors\n");
	}

	irq_dec = platform_get_irq(dev->pdev, 0);
	if (irq_dec <= 0)
		return irq_dec;
	dev->irq = irq_dec;
	ret = devm_request_irq(dev->dev, irq_dec, cedrus_irq,
			       0, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request IRQ\n");

		return ret;
	}

	ret = of_reserved_mem_device_init(dev->dev);
	if (ret && ret != -ENODEV) {
		dev_err(dev->dev, "Failed to reserve memory\n");

		return ret;
	}

	ret = sunxi_sram_claim(dev->dev);
	if (ret) {
		dev_err(dev->dev, "Failed to claim SRAM\n");

		goto err_mem;
	}

	if (variant->shared_domain) {
		dev->ve_domain = devm_sun50i_a133_ve_domain_get(dev->dev);
		if (IS_ERR(dev->ve_domain)) {
			ret = PTR_ERR(dev->ve_domain);
			goto err_sram;
		}
		goto map_registers;
	}

	dev->ahb_clk = devm_clk_get(dev->dev, "ahb");
	if (IS_ERR(dev->ahb_clk)) {
		dev_err(dev->dev, "Failed to get AHB clock\n");

		ret = PTR_ERR(dev->ahb_clk);
		goto err_sram;
	}

	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk)) {
		dev_err(dev->dev, "Failed to get MOD clock\n");

		ret = PTR_ERR(dev->mod_clk);
		goto err_sram;
	}

	dev->ram_clk = devm_clk_get(dev->dev, "ram");
	if (IS_ERR(dev->ram_clk)) {
		dev_err(dev->dev, "Failed to get RAM clock\n");

		ret = PTR_ERR(dev->ram_clk);
		goto err_sram;
	}

	dev->rstc = devm_reset_control_get(dev->dev, NULL);
	if (IS_ERR(dev->rstc)) {
		dev_err(dev->dev, "Failed to get reset control\n");

		ret = PTR_ERR(dev->rstc);
		goto err_sram;
	}

map_registers:
	dev->base = devm_platform_ioremap_resource(dev->pdev, 0);
	if (IS_ERR(dev->base)) {
		dev_err(dev->dev, "Failed to map registers\n");

		ret = PTR_ERR(dev->base);
		goto err_sram;
	}

	if (!variant->shared_domain) {
		ret = clk_set_rate(dev->mod_clk, variant->mod_rate);
		if (ret) {
			dev_err(dev->dev, "Failed to set clock rate\n");

			goto err_sram;
		}
	}

	pm_runtime_enable(dev->dev);
	if (!pm_runtime_enabled(dev->dev)) {
		ret = cedrus_hw_resume(dev->dev);
		if (ret)
			goto err_pm;
	}

	return 0;

err_pm:
	pm_runtime_disable(dev->dev);
err_sram:
	sunxi_sram_release(dev->dev);
err_mem:
	of_reserved_mem_device_release(dev->dev);

	return ret;
}

void cedrus_hw_remove(struct cedrus_dev *dev)
{
	pm_runtime_disable(dev->dev);
	if (!pm_runtime_status_suspended(dev->dev))
		cedrus_hw_suspend(dev->dev);
	cedrus_hw_release(dev);

	sunxi_sram_release(dev->dev);

	of_reserved_mem_device_release(dev->dev);
}

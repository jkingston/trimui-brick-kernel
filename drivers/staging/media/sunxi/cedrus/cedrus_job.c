// SPDX-License-Identifier: GPL-2.0
/* Shared Cedrus video-engine job ownership and cancellation. */

#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_enc.h"
#include "cedrus_enc_video.h"
#include "cedrus_hw.h"
#include "cedrus_job.h"

bool cedrus_job_begin(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	unsigned long flags;
	bool admitted = false;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	if (!cedrus_iommu_fault_pending(dev) && !dev->job_ctx &&
	    ctx == v4l2_m2m_get_curr_priv(dev->m2m_dev)) {
		dev->job_ctx = ctx;
		dev->job_running = false;
		admitted = true;
	}
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);

	return admitted;
}

bool cedrus_job_active(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	unsigned long flags;
	bool active;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	active = !cedrus_iommu_fault_pending(dev) && dev->job_ctx == ctx &&
		 ctx == v4l2_m2m_get_curr_priv(dev->m2m_dev);
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);

	return active;
}

void cedrus_job_mark_running(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	if (!cedrus_iommu_fault_pending(dev) && dev->job_ctx == ctx &&
	    ctx == v4l2_m2m_get_curr_priv(dev->m2m_dev))
		dev->job_running = true;
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
}

void cedrus_job_finish(struct cedrus_ctx *ctx, enum vb2_buffer_state state,
		       bool fault_owner)
{
	struct cedrus_dev *dev = ctx->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->iommu_completion_lock, flags);
	if ((fault_owner || !cedrus_iommu_fault_pending(dev)) &&
	    (dev->job_ctx == ctx || (fault_owner && !dev->job_ctx)) &&
	    ctx == v4l2_m2m_get_curr_priv(dev->m2m_dev)) {
		if (ctx->is_encoder)
			cedrus_enc_job_finish(ctx);
		dev->job_ctx = NULL;
		dev->job_running = false;
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev,
						 ctx->fh.m2m_ctx, state);
	}
	spin_unlock_irqrestore(&dev->iommu_completion_lock, flags);
}

void cedrus_job_abort(void *priv)
{
	struct cedrus_ctx *ctx = priv;
	struct cedrus_dev *dev = ctx->dev;
	bool active;
	int reset_ret;
	int ret = 0;

	/* STREAMOFF waits for job_finish(), so cancellation must be synchronous. */
	mutex_lock(&dev->iommu_fault_mutex);
	disable_irq(dev->irq);
	cancel_delayed_work_sync(&dev->watchdog_work);

	active = cedrus_job_active(ctx);
	if (!active)
		goto out;

	if (ctx->is_encoder) {
		ret = dev->enc_ops->abort(ctx);
	} else {
		ctx->current_codec->irq_disable(ctx);
		ctx->current_codec->irq_clear(ctx);
	}
	if (ret)
		v4l2_warn(&dev->v4l2_dev, "encoder abort returned %d; resetting VE\n",
			  ret);
	reset_ret = reset_control_reset(dev->rstc);

	if (reset_ret && dev->requires_iommu)
		panic("%s: VE abort reset failed (%d); DMA buffers remain owned",
		      dev_name(dev->dev), reset_ret);

	cedrus_job_finish(ctx, VB2_BUF_STATE_ERROR, false);
out:
	enable_irq(dev->irq);
	mutex_unlock(&dev->iommu_fault_mutex);
}

void cedrus_encoder_device_run(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;
	int ret;

	if (dev->requires_iommu)
		mutex_lock(&dev->iommu_fault_mutex);

	if (!cedrus_job_begin(ctx)) {
		if (dev->requires_iommu)
			mutex_unlock(&dev->iommu_fault_mutex);
		if (atomic_read(&dev->iommu_fault_state) == CEDRUS_IOMMU_FAULT_DEAD)
			cedrus_job_finish(ctx, VB2_BUF_STATE_ERROR, true);
		return;
	}

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_copy_metadata(src, dst);
	ret = dev->enc_ops->prepare_job(ctx);
	if (!ret && cedrus_job_active(ctx)) {
		schedule_delayed_work(&dev->watchdog_work, msecs_to_jiffies(2000));
		cedrus_job_mark_running(ctx);
		dev->enc_ops->trigger(ctx);
	}

	if (dev->requires_iommu)
		mutex_unlock(&dev->iommu_fault_mutex);
	if (ret)
		cedrus_job_finish(ctx, VB2_BUF_STATE_ERROR, false);
}

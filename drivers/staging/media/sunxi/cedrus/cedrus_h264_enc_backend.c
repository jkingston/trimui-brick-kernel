// SPDX-License-Identifier: GPL-2.0
/* Unexposed A133 H.264 encoder backend ownership. */

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

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
	struct cedrus_h264_enc_controls controls;
	u32 terminal_status;
	struct cedrus_a133_h264_allocation
		allocations[CEDRUS_A133_H264_ALLOCATION_COUNT];
};

#define CEDRUS_A133_H264_FIXED_QP	30
#define CEDRUS_A133_H264_FIXED_GOP	30
#define CEDRUS_A133_H264_STOCK_BITSTREAM_SIZE	(12 * 1024 * 1024)

static const struct cedrus_enc_format cedrus_a133_h264_formats[] = {
	{
		.pixelformat = V4L2_PIX_FMT_NV12,
		.direction = CEDRUS_ENC_FORMAT_RAW,
		.min_width = 320,
		.max_width = 320,
		.min_height = 240,
		.max_height = 240,
		.width_align = 8,
		.height_align = 8,
		.stride_align = 16,
		.buffer_align = 1,
	}, {
		.pixelformat = V4L2_PIX_FMT_YUV420,
		.direction = CEDRUS_ENC_FORMAT_RAW,
		.min_width = 320,
		.max_width = 320,
		.min_height = 240,
		.max_height = 240,
		.width_align = 8,
		.height_align = 8,
		.stride_align = 16,
		.buffer_align = 1,
	}, {
		.pixelformat = V4L2_PIX_FMT_H264,
		.direction = CEDRUS_ENC_FORMAT_CODED,
		.min_width = 320,
		.max_width = 320,
		.min_height = 240,
		.max_height = 240,
		.width_align = 8,
		.height_align = 8,
		.buffer_align = 256,
		/* Preserve the only measured stock capacity until bounded lower. */
		.min_sizeimage = CEDRUS_A133_H264_STOCK_BITSTREAM_SIZE,
	},
};

static const struct v4l2_ctrl_config cedrus_a133_h264_controls[] = {
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.name = "H.264 Profile",
		.type = V4L2_CTRL_TYPE_MENU,
		.min = 0,
		.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.def = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.menu_skip_mask = GENMASK_ULL(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH - 1, 0),
	}, {
		.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.name = "H.264 Level",
		.type = V4L2_CTRL_TYPE_MENU,
		.min = 0,
		.max = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
		.def = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
		.menu_skip_mask = GENMASK_ULL(V4L2_MPEG_VIDEO_H264_LEVEL_5_1 - 1, 0),
	}, {
		.id = V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
		.name = "H.264 Entropy Mode",
		.type = V4L2_CTRL_TYPE_MENU,
		.min = 0,
		.max = V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC,
		.def = V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC,
		.menu_skip_mask = BIT(V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC),
	}, {
		.id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
		.name = "Frame Level Rate Control Enable",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.min = 0,
		.max = 0,
		.step = 1,
		.def = 0,
	}, {
		.id = V4L2_CID_MPEG_VIDEO_B_FRAMES,
		.name = "Number of B-Frames",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = 0,
		.step = 1,
		.def = 0,
	}, {
		.id = V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
		.name = "H.264 I-Frame QP Value",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = CEDRUS_A133_H264_FIXED_QP,
		.max = CEDRUS_A133_H264_FIXED_QP,
		.step = 1,
		.def = CEDRUS_A133_H264_FIXED_QP,
	}, {
		.id = V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
		.name = "H.264 P-Frame QP Value",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = CEDRUS_A133_H264_FIXED_QP,
		.max = CEDRUS_A133_H264_FIXED_QP,
		.step = 1,
		.def = CEDRUS_A133_H264_FIXED_QP,
	},
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
	backend_ctx->controls = (struct cedrus_h264_enc_controls) {
		.profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.level = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
		.entropy_mode = V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC,
		.gop_size = CEDRUS_A133_H264_FIXED_GOP,
		.i_qp = CEDRUS_A133_H264_FIXED_QP,
		.p_qp = CEDRUS_A133_H264_FIXED_QP,
	};

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

static int cedrus_a133_h264_backend_s_ctrl(struct cedrus_ctx *ctx,
					   struct v4l2_ctrl *ctrl)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	struct cedrus_h264_enc_controls controls;
	struct cedrus_h264_enc_params params;
	unsigned long flags;
	int ret;

	if (!ctx || !ctx->enc_priv || !ctrl)
		return -EINVAL;
	backend_ctx = ctx->enc_priv;

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	controls = backend_ctx->controls;
	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		controls.profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		controls.level = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE:
		controls.entropy_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:
		controls.frame_rc_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		controls.b_frames = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:
		controls.i_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP:
		controls.p_qp = ctrl->val;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	ret = cedrus_h264_enc_controls_prepare(&controls, &params);
	if (!ret)
		backend_ctx->controls = controls;
out:
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	return ret;
}

static bool cedrus_a133_h264_backend_supports_pair(u32 raw_pixelformat,
						   u32 coded_pixelformat)
{
	return (raw_pixelformat == V4L2_PIX_FMT_NV12 ||
		raw_pixelformat == V4L2_PIX_FMT_YUV420) &&
		coded_pixelformat == V4L2_PIX_FMT_H264;
}

static int cedrus_a133_h264_backend_prepare_job(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	struct cedrus_a133_h264_job_request request;
	struct cedrus_h264_enc_params params;
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;
	dma_addr_t source;
	dma_addr_t bitstream;
	unsigned long flags;
	size_t source_size;
	size_t bitstream_size;
	bool idr;
	int ret;

	if (!ctx || !ctx->dev || !ctx->enc_priv)
		return -EINVAL;
	backend_ctx = ctx->enc_priv;
	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst)
		return -EINVAL;

	source = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	bitstream = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	source_size = vb2_get_plane_payload(&src->vb2_buf, 0);
	bitstream_size = vb2_plane_size(&dst->vb2_buf, 0);

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	ret = cedrus_h264_enc_controls_prepare(&backend_ctx->controls, &params);
	if (ret)
		goto out;
	/* A later IDR transition is not yet established by stock evidence. */
	idr = !backend_ctx->job.rotation.started;
	request = (struct cedrus_a133_h264_job_request) {
		.pixelformat = ctx->src_fmt.pixelformat,
		.width = ctx->src_fmt.width,
		.height = ctx->src_fmt.height,
		.bytesperline = ctx->src_fmt.bytesperline,
		.slice = {
			.frame_num = backend_ctx->job.rotation.next_frame_num,
			.pic_order_cnt_lsb =
				backend_ctx->job.rotation.next_frame_num * 2,
			.qp = idr ? params.qp : backend_ctx->controls.p_qp,
			.idr = idr,
		},
	};
	ret = cedrus_a133_h264_backend_job_prepare(&backend_ctx->job, &params,
						   &request, source,
						   source_size, bitstream,
						   bitstream_size);
out:
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	return ret;
}

static int cedrus_a133_h264_backend_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	unsigned long flags;
	int ret;

	if (!ctx || !ctx->enc_priv)
		return -EINVAL;
	backend_ctx = ctx->enc_priv;

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	ret = cedrus_a133_h264_backend_job_trigger(&backend_ctx->job,
						   ctx->dev->base);
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
	return ret;
}

static enum cedrus_enc_irq_status
cedrus_a133_h264_backend_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx;
	struct vb2_v4l2_buffer *dst;
	unsigned long flags;
	u32 produced_bits;
	u32 status;
	int ret;

	if (!ctx || !ctx->dev || !ctx->enc_priv)
		return CEDRUS_ENC_IRQ_ERROR;
	backend_ctx = ctx->enc_priv;
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!dst)
		return CEDRUS_ENC_IRQ_ERROR;

	/* Match stock terminal sampling: length first, then status, then W1C. */
	produced_bits = cedrus_a133_h264_bitstream_length(ctx->dev->base);
	status = cedrus_a133_h264_irq_read(ctx->dev->base);
	if (cedrus_a133_h264_enc_irq_status(status) == CEDRUS_ENC_IRQ_NONE)
		return CEDRUS_ENC_IRQ_NONE;

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	ret = cedrus_a133_h264_backend_job_complete(&backend_ctx->job,
						    vb2_plane_size(&dst->vb2_buf, 0),
						    status, produced_bits);
	backend_ctx->terminal_status = status;
	if (!ret) {
		vb2_set_plane_payload(&dst->vb2_buf, 0,
				      backend_ctx->job.completion.payload);
		dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				V4L2_BUF_FLAG_BFRAME);
		dst->flags |= backend_ctx->job.completion.keyframe ?
			V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
	}
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);

	return ret == -EAGAIN ? CEDRUS_ENC_IRQ_NONE :
		ret ? CEDRUS_ENC_IRQ_ERROR : CEDRUS_ENC_IRQ_OK;
}

static void cedrus_a133_h264_backend_irq_ack(struct cedrus_ctx *ctx)
{
	struct cedrus_a133_h264_backend_ctx *backend_ctx = ctx->enc_priv;
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&backend_ctx->job_lock, flags);
	status = backend_ctx->terminal_status;
	backend_ctx->terminal_status = 0;
	cedrus_a133_h264_irq_ack_status(ctx->dev->base, status);
	spin_unlock_irqrestore(&backend_ctx->job_lock, flags);
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
	.formats = cedrus_a133_h264_formats,
	.num_formats = ARRAY_SIZE(cedrus_a133_h264_formats),
	.controls = cedrus_a133_h264_controls,
	.num_controls = ARRAY_SIZE(cedrus_a133_h264_controls),
	.bind = cedrus_a133_h264_backend_bind,
	.ctx_init = cedrus_a133_h264_backend_ctx_init,
	.ctx_cleanup = cedrus_a133_h264_backend_ctx_cleanup,
	.s_ctrl = cedrus_a133_h264_backend_s_ctrl,
	.supports_pair = cedrus_a133_h264_backend_supports_pair,
	.start_streaming = cedrus_a133_h264_backend_allocate,
	.stop_streaming = cedrus_a133_h264_backend_release,
	.prepare_job = cedrus_a133_h264_backend_prepare_job,
	.trigger = cedrus_a133_h264_backend_trigger,
	.irq_status = cedrus_a133_h264_backend_irq_status,
	.irq_ack = cedrus_a133_h264_backend_irq_ack,
	.abort = cedrus_a133_h264_backend_abort,
	.reset_complete = cedrus_a133_h264_backend_ctx_reset_complete,
	.suspend = cedrus_a133_h264_backend_dev_suspend,
	.resume = cedrus_a133_h264_backend_dev_resume,
	.unbind = cedrus_a133_h264_backend_unbind,
};

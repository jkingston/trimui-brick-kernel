// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus encoder hardware-contract helpers
 *
 * The A133 register values below are intentionally limited to fields whose
 * encoding has been isolated by successful stock H.264 runs.  This file does
 * not program the VE and cannot make an incomplete backend visible to users.
 */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/overflow.h>

#include <linux/videodev2.h>

#include "cedrus_enc.h"
#include "cedrus_enc_regs.h"

#define CEDRUS_A133_ENC_DIMENSION_UNIT	8U
#define CEDRUS_A133_ENC_STRIDE_UNIT	16U
#define CEDRUS_A133_ENC_ADDR_UNIT	256U
#define CEDRUS_A133_ENC_MAX_DIMENSION	4096U

bool cedrus_enc_control_is_standard(u32 id)
{
	switch (id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
	case V4L2_CID_JPEG_COMPRESSION_QUALITY:
		return true;
	default:
		return false;
	}
}

int cedrus_enc_format_validate(const struct cedrus_enc_format *format)
{
	if (!format || !format->pixelformat ||
	    (format->direction != CEDRUS_ENC_FORMAT_RAW &&
	     format->direction != CEDRUS_ENC_FORMAT_CODED) ||
	    !format->min_width || format->min_width > format->max_width ||
	    !format->min_height || format->min_height > format->max_height ||
	    !is_power_of_2(format->width_align) ||
	    !is_power_of_2(format->height_align) ||
	    !is_power_of_2(format->buffer_align) ||
	    !IS_ALIGNED(format->max_width, format->width_align) ||
	    !IS_ALIGNED(format->max_height, format->height_align))
		return -EINVAL;

	if (format->direction == CEDRUS_ENC_FORMAT_RAW) {
		u64 max_size;

		if (format->pixelformat != V4L2_PIX_FMT_NV12 &&
		    format->pixelformat != V4L2_PIX_FMT_YUV420)
			return -EOPNOTSUPP;
		if (!is_power_of_2(format->stride_align))
			return -EINVAL;
		max_size = (u64)ALIGN(format->max_width, format->stride_align) *
			   format->max_height * 3 / 2;
		if (max_size > U32_MAX)
			return -ERANGE;
	} else {
		if (format->pixelformat != V4L2_PIX_FMT_H264 &&
		    format->pixelformat != V4L2_PIX_FMT_JPEG)
			return -EOPNOTSUPP;
		if (format->stride_align || !format->min_sizeimage)
			return -EINVAL;
	}

	return 0;
}

int cedrus_enc_backend_validate(const struct cedrus_enc_backend_ops *ops)
{
	if (!ops)
		return -EINVAL;

	if ((ops->contract & CEDRUS_ENC_CONTRACT_REQUIRED) !=
	    CEDRUS_ENC_CONTRACT_REQUIRED)
		return -EOPNOTSUPP;

	if (!ops->formats || !ops->num_formats || !ops->controls ||
	    !ops->num_controls || !ops->s_ctrl || !ops->supports_pair ||
	    !ops->start_streaming ||
	    !ops->stop_streaming || !ops->prepare_job || !ops->trigger ||
	    !ops->irq_status || !ops->irq_ack || !ops->abort ||
	    !ops->reset_complete || !ops->suspend || !ops->resume ||
	    !ops->unbind)
		return -EINVAL;

	{
		bool raw = false, coded = false;
		unsigned int i;

		for (i = 0; i < ops->num_formats; i++) {
			int ret = cedrus_enc_format_validate(&ops->formats[i]);

			if (ret)
				return ret;
			raw |= ops->formats[i].direction == CEDRUS_ENC_FORMAT_RAW;
			coded |= ops->formats[i].direction == CEDRUS_ENC_FORMAT_CODED;
		}
		if (!raw || !coded)
			return -EINVAL;
		for (i = 0; i < ops->num_formats; i++) {
			unsigned int j;

			if (ops->formats[i].direction != CEDRUS_ENC_FORMAT_RAW)
				continue;
			for (j = 0; j < ops->num_formats; j++) {
				if (ops->formats[j].direction != CEDRUS_ENC_FORMAT_CODED)
					continue;
				if (!ops->supports_pair(ops->formats[i].pixelformat,
						ops->formats[j].pixelformat))
					return -EINVAL;
			}
		}
		for (i = 0; i < ops->num_controls; i++)
			if (!cedrus_enc_control_is_standard(ops->controls[i].id) ||
			    ops->controls[i].ops)
				return -EINVAL;
	}

	return 0;
}

int cedrus_enc_abort(const struct cedrus_enc_backend_ops *ops,
		     struct cedrus_ctx *ctx,
		     enum cedrus_enc_abort_reason reason)
{
	if (!ops || !ops->abort || !ctx ||
	    (unsigned int)reason > CEDRUS_ENC_ABORT_IOMMU_FAULT)
		return -EINVAL;

	return ops->abort(ctx, reason);
}

int cedrus_enc_reset_complete(const struct cedrus_enc_backend_ops *ops,
			      struct cedrus_ctx *ctx)
{
	if (!ops || !ops->reset_complete || !ctx)
		return -EINVAL;

	return ops->reset_complete(ctx);
}

void cedrus_enc_unbind(const struct cedrus_enc_backend_ops *ops,
		       struct cedrus_dev *dev)
{
	if (ops && ops->unbind && dev)
		ops->unbind(dev);
}

int cedrus_a133_h264_enc_source_prepare(u32 pixelformat, u32 width,
					u32 height, u32 bytesperline,
					dma_addr_t luma, dma_addr_t chroma,
					struct cedrus_a133_enc_source *source)
{
	size_t chroma_plane_size;
	dma_addr_t chroma2;
	u32 width_units;
	u32 height_units;
	u32 stride_units;

	if (!source || !width || !height || !bytesperline)
		return -EINVAL;
	if (width > CEDRUS_A133_ENC_MAX_DIMENSION ||
	    height > CEDRUS_A133_ENC_MAX_DIMENSION)
		return -ERANGE;

	if (pixelformat != V4L2_PIX_FMT_NV12 &&
	    pixelformat != V4L2_PIX_FMT_YUV420)
		return -EINVAL;

	/* The isolated register fields encode these quantities exactly. */
	if (width % CEDRUS_A133_ENC_DIMENSION_UNIT ||
	    height % CEDRUS_A133_ENC_DIMENSION_UNIT ||
	    bytesperline % CEDRUS_A133_ENC_STRIDE_UNIT ||
	    bytesperline < width)
		return -EINVAL;

	width_units = width / CEDRUS_A133_ENC_DIMENSION_UNIT;
	height_units = height / CEDRUS_A133_ENC_DIMENSION_UNIT;
	stride_units = bytesperline / CEDRUS_A133_ENC_STRIDE_UNIT;

	if (width_units > U16_MAX || height_units > U16_MAX ||
	    stride_units > U16_MAX)
		return -ERANGE;

	/*
	 * Stock programs the third input address for both formats.  It points at
	 * the planar V plane for YUV420 and at the same half-chroma boundary for
	 * NV12, where the preprocessor ignores it.
	 */
	if (check_mul_overflow((size_t)bytesperline, (size_t)height,
			       &chroma_plane_size))
		return -EOVERFLOW;
	chroma_plane_size /= 4;
	if (check_add_overflow(chroma, (dma_addr_t)chroma_plane_size,
			       &chroma2))
		return -EOVERFLOW;
	if (luma > U32_MAX || chroma > U32_MAX || chroma2 > U32_MAX)
		return -ERANGE;

	*source = (struct cedrus_a133_enc_source) {
		.input_size = width_units << 16 | height_units,
		.stride = stride_units << 16,
		.control = pixelformat == V4L2_PIX_FMT_YUV420 ?
			   VE_ENC_ISP_CONTROL_PLANAR : 0,
		.mb_width = stride_units,
		.input_size_reversed = height_units << 16 | width_units,
		.luma = luma,
		.chroma = chroma,
		.chroma2 = chroma2,
	};

	return 0;
}

int cedrus_a133_h264_enc_bitstream_prepare(dma_addr_t address, size_t size,
					   struct cedrus_a133_enc_bitstream *bs)
{
	dma_addr_t last;
	size_t limit;

	if (!bs || !size)
		return -EINVAL;

	/* Start/end registers discard eight address bits.  Reject aliasing. */
	if (!IS_ALIGNED(address, CEDRUS_A133_ENC_ADDR_UNIT) ||
	    !IS_ALIGNED(size, CEDRUS_A133_ENC_ADDR_UNIT))
		return -EINVAL;

	if (check_add_overflow(address, (dma_addr_t)size - 1, &last) ||
	    check_mul_overflow(size, (size_t)8, &limit))
		return -EOVERFLOW;

	if ((address >> 8) > U32_MAX || (last >> 8) > U32_MAX ||
	    limit > U32_MAX)
		return -ERANGE;

	*bs = (struct cedrus_a133_enc_bitstream) {
		.start = address >> 8,
		.end = last >> 8,
		.offset = 0,
		.limit = limit,
	};

	return 0;
}

enum cedrus_enc_irq_status cedrus_a133_h264_enc_irq_status(u32 status)
{
	if (status & (VE_ENC_AVC_STATUS_SIZE_ERROR |
		      VE_ENC_AVC_STATUS_MB_ERROR))
		return CEDRUS_ENC_IRQ_ERROR;

	if (status & VE_ENC_AVC_STATUS_DONE)
		return CEDRUS_ENC_IRQ_OK;

	return CEDRUS_ENC_IRQ_NONE;
}

int cedrus_a133_h264_enc_qp_update(u32 register_value, unsigned int qp,
				   u32 *updated)
{
	if (!updated || qp > 51)
		return -EINVAL;

	/* Runtime deltas isolate the selected QP to the low byte. */
	*updated = (register_value & ~GENMASK(7, 0)) | qp;

	return 0;
}

int cedrus_a133_h264_enc_buffer_layout(u32 width, u32 height,
				       struct cedrus_a133_h264_enc_buffer_layout *layout)
{
	size_t aligned_width, aligned_height;
	size_t reference_width, luma_height, chroma_height;
	size_t mb_width, mb_height, subpixel_stride, subpixel_height;
	size_t value;

	if (!layout || !width || !height)
		return -EINVAL;
	if (width > CEDRUS_A133_ENC_MAX_DIMENSION ||
	    height > CEDRUS_A133_ENC_MAX_DIMENSION)
		return -ERANGE;
	if (width % CEDRUS_A133_ENC_DIMENSION_UNIT ||
	    height % CEDRUS_A133_ENC_DIMENSION_UNIT)
		return -EINVAL;

	aligned_width = ALIGN(width, 16);
	aligned_height = ALIGN(height, 16);
	mb_width = aligned_width / 16;
	mb_height = aligned_height / 16;

	/* Exact Ver2 formulas in the matching A133 stock allocation path. */
	reference_width = ALIGN(aligned_width, 32);
	luma_height = ALIGN(aligned_height + 8, 64);
	chroma_height = ALIGN(aligned_height, 128);

	if (check_mul_overflow(reference_width, luma_height, &value))
		return -EOVERFLOW;
	layout->reconstruction_luma = value;
	if (check_mul_overflow(reference_width, chroma_height, &value))
		return -EOVERFLOW;
	layout->reconstruction_chroma = value / 2;
	if (check_add_overflow(layout->reconstruction_luma,
			       layout->reconstruction_chroma, &value))
		return -EOVERFLOW;
	layout->reconstruction = value;

	subpixel_stride = 32 * DIV_ROUND_UP(mb_width, 48) +
			  2 * ALIGN(mb_width, 32);
	subpixel_height = (aligned_height + 72) / 8;
	if (check_mul_overflow(subpixel_stride, subpixel_height, &value))
		return -EOVERFLOW;
	layout->subpixel = value;

	if (check_mul_overflow(ALIGN(mb_width, 4), mb_height, &value) ||
	    check_mul_overflow(value, (size_t)8, &value))
		return -EOVERFLOW;
	layout->motion_vectors = value;

	if (check_mul_overflow(aligned_width, (size_t)8, &value))
		return -EOVERFLOW;
	layout->macroblock_info = value;
	layout->deblock = value;

	if (check_mul_overflow(mb_height, (size_t)12, &value) ||
	    check_add_overflow(value, (size_t)500, &value))
		return -EOVERFLOW;
	layout->work_9c = value;

	if (check_mul_overflow(ALIGN(mb_width, 8), mb_height, &value) ||
	    check_mul_overflow(value, (size_t)12, &value))
		return -EOVERFLOW;
	layout->filter_history = value;

	return 0;
}

int cedrus_a133_h264_enc_frame_slot_prepare(dma_addr_t reconstruction,
					    dma_addr_t subpixel,
					    const struct cedrus_a133_h264_enc_buffer_layout *layout,
					    struct cedrus_a133_h264_enc_frame_slot *slot)
{
	dma_addr_t chroma, reconstruction_last, subpixel_last;

	if (!layout || !slot || !layout->reconstruction ||
	    !layout->reconstruction_luma || !layout->reconstruction_chroma ||
	    !layout->subpixel ||
	    layout->reconstruction_luma > layout->reconstruction ||
	    layout->reconstruction_chroma != layout->reconstruction -
					     layout->reconstruction_luma)
		return -EINVAL;
	if (!IS_ALIGNED(reconstruction, CEDRUS_A133_ENC_ADDR_UNIT) ||
	    !IS_ALIGNED(subpixel, CEDRUS_A133_ENC_ADDR_UNIT))
		return -EINVAL;
	if (check_add_overflow(reconstruction,
			       (dma_addr_t)layout->reconstruction_luma,
			       &chroma) ||
	    check_add_overflow(reconstruction,
			       (dma_addr_t)layout->reconstruction - 1,
			       &reconstruction_last) ||
	    check_add_overflow(subpixel, (dma_addr_t)layout->subpixel - 1,
			       &subpixel_last))
		return -EOVERFLOW;
	if ((reconstruction >> 8) > U32_MAX || (chroma >> 8) > U32_MAX ||
	    (reconstruction_last >> 8) > U32_MAX ||
	    (subpixel >> 8) > U32_MAX || (subpixel_last >> 8) > U32_MAX)
		return -ERANGE;

	*slot = (struct cedrus_a133_h264_enc_frame_slot) {
		.luma = reconstruction >> 8,
		.chroma = chroma >> 8,
		.subpixel = subpixel >> 8,
	};

	return 0;
}

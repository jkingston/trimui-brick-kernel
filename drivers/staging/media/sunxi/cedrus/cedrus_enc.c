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
#include <linux/overflow.h>

#include <linux/videodev2.h>

#include "cedrus_enc.h"
#include "cedrus_enc_regs.h"

#define CEDRUS_A133_ENC_DIMENSION_UNIT	8U
#define CEDRUS_A133_ENC_STRIDE_UNIT	16U
#define CEDRUS_A133_ENC_ADDR_UNIT	256U
#define CEDRUS_A133_ENC_MAX_DIMENSION	4096U

int cedrus_enc_backend_validate(const struct cedrus_enc_backend_ops *ops)
{
	if (!ops)
		return -EINVAL;

	if ((ops->contract & CEDRUS_ENC_CONTRACT_REQUIRED) !=
	    CEDRUS_ENC_CONTRACT_REQUIRED)
		return -EOPNOTSUPP;

	if (!ops->start_streaming || !ops->stop_streaming ||
	    !ops->prepare_job || !ops->trigger || !ops->irq_status ||
	    !ops->irq_ack || !ops->abort || !ops->iommu_fault ||
	    !ops->suspend || !ops->resume)
		return -EINVAL;

	return 0;
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

// SPDX-License-Identifier: GPL-2.0
/* A133 H.264 encoder DMA and MMIO helpers. */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/overflow.h>

#include "cedrus_enc_regs.h"
#include "cedrus_h264_enc_hw.h"

#define CEDRUS_A133_ENC_ADDR_UNIT	256U
#define CEDRUS_A133_ENC_IRQ_MASK	0xfU
#define CEDRUS_A133_ENC_DIMENSION_UNIT	8U
#define CEDRUS_A133_ENC_MB_UNIT		16U
#define CEDRUS_A133_ENC_LENGTH_STRIDE_MBS	48U
#define CEDRUS_A133_ENC_MAX_DIMENSION	4096U
#define CEDRUS_A133_ENC_MOTION_CONTROL	0x1200U

int cedrus_a133_h264_codec_prepare(u32 width, u32 height, unsigned int qp,
				   bool idr,
				   struct cedrus_a133_h264_enc_codec *codec)
{
	u32 length_stride;
	u32 mb_width;

	if (!codec || !width || !height)
		return -EINVAL;
	if (width > CEDRUS_A133_ENC_MAX_DIMENSION ||
	    height > CEDRUS_A133_ENC_MAX_DIMENSION)
		return -ERANGE;
	if (width % CEDRUS_A133_ENC_DIMENSION_UNIT ||
	    height % CEDRUS_A133_ENC_DIMENSION_UNIT || qp > 51)
		return -EOPNOTSUPP;

	mb_width = DIV_ROUND_UP(width, CEDRUS_A133_ENC_MB_UNIT);
	length_stride = DIV_ROUND_UP(mb_width,
				     CEDRUS_A133_ENC_LENGTH_STRIDE_MBS);
	if (length_stride > FIELD_MAX(VE_ENC_AVC_QP_LENGTH_STRIDE))
		return -ERANGE;

	/*
	 * h264InitRegInfo derives the length stride from 48 macroblocks, enables
	 * dynamic ME above 192 pixels and sets bit 27.  The remaining constants
	 * are common to every successful fixed-QP A133 capture; only bit 4
	 * changes for P.
	 */
	*codec = (struct cedrus_a133_h264_enc_codec) {
		.picture_size = (width / CEDRUS_A133_ENC_DIMENSION_UNIT) << 16 |
				height / CEDRUS_A133_ENC_DIMENSION_UNIT,
		.picture_control = VE_ENC_AVC_PICTURE_CONTROL_CABAC |
				   VE_ENC_AVC_PICTURE_CONTROL_FIXED_MODE |
				   (idr ? 0 :
				    VE_ENC_AVC_PICTURE_CONTROL_P_SLICE),
		.qp_control = VE_ENC_AVC_QP_STOCK_VER2 |
			      (width > 192 ? VE_ENC_AVC_QP_DYNAMIC_ME : 0) |
			      FIELD_PREP(VE_ENC_AVC_QP_LENGTH_STRIDE,
					 length_stride) |
			      FIELD_PREP(VE_ENC_AVC_QP_VALUE, qp),
		.motion_control = CEDRUS_A133_ENC_MOTION_CONTROL,
	};

	return 0;
}

int cedrus_a133_h264_frame_prepare(struct cedrus_a133_h264_enc_rotation *rotation,
				   const struct cedrus_a133_h264_enc_frame_slot slots[2], bool idr,
				   struct cedrus_a133_h264_enc_frame_slot *reference,
				   struct cedrus_a133_h264_enc_frame_slot *reconstruction)
{
	u8 next;

	if (!rotation || !slots || !reference || !reconstruction)
		return -EINVAL;
	if (rotation->pending)
		return -EBUSY;
	if (rotation->reference > 1)
		return -EINVAL;

	if (!rotation->started) {
		if (!idr)
			return -EINVAL;
		next = 0;
		*reference = slots[0];
		*reconstruction = slots[0];
	} else {
		/* Stock evidence does not yet establish a later-IDR transition. */
		if (idr)
			return -EOPNOTSUPP;
		next = rotation->reference ^ 1;
		*reference = slots[rotation->reference];
		*reconstruction = slots[next];
	}

	rotation->pending_reference = next;
	rotation->pending = true;
	return 0;
}

int cedrus_a133_h264_frame_complete(struct cedrus_a133_h264_enc_rotation *rotation,
				    bool success)
{
	if (!rotation || !rotation->pending)
		return -EINVAL;

	if (success) {
		rotation->reference = rotation->pending_reference;
		rotation->started = true;
	}
	rotation->pending = false;
	return 0;
}

static int cedrus_a133_h264_dma_address(dma_addr_t address, size_t size,
					u32 *encoded)
{
	dma_addr_t last;

	if (!encoded || !size || !IS_ALIGNED(address, CEDRUS_A133_ENC_ADDR_UNIT))
		return -EINVAL;
	if (check_add_overflow(address, (dma_addr_t)size - 1, &last))
		return -EOVERFLOW;
	if ((address >> 8) > U32_MAX || (last >> 8) > U32_MAX)
		return -ERANGE;

	*encoded = address >> 8;
	return 0;
}

int cedrus_a133_h264_dma_prepare(const struct cedrus_a133_enc_source *source,
				 const struct cedrus_a133_enc_bitstream *bitstream,
				 const struct cedrus_a133_h264_enc_frame_slot *reference,
				 const struct cedrus_a133_h264_enc_frame_slot *reconstruction,
				 dma_addr_t motion_vectors,
				 dma_addr_t macroblock_info,
				 const struct cedrus_a133_h264_enc_buffer_layout *layout,
				 struct cedrus_a133_h264_enc_dma *dma)
{
	int ret;

	if (!source || !bitstream || !reference || !reconstruction ||
	    !layout || !dma || !layout->motion_vectors ||
	    !layout->macroblock_info)
		return -EINVAL;

	ret = cedrus_a133_h264_dma_address(motion_vectors,
					   layout->motion_vectors,
					   &dma->motion_vectors);
	if (ret)
		return ret;
	ret = cedrus_a133_h264_dma_address(macroblock_info,
					   layout->macroblock_info,
					   &dma->macroblock_info);
	if (ret)
		return ret;

	dma->source = *source;
	dma->bitstream = *bitstream;
	dma->reference = *reference;
	dma->reconstruction = *reconstruction;

	return 0;
}

void cedrus_a133_h264_program_source(void __iomem *base,
				     const struct cedrus_a133_h264_enc_dma *dma)
{
	writel(dma->source.input_size, base + VE_ENC_ISP_INPUT_SIZE);
	writel(dma->source.stride, base + VE_ENC_ISP_INPUT_STRIDE);
	writel(dma->source.control, base + VE_ENC_ISP_CONTROL);
	writel(dma->source.mb_width, base + VE_ENC_ISP_MB_WIDTH);
	writel(dma->source.input_size_reversed,
	       base + VE_ENC_ISP_INPUT_SIZE_REVERSED);
	writel(dma->source.luma, base + VE_ENC_ISP_INPUT_LUMA);
	writel(dma->source.chroma, base + VE_ENC_ISP_INPUT_CHROMA);
	writel(dma->source.chroma2, base + VE_ENC_ISP_INPUT_CHROMA2);
}

void cedrus_a133_h264_program_bitstream(void __iomem *base,
					const struct cedrus_a133_h264_enc_dma *dma)
{
	writel(dma->bitstream.start, base + VE_ENC_AVC_BITSTREAM_START);
	writel(dma->bitstream.end, base + VE_ENC_AVC_BITSTREAM_END);
	writel(dma->bitstream.offset, base + VE_ENC_AVC_BITSTREAM_OFFSET);
	writel(dma->bitstream.limit, base + VE_ENC_AVC_BITSTREAM_LIMIT);
}

void cedrus_a133_h264_program_frames(void __iomem *base,
				     const struct cedrus_a133_h264_enc_dma *dma)
{
	writel(dma->reference.luma, base + VE_ENC_AVC_REFERENCE_LUMA);
	writel(dma->reference.chroma, base + VE_ENC_AVC_REFERENCE_CHROMA);
	writel(dma->reconstruction.luma,
	       base + VE_ENC_AVC_RECONSTRUCTION_LUMA);
	writel(dma->reconstruction.chroma,
	       base + VE_ENC_AVC_RECONSTRUCTION_CHROMA);
	writel(dma->reference.subpixel,
	       base + VE_ENC_AVC_SUBPIXEL_REFERENCE);
	writel(dma->reconstruction.subpixel,
	       base + VE_ENC_AVC_SUBPIXEL_CURRENT);
}

void cedrus_a133_h264_program_aux(void __iomem *base,
				  const struct cedrus_a133_h264_enc_dma *dma)
{
	writel(dma->motion_vectors, base + VE_ENC_AVC_MOTION_VECTORS);
	writel(dma->macroblock_info, base + VE_ENC_AVC_MACROBLOCK_INFO);
}

void cedrus_a133_h264_program_codec(void __iomem *base,
				    const struct cedrus_a133_h264_enc_codec *codec)
{
	writel(codec->picture_size, base + VE_ENC_AVC_PICTURE_SIZE);
	writel(codec->picture_control, base + VE_ENC_AVC_PICTURE_CONTROL);
	writel(codec->qp_control, base + VE_ENC_AVC_QP);
	writel(codec->motion_control, base + VE_ENC_AVC_MOTION_CONTROL);
}

void cedrus_a133_h264_irq_arm(void __iomem *base)
{
	u32 status;

	writel(CEDRUS_A133_ENC_IRQ_MASK, base + VE_ENC_AVC_IRQ_CONTROL);
	status = readl(base + VE_ENC_AVC_STATUS);
	writel(status | VE_ENC_AVC_STATUS_LOW_MASK, base + VE_ENC_AVC_STATUS);
}

u32 cedrus_a133_h264_irq_ack(void __iomem *base)
{
	u32 status = readl(base + VE_ENC_AVC_STATUS);

	writel(status, base + VE_ENC_AVC_STATUS);
	return status;
}

void cedrus_a133_h264_trigger(void __iomem *base)
{
	writel(VE_ENC_AVC_TRIGGER_SLICE, base + VE_ENC_AVC_TRIGGER);
}

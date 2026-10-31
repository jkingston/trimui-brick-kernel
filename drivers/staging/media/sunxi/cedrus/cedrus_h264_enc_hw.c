// SPDX-License-Identifier: GPL-2.0
/* A133 H.264 encoder DMA and MMIO helpers. */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/overflow.h>

#include "cedrus_enc_regs.h"
#include "cedrus_h264_enc.h"
#include "cedrus_h264_enc_hw.h"

#define CEDRUS_A133_ENC_ADDR_UNIT	256U
#define CEDRUS_A133_ENC_IRQ_MASK	0xfU
#define CEDRUS_A133_ENC_DIMENSION_UNIT	8U
#define CEDRUS_A133_ENC_MB_UNIT		16U
#define CEDRUS_A133_ENC_LENGTH_STRIDE_MBS	48U
#define CEDRUS_A133_ENC_MAX_DIMENSION	4096U
#define CEDRUS_A133_ENC_MOTION_CONTROL	0x1200U
#define CEDRUS_A133_ENC_PUT_BITS_POLLS	10000U
#define CEDRUS_A133_ENC_PUT_BITS_MAX	\
	FIELD_MAX(VE_ENC_AVC_TRIGGER_BIT_COUNT)
#define CEDRUS_A133_ENC_LEVEL_5_1_MAX_MBS	36864U
#define CEDRUS_A133_ENC_PIC_INIT_QP	26
#define CEDRUS_A133_ENC_320_PICTURE_SIZE	0x0028001eU
#define CEDRUS_A133_ENC_320_QP30		0x0800051eU
#define CEDRUS_A133_ENC_IDR_CONTROL	0x00000500U
#define CEDRUS_A133_ENC_P_CONTROL	0x00000510U
#define CEDRUS_A133_ENC_DYNAMIC_ME_48	0x000c0002U
#define CEDRUS_A133_ENC_DYNAMIC_ME_4C	0x00400034U

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
	 * dynamic ME above 192 pixels and sets bit 27.  The tested path selects
	 * fixModeNum 1.  The remaining constants are common to every successful
	 * fixed-QP A133 capture; only bit 4 changes for P.
	 */
	*codec = (struct cedrus_a133_h264_enc_codec) {
		.picture_size = (width / CEDRUS_A133_ENC_DIMENSION_UNIT) << 16 |
				height / CEDRUS_A133_ENC_DIMENSION_UNIT,
		.picture_control = VE_ENC_AVC_PICTURE_CONTROL_CABAC |
				   FIELD_PREP(VE_ENC_AVC_PICTURE_CONTROL_FIX_MODE_NUM,
					      1) |
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

bool cedrus_a133_h264_frame_is_idr(const struct cedrus_a133_h264_enc_rotation *rotation,
				   u32 gop_size, bool force_keyframe)
{
	if (!rotation || !gop_size)
		return false;

	return !rotation->started || force_keyframe ||
	       rotation->frames_since_idr >= gop_size;
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
		next = rotation->reference ^ 1;
		*reference = slots[rotation->reference];
		*reconstruction = slots[next];
	}

	rotation->pending_reference = next;
	rotation->pending_keyframe = idr;
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
		if (rotation->pending_keyframe) {
			rotation->next_frame_num = 1;
			rotation->frames_since_idr = 1;
		} else {
			rotation->next_frame_num++;
			rotation->frames_since_idr++;
		}
		rotation->started = true;
	}
	rotation->pending = false;
	rotation->pending_keyframe = false;
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
				 dma_addr_t work_9c,
				 const struct cedrus_a133_h264_enc_buffer_layout *layout,
				 struct cedrus_a133_h264_enc_dma *dma)
{
	int ret;

	if (!source || !bitstream || !reference || !reconstruction ||
	    !layout || !dma || !layout->motion_vectors ||
	    !layout->macroblock_info || !layout->work_9c)
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
	ret = cedrus_a133_h264_dma_address(work_9c, layout->work_9c,
					   &dma->work_9c);
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
	writel(dma->bitstream.offset, base + VE_ENC_AVC_BITSTREAM_OFFSET);
	writel(dma->bitstream.start, base + VE_ENC_AVC_BITSTREAM_START);
	writel(dma->bitstream.end, base + VE_ENC_AVC_BITSTREAM_END);
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
	writel(dma->work_9c, base + VE_ENC_AVC_WORK_9C);
	/* The successful stock fixed-QP path leaves the deblock address zero. */
	writel(0, base + VE_ENC_AVC_DEBLOCK);
}

void cedrus_a133_h264_program_codec(void __iomem *base,
				    const struct cedrus_a133_h264_enc_codec *codec)
{
	writel(codec->picture_size, base + VE_ENC_AVC_PICTURE_SIZE);
	writel(codec->picture_control, base + VE_ENC_AVC_PICTURE_CONTROL);
	writel(codec->qp_control, base + VE_ENC_AVC_QP);
	writel(codec->motion_control, base + VE_ENC_AVC_MOTION_CONTROL);
}

int cedrus_a133_h264_put_bits(void __iomem *base, u32 value,
			      unsigned int count)
{
	unsigned int polls_left;

	if (!base || !count || count > CEDRUS_A133_ENC_PUT_BITS_MAX)
		return -EINVAL;

	/*
	 * Stock PutBits waits for status bit 9 for at most 10000 reads, then
	 * writes the payload before issuing start type 1 with a six-bit count.
	 * Counts above 32 emit leading zeroes before the 32-bit payload; stock
	 * uses that representation for long Exp-Golomb values.
	 */
	for (polls_left = CEDRUS_A133_ENC_PUT_BITS_POLLS;
	     polls_left; polls_left--) {
		if (readl(base + VE_ENC_AVC_STATUS) &
		    VE_ENC_AVC_STATUS_PUT_BITS_READY) {
			/* Stock treats readiness on the final read as timeout. */
			if (polls_left == 1)
				break;
			writel(value, base + VE_ENC_AVC_PUT_BITS_DATA);
			writel(VE_ENC_AVC_TRIGGER_PUT_BITS |
			       FIELD_PREP(VE_ENC_AVC_TRIGGER_BIT_COUNT, count),
			       base + VE_ENC_AVC_TRIGGER);
			return 0;
		}
	}

	return -ETIMEDOUT;
}

int cedrus_a133_h264_put_ue(void __iomem *base, u32 value)
{
	u32 code_num;
	unsigned int bits;

	if (value == U32_MAX)
		return -EOVERFLOW;

	code_num = value + 1;
	bits = fls(code_num);

	return cedrus_a133_h264_put_bits(base, code_num, 2 * bits - 1);
}

int cedrus_a133_h264_put_se(void __iomem *base, s32 value)
{
	u32 code_num;
	unsigned int bits;

	if (value == S32_MIN)
		return -EOVERFLOW;

	if (value > 0)
		code_num = (u32)value * 2;
	else
		code_num = (u32)-value * 2 + 1;
	bits = fls(code_num);

	return cedrus_a133_h264_put_bits(base, code_num, 2 * bits - 1);
}

struct cedrus_a133_h264_vlc_builder {
	struct cedrus_a133_h264_vlc_sequence *sequence;
	int error;
};

static int cedrus_a133_h264_vlc_append(struct cedrus_a133_h264_vlc_sequence *sequence,
				       u32 value, unsigned int bits)
{
	if (!bits || bits > CEDRUS_A133_ENC_PUT_BITS_MAX)
		return -EINVAL;
	if (bits < 32 && value >= BIT(bits))
		return -ERANGE;
	if (sequence->count == CEDRUS_A133_H264_VLC_MAX_COMMANDS)
		return -EOVERFLOW;
	if (sequence->bit_count > U16_MAX - bits)
		return -EOVERFLOW;

	sequence->commands[sequence->count++] =
		(struct cedrus_a133_h264_vlc_command) {
			.value = value,
			.bits = bits,
		};
	sequence->bit_count += bits;
	return 0;
}

static void cedrus_a133_h264_vlc_bits(struct cedrus_a133_h264_vlc_builder *builder,
				      u32 value, unsigned int bits)
{
	if (!builder->error)
		builder->error = cedrus_a133_h264_vlc_append(builder->sequence,
							     value, bits);
}

static void cedrus_a133_h264_vlc_ue(struct cedrus_a133_h264_vlc_builder *builder,
				    u32 value)
{
	u32 code_num;
	unsigned int bits;

	if (builder->error)
		return;
	if (value == U32_MAX) {
		builder->error = -EOVERFLOW;
		return;
	}

	code_num = value + 1;
	bits = fls(code_num);
	cedrus_a133_h264_vlc_bits(builder, code_num, 2 * bits - 1);
}

static void cedrus_a133_h264_vlc_se(struct cedrus_a133_h264_vlc_builder *builder,
				    s32 value)
{
	u32 code_num;
	unsigned int bits;

	if (builder->error)
		return;
	if (value == S32_MIN) {
		builder->error = -EOVERFLOW;
		return;
	}

	if (value > 0)
		code_num = (u32)value * 2;
	else
		code_num = (u32)-value * 2 + 1;
	bits = fls(code_num);
	cedrus_a133_h264_vlc_bits(builder, code_num, 2 * bits - 1);
}

static void cedrus_a133_h264_vlc_nal_start(struct cedrus_a133_h264_vlc_builder *builder,
					   u8 header)
{
	struct cedrus_h264_enc_prefix_map *prefixes =
		&builder->sequence->prefixes;
	static const u8 start[] = { 0, 0, 0, 1 };
	unsigned int i;

	if (builder->error)
		return;
	if (!IS_ALIGNED(builder->sequence->bit_count, 8) ||
	    prefixes->count == CEDRUS_H264_ENC_MAX_PREFIXES) {
		builder->error = -EOVERFLOW;
		return;
	}
	prefixes->entries[prefixes->count++] =
		(struct cedrus_h264_enc_prefix) {
			.byte_offset = builder->sequence->bit_count / 8,
			.nal_header = header,
		};
	for (i = 0; i < ARRAY_SIZE(start); i++)
		cedrus_a133_h264_vlc_bits(builder, start[i], 8);
	cedrus_a133_h264_vlc_bits(builder, header, 8);
}

static void
cedrus_a133_h264_vlc_trailing_bits(struct cedrus_a133_h264_vlc_builder *builder)
{
	unsigned int zero_bits;

	cedrus_a133_h264_vlc_bits(builder, 1, 1);
	if (builder->error)
		return;

	/* Stock emits a second PUT_BITS transaction to reach a byte boundary. */
	zero_bits = 8 - builder->sequence->bit_count % 8;
	cedrus_a133_h264_vlc_bits(builder, 0, zero_bits);
}

static int
cedrus_a133_h264_vlc_params_check(const struct cedrus_h264_enc_params *params)
{
	if (!params)
		return -EINVAL;
	if (params->profile_idc != 100 || params->constraint_flags ||
	    params->level_idc != 51 || !params->cabac || params->qp > 51 ||
	    !params->gop_size)
		return -EOPNOTSUPP;

	return 0;
}

int cedrus_a133_h264_headers_prepare(const struct cedrus_h264_enc_params *params,
				     u32 width, u32 height,
				     struct cedrus_a133_h264_vlc_sequence *sequence)
{
	struct cedrus_a133_h264_vlc_builder builder = { .sequence = sequence };
	u32 coded_width, coded_height;
	u32 crop_right, crop_bottom;
	int ret;

	if (!sequence || !width || !height)
		return -EINVAL;
	ret = cedrus_a133_h264_vlc_params_check(params);
	if (ret)
		return ret;
	if (width > CEDRUS_A133_ENC_MAX_DIMENSION ||
	    height > CEDRUS_A133_ENC_MAX_DIMENSION)
		return -ERANGE;
	if (width % CEDRUS_A133_ENC_DIMENSION_UNIT ||
	    height % CEDRUS_A133_ENC_DIMENSION_UNIT)
		return -EOPNOTSUPP;

	coded_width = ALIGN(width, CEDRUS_A133_ENC_MB_UNIT);
	coded_height = ALIGN(height, CEDRUS_A133_ENC_MB_UNIT);
	if (coded_width / CEDRUS_A133_ENC_MB_UNIT *
	    (coded_height / CEDRUS_A133_ENC_MB_UNIT) >
	    CEDRUS_A133_ENC_LEVEL_5_1_MAX_MBS)
		return -ERANGE;
	crop_right = (coded_width - width) / 2;
	crop_bottom = (coded_height - height) / 2;
	*sequence = (struct cedrus_a133_h264_vlc_sequence) {};

	/* _InitSPS: exact stock High/CABAC/Level-5.1 software-header path. */
	cedrus_a133_h264_vlc_nal_start(&builder, 0x67);
	cedrus_a133_h264_vlc_bits(&builder, params->profile_idc, 8);
	cedrus_a133_h264_vlc_bits(&builder, params->constraint_flags, 8);
	cedrus_a133_h264_vlc_bits(&builder, params->level_idc, 8);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, 1);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_ue(&builder, 4);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, 4);
	cedrus_a133_h264_vlc_ue(&builder, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_ue(&builder, coded_width / 16 - 1);
	cedrus_a133_h264_vlc_ue(&builder, coded_height / 16 - 1);
	cedrus_a133_h264_vlc_bits(&builder, 1, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_bits(&builder, crop_right || crop_bottom, 1);
	if (crop_right || crop_bottom) {
		cedrus_a133_h264_vlc_ue(&builder, 0);
		cedrus_a133_h264_vlc_ue(&builder, crop_right);
		cedrus_a133_h264_vlc_ue(&builder, 0);
		cedrus_a133_h264_vlc_ue(&builder, crop_bottom);
	}
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_trailing_bits(&builder);

	/* _InitPPS: exact one-reference, no-weighting stock path. */
	cedrus_a133_h264_vlc_nal_start(&builder, 0x68);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_bits(&builder, 1, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 2);
	cedrus_a133_h264_vlc_se(&builder, 0);
	cedrus_a133_h264_vlc_se(&builder, 0);
	cedrus_a133_h264_vlc_se(&builder, 0);
	cedrus_a133_h264_vlc_bits(&builder, 1, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	cedrus_a133_h264_vlc_se(&builder, 0);
	cedrus_a133_h264_vlc_trailing_bits(&builder);

	return builder.error;
}

int cedrus_a133_h264_slice_header_prepare(const struct cedrus_h264_enc_params *params,
					  const struct cedrus_a133_h264_slice_header *slice,
					  struct cedrus_a133_h264_vlc_sequence *sequence)
{
	struct cedrus_a133_h264_vlc_builder builder = { .sequence = sequence };
	int ret;

	if (!slice || !sequence)
		return -EINVAL;
	ret = cedrus_a133_h264_vlc_params_check(params);
	if (ret)
		return ret;
	if (slice->qp > 51)
		return -ERANGE;
	*sequence = (struct cedrus_a133_h264_vlc_sequence) {};

	/* SliceHeader: the stock first-reference IDR/P branch. */
	cedrus_a133_h264_vlc_nal_start(&builder, slice->idr ? 0x65 : 0x41);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_ue(&builder, slice->idr ? 7 : 5);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_bits(&builder, slice->frame_num, 8);
	if (slice->idr)
		cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_bits(&builder, slice->pic_order_cnt_lsb, 8);
	if (slice->idr) {
		cedrus_a133_h264_vlc_bits(&builder, 0, 1);
		cedrus_a133_h264_vlc_bits(&builder, 0, 1);
	} else {
		cedrus_a133_h264_vlc_bits(&builder, 0, 1);
		cedrus_a133_h264_vlc_bits(&builder, 0, 1);
		cedrus_a133_h264_vlc_bits(&builder, 0, 1);
		cedrus_a133_h264_vlc_ue(&builder, 1);
	}
	cedrus_a133_h264_vlc_se(&builder,
				slice->qp - CEDRUS_A133_ENC_PIC_INIT_QP);
	cedrus_a133_h264_vlc_ue(&builder, 0);
	cedrus_a133_h264_vlc_se(&builder, 0);
	cedrus_a133_h264_vlc_se(&builder, 0);

	return builder.error;
}

static int
cedrus_a133_h264_vlc_validate(const struct cedrus_a133_h264_vlc_sequence *sequence)
{
	unsigned int bit_count = 0;
	unsigned int i;

	if (!sequence || !sequence->count ||
	    sequence->count > CEDRUS_A133_H264_VLC_MAX_COMMANDS)
		return -EINVAL;

	/* Validate the whole transaction before the first irreversible write. */
	for (i = 0; i < sequence->count; i++) {
		unsigned int bits = sequence->commands[i].bits;

		if (!bits || bits > CEDRUS_A133_ENC_PUT_BITS_MAX)
			return -EINVAL;
		if (bits < 32 && sequence->commands[i].value >= BIT(bits))
			return -ERANGE;
		bit_count += bits;
	}
	if (bit_count != sequence->bit_count)
		return -EINVAL;
	return 0;
}

int cedrus_a133_h264_vlc_emit(void __iomem *base,
			      const struct cedrus_a133_h264_vlc_sequence *sequence)
{
	unsigned int i;
	int ret;

	if (!base)
		return -EINVAL;
	ret = cedrus_a133_h264_vlc_validate(sequence);
	if (ret)
		return ret;

	for (i = 0; i < sequence->count; i++) {
		ret = cedrus_a133_h264_put_bits(base,
						sequence->commands[i].value,
						sequence->commands[i].bits);
		if (ret)
			return ret;
	}

	return 0;
}

int cedrus_a133_h264_program_job(void __iomem *base,
				 const struct cedrus_a133_h264_enc_dma *dma,
				 const struct cedrus_a133_h264_enc_codec *codec,
				 const struct cedrus_a133_h264_vlc_sequence *parameter_sets,
				 const struct cedrus_a133_h264_vlc_sequence *slice_header,
				 bool emit_parameter_sets)
{
	struct cedrus_a133_h264_enc_dma frame_dma;
	int ret;

	if (!base || !dma || !codec || !slice_header ||
	    (emit_parameter_sets && !parameter_sets))
		return -EINVAL;
	if (codec->picture_size != CEDRUS_A133_ENC_320_PICTURE_SIZE ||
	    codec->qp_control != CEDRUS_A133_ENC_320_QP30 ||
	    codec->motion_control != CEDRUS_A133_ENC_MOTION_CONTROL ||
	    (codec->picture_control != CEDRUS_A133_ENC_IDR_CONTROL &&
	     codec->picture_control != CEDRUS_A133_ENC_P_CONTROL) ||
	    dma->source.input_size != CEDRUS_A133_ENC_320_PICTURE_SIZE ||
	    dma->bitstream.offset)
		return -EOPNOTSUPP;
	ret = cedrus_a133_h264_vlc_validate(slice_header);
	if (ret)
		return ret;
	if (emit_parameter_sets) {
		ret = cedrus_a133_h264_vlc_validate(parameter_sets);
		if (ret)
			return ret;
	}

	if (emit_parameter_sets) {
		writel(VE_ENC_AVC_TRIGGER_PARAMETER_SETS_BEGIN,
		       base + VE_ENC_AVC_TRIGGER);
		writel(dma->bitstream.offset,
		       base + VE_ENC_AVC_BITSTREAM_OFFSET);
		writel(dma->bitstream.start, base + VE_ENC_AVC_BITSTREAM_START);
		writel(dma->bitstream.end, base + VE_ENC_AVC_BITSTREAM_END);
		writel(dma->bitstream.limit, base + VE_ENC_AVC_BITSTREAM_LIMIT);
		writel(codec->picture_control,
		       base + VE_ENC_AVC_PICTURE_CONTROL);
		ret = cedrus_a133_h264_vlc_emit(base, parameter_sets);
		if (ret)
			return ret;
		writel(VE_ENC_AVC_TRIGGER_PARAMETER_SETS_END,
		       base + VE_ENC_AVC_TRIGGER);
	}

	cedrus_a133_h264_program_source(base, dma);
	cedrus_a133_h264_irq_arm(base);

	/* h264_config_registers fixed setup, in observed stock call order. */
	writel(0, base + VE_ENC_AVC_HP_FILTER);
	writel(codec->picture_size, base + VE_ENC_AVC_PICTURE_SIZE);
	writel(dma->work_9c, base + VE_ENC_AVC_WORK_9C);
	writel(codec->picture_control & VE_ENC_AVC_PICTURE_CONTROL_P_SLICE ?
	       0x120f0c09 : 0, base + VE_ENC_AVC_CLASSIFY_30);
	writel(0x261e1814, base + VE_ENC_AVC_CLASSIFY_34);
	writel(0xffff3630, base + VE_ENC_AVC_CLASSIFY_38);
	writel(codec->qp_control & ~VE_ENC_AVC_QP_STOCK_VER2,
	       base + VE_ENC_AVC_QP);
	writel(CEDRUS_A133_ENC_DYNAMIC_ME_48,
	       base + VE_ENC_AVC_DYNAMIC_ME_48);
	writel(CEDRUS_A133_ENC_DYNAMIC_ME_4C,
	       base + VE_ENC_AVC_DYNAMIC_ME_4C);
	writel(0, base + VE_ENC_AVC_CLASSIFY_40);
	writel(codec->picture_control & VE_ENC_AVC_PICTURE_CONTROL_P_SLICE ?
	       0xc0001900 : 0x80001900, base + VE_ENC_AVC_FIXED_2C);
	frame_dma = *dma;
	frame_dma.bitstream.offset = emit_parameter_sets ?
		parameter_sets->bit_count : 0;
	cedrus_a133_h264_program_bitstream(base, &frame_dma);
	cedrus_a133_h264_program_frames(base, dma);
	writel(dma->macroblock_info, base + VE_ENC_AVC_MACROBLOCK_INFO);
	writel(0, base + VE_ENC_AVC_DEBLOCK);
	writel(0, base + VE_ENC_AVC_FIXED_28);

	ret = cedrus_a133_h264_vlc_emit(base, slice_header);
	if (ret)
		return ret;
	writel(codec->picture_control, base + VE_ENC_AVC_PICTURE_CONTROL);
	writel(codec->qp_control, base + VE_ENC_AVC_QP);
	writel(0, base + VE_ENC_AVC_HEADER_CONTROL);
	writel(0, base + VE_ENC_AVC_VER2_24);
	writel(dma->motion_vectors, base + VE_ENC_AVC_MOTION_VECTORS);
	writel(codec->motion_control, base + VE_ENC_AVC_MOTION_CONTROL);
	return 0;
}

void cedrus_a133_h264_irq_arm(void __iomem *base)
{
	u32 status;

	writel(CEDRUS_A133_ENC_IRQ_MASK, base + VE_ENC_AVC_IRQ_CONTROL);
	status = readl(base + VE_ENC_AVC_STATUS);
	writel(status | VE_ENC_AVC_STATUS_LOW_MASK, base + VE_ENC_AVC_STATUS);
}

u32 cedrus_a133_h264_irq_read(void __iomem *base)
{
	return readl(base + VE_ENC_AVC_STATUS);
}

u32 cedrus_a133_h264_bitstream_length(void __iomem *base)
{
	return readl(base + VE_ENC_AVC_BITSTREAM_LENGTH);
}

void cedrus_a133_h264_irq_ack_status(void __iomem *base, u32 status)
{
	writel(status, base + VE_ENC_AVC_STATUS);
}

u32 cedrus_a133_h264_irq_ack(void __iomem *base)
{
	u32 status = cedrus_a133_h264_irq_read(base);

	cedrus_a133_h264_irq_ack_status(base, status);
	return status;
}

void cedrus_a133_h264_trigger(void __iomem *base)
{
	writel(VE_ENC_AVC_TRIGGER_SLICE, base + VE_ENC_AVC_TRIGGER);
}

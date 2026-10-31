// SPDX-License-Identifier: GPL-2.0
/*
 * Standard H.264 encoder helpers.
 *
 * These helpers deliberately contain no A133 register programming.  They
 * translate only the control combination exercised by the stock Brick stack,
 * produce standards-compliant Annex-B SPS/PPS data, and implement the V4L2
 * GOP/force-keyframe policy.  The stock 320x240 High/CABAC/Level-5.1 fixture
 * is reproduced byte-for-byte.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/string.h>

#include <linux/v4l2-controls.h>

#include "cedrus_h264_enc.h"

#define CEDRUS_H264_MAX_RBSP_SIZE	64
#define CEDRUS_H264_MAX_DIMENSION	4096
#define CEDRUS_H264_LEVEL_5_1_MAX_MBS	36864

int cedrus_h264_enc_repair_start_codes(u8 *data, size_t bytes,
				       const struct cedrus_h264_enc_prefix_map *prefixes)
{
	bool repair[CEDRUS_H264_ENC_MAX_PREFIXES] = {};
	unsigned int i;

	if (!data || !prefixes || !prefixes->count ||
	    prefixes->count > CEDRUS_H264_ENC_MAX_PREFIXES)
		return -EINVAL;

	/* Validate every generated boundary before changing the capture buffer. */
	for (i = 0; i < prefixes->count; i++) {
		const struct cedrus_h264_enc_prefix *prefix = &prefixes->entries[i];
		size_t offset = prefix->byte_offset;

		if ((i && offset <= prefixes->entries[i - 1].byte_offset) ||
		    offset > bytes || bytes - offset < 5)
			return -EINVAL;
		if (data[offset] || data[offset + 1])
			return -EBADMSG;
		if (!data[offset + 2] && data[offset + 3] == 1 &&
		    data[offset + 4] == prefix->nal_header)
			continue;
		if (bytes - offset < 6)
			return -EBADMSG;
		if (!data[offset + 2] && !data[offset + 3] &&
		    data[offset + 4] == 1 &&
		    data[offset + 5] == prefix->nal_header)
			continue;
		if (data[offset + 2] != 3 || data[offset + 3] ||
		    data[offset + 4] != 1 ||
		    data[offset + 5] != prefix->nal_header)
			return -EBADMSG;
		repair[i] = true;
	}

	/* 00 00 00 00 01 is a valid length-preserving Annex-B prefix. */
	for (i = 0; i < prefixes->count; i++)
		if (repair[i])
			data[prefixes->entries[i].byte_offset + 2] = 0;

	return 0;
}

struct cedrus_h264_bit_writer {
	u8 data[CEDRUS_H264_MAX_RBSP_SIZE];
	size_t bitpos;
	bool overflow;
};

static void cedrus_h264_put_bits(struct cedrus_h264_bit_writer *writer,
				 u32 value, unsigned int count)
{
	unsigned int i;

	if (count > 32 || writer->bitpos + count > sizeof(writer->data) * 8) {
		writer->overflow = true;
		return;
	}

	for (i = count; i; i--) {
		size_t byte = writer->bitpos / 8;
		unsigned int shift = 7 - writer->bitpos % 8;

		writer->data[byte] |= ((value >> (i - 1)) & 1) << shift;
		writer->bitpos++;
	}
}

static void cedrus_h264_put_ue(struct cedrus_h264_bit_writer *writer, u32 value)
{
	u32 code_num = value + 1;
	unsigned int bits;

	if (!code_num) {
		writer->overflow = true;
		return;
	}

	bits = fls(code_num);
	cedrus_h264_put_bits(writer, 0, bits - 1);
	cedrus_h264_put_bits(writer, code_num, bits);
}

static void cedrus_h264_put_se(struct cedrus_h264_bit_writer *writer, s32 value)
{
	u32 code_num;

	if (value == S32_MIN) {
		writer->overflow = true;
		return;
	}

	if (value > 0)
		code_num = (u32)value * 2U - 1;
	else
		code_num = (u32)-value * 2U;
	cedrus_h264_put_ue(writer, code_num);
}

static size_t cedrus_h264_finish_rbsp(struct cedrus_h264_bit_writer *writer)
{
	cedrus_h264_put_bits(writer, 1, 1);
	while (writer->bitpos % 8)
		cedrus_h264_put_bits(writer, 0, 1);

	return writer->overflow ? 0 : writer->bitpos / 8;
}

static int cedrus_h264_append_nal(u8 nal_header,
				  const struct cedrus_h264_bit_writer *writer,
				 size_t rbsp_size, u8 *dst, size_t capacity,
				 size_t *offset)
{
	size_t i;
	unsigned int zeroes = 0;

	if (*offset > capacity || capacity - *offset < 5)
		return -ENOSPC;
	dst[(*offset)++] = 0;
	dst[(*offset)++] = 0;
	dst[(*offset)++] = 0;
	dst[(*offset)++] = 1;
	dst[(*offset)++] = nal_header;

	for (i = 0; i < rbsp_size; i++) {
		u8 byte = writer->data[i];

		if (zeroes == 2 && byte <= 3) {
			if (*offset == capacity)
				return -ENOSPC;
			dst[(*offset)++] = 3;
			zeroes = 0;
		}
		if (*offset == capacity)
			return -ENOSPC;
		dst[(*offset)++] = byte;
		zeroes = byte ? 0 : zeroes + 1;
	}

	return 0;
}

int cedrus_h264_enc_controls_prepare(const struct cedrus_h264_enc_controls *controls,
				     struct cedrus_h264_enc_params *params)
{
	if (!controls || !params || !controls->gop_size)
		return -EINVAL;

	/*
	 * This is the exact independently exercised A133 combination.  Other
	 * profiles, entropy modes and hardware rate-control mappings remain
	 * rejected until a paired device trace isolates their packed fields.
	 * V4L2 defines fixed per-frame-type QP through disabled frame rate
	 * control; bitrate mode is irrelevant in that state.
	 */
	if (controls->profile != V4L2_MPEG_VIDEO_H264_PROFILE_HIGH ||
	    controls->level != V4L2_MPEG_VIDEO_H264_LEVEL_5_1 ||
	    controls->entropy_mode != V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC ||
	    controls->frame_rc_enable || controls->b_frames ||
	    controls->i_qp > 51 ||
	    controls->p_qp > 51 || controls->i_qp != controls->p_qp)
		return -EOPNOTSUPP;

	*params = (struct cedrus_h264_enc_params) {
		.profile_idc = 100,
		.constraint_flags = 0,
		.level_idc = 51,
		.qp = controls->i_qp,
		.cabac = true,
		.gop_size = controls->gop_size,
	};

	return 0;
}

int cedrus_h264_enc_headers(const struct cedrus_h264_enc_params *params,
			    u32 width, u32 height, u8 *dst, size_t capacity,
			    size_t *bytes_used)
{
	struct cedrus_h264_bit_writer writer = {};
	u32 coded_width, coded_height;
	u32 crop_right, crop_bottom;
	size_t rbsp_size, offset = 0;
	int ret;

	if (!params || !dst || !bytes_used || !width || !height)
		return -EINVAL;
	if (width > CEDRUS_H264_MAX_DIMENSION ||
	    height > CEDRUS_H264_MAX_DIMENSION)
		return -ERANGE;
	if (width % 8 || height % 8 || params->profile_idc != 100 ||
	    params->constraint_flags || params->level_idc != 51 ||
	    !params->cabac || params->qp > 51 || !params->gop_size)
		return -EOPNOTSUPP;

	coded_width = ALIGN(width, 16);
	coded_height = ALIGN(height, 16);
	if (coded_width / 16 * (coded_height / 16) >
	    CEDRUS_H264_LEVEL_5_1_MAX_MBS)
		return -ERANGE;
	crop_right = (coded_width - width) / 2;
	crop_bottom = (coded_height - height) / 2;

	/* SPS: stock-compatible High-profile, progressive 4:2:0 syntax. */
	cedrus_h264_put_bits(&writer, params->profile_idc, 8);
	cedrus_h264_put_bits(&writer, params->constraint_flags, 8);
	cedrus_h264_put_bits(&writer, params->level_idc, 8);
	cedrus_h264_put_ue(&writer, 0); /* seq_parameter_set_id */
	cedrus_h264_put_ue(&writer, 1); /* chroma_format_idc: 4:2:0 */
	cedrus_h264_put_ue(&writer, 0); /* bit_depth_luma_minus8 */
	cedrus_h264_put_ue(&writer, 0); /* bit_depth_chroma_minus8 */
	cedrus_h264_put_bits(&writer, 0, 1); /* transform bypass */
	cedrus_h264_put_bits(&writer, 0, 1); /* scaling matrices */
	cedrus_h264_put_ue(&writer, 4); /* eight-bit frame_num */
	cedrus_h264_put_ue(&writer, 0); /* pic_order_cnt_type */
	cedrus_h264_put_ue(&writer, 4); /* eight-bit POC LSB */
	cedrus_h264_put_ue(&writer, 1); /* one reference frame */
	cedrus_h264_put_bits(&writer, 0, 1); /* no frame_num gaps */
	cedrus_h264_put_ue(&writer, coded_width / 16 - 1);
	cedrus_h264_put_ue(&writer, coded_height / 16 - 1);
	cedrus_h264_put_bits(&writer, 1, 1); /* frame_mbs_only_flag */
	cedrus_h264_put_bits(&writer, 0, 1); /* direct_8x8_inference */
	cedrus_h264_put_bits(&writer, crop_right || crop_bottom, 1);
	if (crop_right || crop_bottom) {
		cedrus_h264_put_ue(&writer, 0);
		cedrus_h264_put_ue(&writer, crop_right);
		cedrus_h264_put_ue(&writer, 0);
		cedrus_h264_put_ue(&writer, crop_bottom);
	}
	cedrus_h264_put_bits(&writer, 0, 1); /* no VUI yet */
	rbsp_size = cedrus_h264_finish_rbsp(&writer);
	if (!rbsp_size)
		return -EOVERFLOW;
	ret = cedrus_h264_append_nal(0x67, &writer, rbsp_size,
				     dst, capacity, &offset);
	if (ret)
		return ret;

	memset(&writer, 0, sizeof(writer));
	cedrus_h264_put_ue(&writer, 0); /* pic_parameter_set_id */
	cedrus_h264_put_ue(&writer, 0); /* seq_parameter_set_id */
	cedrus_h264_put_bits(&writer, 1, 1); /* CABAC */
	cedrus_h264_put_bits(&writer, 0, 1); /* progressive picture order */
	cedrus_h264_put_ue(&writer, 0); /* one slice group */
	cedrus_h264_put_ue(&writer, 0); /* one list-0 reference */
	cedrus_h264_put_ue(&writer, 0); /* one list-1 reference */
	cedrus_h264_put_bits(&writer, 0, 1); /* weighted prediction */
	cedrus_h264_put_bits(&writer, 0, 2); /* weighted bipred */
	cedrus_h264_put_se(&writer, 0); /* pic_init_qp_minus26 */
	cedrus_h264_put_se(&writer, 0); /* pic_init_qs_minus26 */
	cedrus_h264_put_se(&writer, 0); /* chroma_qp_index_offset */
	cedrus_h264_put_bits(&writer, 1, 1); /* deblocking controls */
	cedrus_h264_put_bits(&writer, 0, 1); /* constrained intra */
	cedrus_h264_put_bits(&writer, 0, 1); /* redundant picture count */
	cedrus_h264_put_bits(&writer, 0, 1); /* transform_8x8_mode */
	cedrus_h264_put_bits(&writer, 0, 1); /* scaling matrices */
	cedrus_h264_put_se(&writer, 0); /* second chroma QP offset */
	rbsp_size = cedrus_h264_finish_rbsp(&writer);
	if (!rbsp_size)
		return -EOVERFLOW;
	ret = cedrus_h264_append_nal(0x68, &writer, rbsp_size,
				     dst, capacity, &offset);
	if (ret)
		return ret;

	*bytes_used = offset;
	return 0;
}

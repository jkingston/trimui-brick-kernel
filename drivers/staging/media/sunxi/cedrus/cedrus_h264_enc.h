/* SPDX-License-Identifier: GPL-2.0 */
/* Standard H.264 bytestream and V4L2-control helpers for Cedrus encoders. */

#ifndef _CEDRUS_H264_ENC_H_
#define _CEDRUS_H264_ENC_H_

#include <linux/types.h>

struct cedrus_h264_enc_controls {
	u32 profile;
	u32 level;
	u32 entropy_mode;
	u32 gop_size;
	u32 i_qp;
	u32 p_qp;
	u32 b_frames;
	bool frame_rc_enable;
};

struct cedrus_h264_enc_params {
	u8 profile_idc;
	u8 constraint_flags;
	u8 level_idc;
	u8 qp;
	bool cabac;
	u32 gop_size;
};

#define CEDRUS_H264_ENC_MAX_PREFIXES 3

struct cedrus_h264_enc_prefix {
	u16 byte_offset;
	u8 nal_header;
};

struct cedrus_h264_enc_prefix_map {
	struct cedrus_h264_enc_prefix entries[CEDRUS_H264_ENC_MAX_PREFIXES];
	u8 count;
};

int cedrus_h264_enc_controls_prepare(const struct cedrus_h264_enc_controls *controls,
				     struct cedrus_h264_enc_params *params);

int cedrus_h264_enc_headers(const struct cedrus_h264_enc_params *params,
			    u32 width, u32 height, u8 *dst, size_t capacity,
			    size_t *bytes_used);

int cedrus_h264_enc_repair_start_codes(u8 *data, size_t bytes,
				       const struct cedrus_h264_enc_prefix_map *prefixes);

#endif /* _CEDRUS_H264_ENC_H_ */

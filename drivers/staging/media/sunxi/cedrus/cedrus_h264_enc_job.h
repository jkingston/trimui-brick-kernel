/* SPDX-License-Identifier: GPL-2.0 */
/* Fail-closed A133 H.264 encoder job planning. */

#ifndef _CEDRUS_H264_ENC_JOB_H_
#define _CEDRUS_H264_ENC_JOB_H_

#include "cedrus_h264_enc.h"
#include "cedrus_h264_enc_hw.h"

/*
 * DMA allocations owned by one encoder context.  Every address is a byte
 * address; the planner applies the A133's 256-byte register encoding.
 */
struct cedrus_a133_h264_job_buffers {
	dma_addr_t source;
	size_t source_size;
	dma_addr_t bitstream;
	size_t bitstream_size;
	dma_addr_t reconstruction[2];
	dma_addr_t subpixel[2];
	dma_addr_t motion_vectors;
	dma_addr_t macroblock_info;
};

struct cedrus_a133_h264_job_request {
	u32 pixelformat;
	u32 width;
	u32 height;
	u32 bytesperline;
	struct cedrus_a133_h264_slice_header slice;
};

/*
 * A complete image of every currently proven part of one A133 H.264 job.
 * There is deliberately no function which triggers this plan: unresolved
 * b9c/bc4 auxiliary-buffer contracts still keep the backend closed.
 */
struct cedrus_a133_h264_job_plan {
	struct cedrus_a133_h264_enc_buffer_layout layout;
	struct cedrus_a133_h264_enc_frame_slot slots[2];
	struct cedrus_a133_h264_enc_dma dma;
	struct cedrus_a133_h264_enc_codec codec;
	struct cedrus_a133_h264_vlc_sequence parameter_sets;
	struct cedrus_a133_h264_vlc_sequence slice_header;
	bool emit_parameter_sets;
};

int cedrus_a133_h264_job_plan_prepare(struct cedrus_a133_h264_enc_rotation *rotation,
				      const struct cedrus_h264_enc_params *params,
				      const struct cedrus_a133_h264_job_request *request,
				      const struct cedrus_a133_h264_job_buffers *buffers,
				      struct cedrus_a133_h264_job_plan *plan);

#endif /* _CEDRUS_H264_ENC_JOB_H_ */

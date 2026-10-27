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
	size_t reconstruction_size[2];
	dma_addr_t subpixel[2];
	size_t subpixel_size[2];
	dma_addr_t motion_vectors;
	size_t motion_vectors_size;
	dma_addr_t macroblock_info;
	size_t macroblock_info_size;
	dma_addr_t work_9c;
	size_t work_9c_size;
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
 * There is deliberately no function which triggers this plan: reset, abort
 * and fault containment still keep the backend closed.
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

/* Metadata published only after a terminal hardware status is consumed. */
struct cedrus_a133_h264_job_completion {
	size_t payload;
	u32 status;
	bool keyframe;
	bool error;
};

int cedrus_a133_h264_job_plan_prepare(struct cedrus_a133_h264_enc_rotation *rotation,
				      const struct cedrus_h264_enc_params *params,
				      const struct cedrus_a133_h264_job_request *request,
				      const struct cedrus_a133_h264_job_buffers *buffers,
				      struct cedrus_a133_h264_job_plan *plan);

int cedrus_a133_h264_job_complete(struct cedrus_a133_h264_enc_rotation *rotation,
				  size_t capture_capacity, u32 status,
				  u32 produced_bits,
				  struct cedrus_a133_h264_job_completion *completion);

#endif /* _CEDRUS_H264_ENC_JOB_H_ */

/* SPDX-License-Identifier: GPL-2.0 */
/* Fail-closed A133 JPEG encoder job planning. */

#ifndef _CEDRUS_JPEG_ENC_JOB_H_
#define _CEDRUS_JPEG_ENC_JOB_H_

#include "cedrus_jpeg_enc_hw.h"

struct cedrus_a133_jpeg_job_buffers {
	dma_addr_t source;
	size_t source_size;
	dma_addr_t bitstream;
	size_t bitstream_size;
};

struct cedrus_a133_jpeg_job_request {
	u32 pixelformat;
	u32 width;
	u32 height;
	u32 bytesperline;
	u32 quality;
};

enum cedrus_a133_jpeg_job_state {
	CEDRUS_A133_JPEG_JOB_IDLE,
	CEDRUS_A133_JPEG_JOB_PREPARED,
	CEDRUS_A133_JPEG_JOB_RUNNING,
	CEDRUS_A133_JPEG_JOB_RESET_REQUIRED,
	CEDRUS_A133_JPEG_JOB_SUSPENDED,
};

struct cedrus_a133_jpeg_job_lifecycle {
	u64 generation;
	u64 active_generation;
	u64 recovery_generation;
	enum cedrus_a133_jpeg_job_state state;
};

struct cedrus_a133_jpeg_job_plan {
	struct cedrus_jpeg_source source;
	struct cedrus_jpeg_capture capture;
	struct cedrus_jpeg_tables tables;
	struct cedrus_a133_jpeg_codec codec;
	struct cedrus_a133_jpeg_isp isp;
	u8 header[CEDRUS_JPEG_HEADER_SIZE];
	u64 generation;
};

struct cedrus_a133_jpeg_job_completion {
	size_t payload;
	u32 status;
	bool error;
};

void cedrus_a133_jpeg_job_init(struct cedrus_a133_jpeg_job_lifecycle *lifecycle);
int cedrus_a133_jpeg_job_prepare(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
				 const struct cedrus_a133_jpeg_job_request *request,
				 const struct cedrus_a133_jpeg_job_buffers *buffers,
				 struct cedrus_a133_jpeg_job_plan *plan);
int cedrus_a133_jpeg_job_trigger(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
				 void __iomem *base,
				 const struct cedrus_a133_jpeg_job_plan *plan);
int cedrus_a133_jpeg_job_complete(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
				  u64 generation,
				  const struct cedrus_a133_jpeg_job_plan *plan,
				  u32 status, u32 produced_bits,
				  struct cedrus_a133_jpeg_job_completion *completion);
int cedrus_a133_jpeg_job_abort(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
			       u64 generation);
int cedrus_a133_jpeg_job_reset_complete(struct cedrus_a133_jpeg_job_lifecycle *lifecycle);
int cedrus_a133_jpeg_job_suspend(struct cedrus_a133_jpeg_job_lifecycle *lifecycle);
int cedrus_a133_jpeg_job_resume(struct cedrus_a133_jpeg_job_lifecycle *lifecycle);

#endif /* _CEDRUS_JPEG_ENC_JOB_H_ */

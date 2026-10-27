// SPDX-License-Identifier: GPL-2.0
/* Fail-closed A133 JPEG encoder job planning. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>

#include "cedrus_jpeg_enc_job.h"

#define CEDRUS_A133_JPEG_CAPTURE_WIDTH	320U
#define CEDRUS_A133_JPEG_CAPTURE_HEIGHT	240U
#define CEDRUS_A133_JPEG_CAPTURE_QUALITY	95U
#define CEDRUS_A133_JPEG_CAPTURE_SIZE	(4U * 1024U * 1024U)

struct cedrus_a133_jpeg_dma_range {
	dma_addr_t start;
	size_t size;
};

static int
cedrus_a133_jpeg_dma_ranges_validate(const struct cedrus_a133_jpeg_dma_range *ranges,
				     unsigned int count)
{
	dma_addr_t ends[2];
	unsigned int i, j;

	if (count > ARRAY_SIZE(ends))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!ranges[i].size || !IS_ALIGNED(ranges[i].start, 16))
			return -EINVAL;
		if (check_add_overflow(ranges[i].start,
				       (dma_addr_t)ranges[i].size, &ends[i]))
			return -EOVERFLOW;
		if (ends[i] - 1 > U32_MAX)
			return -ERANGE;
		for (j = 0; j < i; j++)
			if (ranges[i].start < ends[j] &&
			    ranges[j].start < ends[i])
				return -EINVAL;
	}

	return 0;
}

void cedrus_a133_jpeg_job_init(struct cedrus_a133_jpeg_job_lifecycle *lifecycle)
{
	if (lifecycle)
		*lifecycle = (struct cedrus_a133_jpeg_job_lifecycle) {};
}

int cedrus_a133_jpeg_job_prepare(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
				 const struct cedrus_a133_jpeg_job_request *request,
				 const struct cedrus_a133_jpeg_job_buffers *buffers,
				 struct cedrus_a133_jpeg_job_plan *plan)
{
	struct cedrus_a133_jpeg_job_lifecycle next_lifecycle;
	struct cedrus_a133_jpeg_job_plan next = {};
	struct cedrus_a133_jpeg_dma_range ranges[2];
	int ret;

	if (!lifecycle || !request || !buffers || !plan)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_IDLE ||
	    lifecycle->active_generation)
		return -EBUSY;
	/* This is the only complete main-frame profile captured on the Brick. */
	if (request->width != CEDRUS_A133_JPEG_CAPTURE_WIDTH ||
	    request->height != CEDRUS_A133_JPEG_CAPTURE_HEIGHT ||
	    request->quality != CEDRUS_A133_JPEG_CAPTURE_QUALITY ||
	    buffers->bitstream_size != CEDRUS_A133_JPEG_CAPTURE_SIZE)
		return -EOPNOTSUPP;

	ret = cedrus_jpeg_prepare_source(&next.source, request->pixelformat,
					 request->width, request->height, request->bytesperline,
			buffers->source_size, buffers->source);
	if (ret)
		return ret;
	ret = cedrus_jpeg_prepare_capture(&next.capture, buffers->bitstream,
					  buffers->bitstream_size, 0);
	if (ret)
		return ret;
	ranges[0] = (struct cedrus_a133_jpeg_dma_range) {
		.start = buffers->source, .size = buffers->source_size,
	};
	ranges[1] = (struct cedrus_a133_jpeg_dma_range) {
		.start = buffers->bitstream, .size = buffers->bitstream_size,
	};
	ret = cedrus_a133_jpeg_dma_ranges_validate(ranges, ARRAY_SIZE(ranges));
	if (ret)
		return ret;

	ret = cedrus_jpeg_prepare_tables(&next.tables, request->quality);
	if (ret)
		return ret;
	ret = cedrus_jpeg_write_header(next.header, sizeof(next.header),
				       request->width, request->height,
				       &next.tables);
	if (ret != CEDRUS_JPEG_HEADER_SIZE)
		return ret < 0 ? ret : -EIO;
	ret = cedrus_a133_jpeg_codec_prepare(request->width, request->height,
					     request->quality, &next.tables, &next.codec);
	if (ret)
		return ret;
	ret = cedrus_a133_jpeg_isp_prepare(&next.source, &next.isp);
	if (ret)
		return ret;

	next_lifecycle = *lifecycle;
	next_lifecycle.generation++;
	if (!next_lifecycle.generation)
		next_lifecycle.generation++;
	next_lifecycle.active_generation = next_lifecycle.generation;
	next_lifecycle.recovery_generation = 0;
	next_lifecycle.state = CEDRUS_A133_JPEG_JOB_PREPARED;
	next.generation = next_lifecycle.generation;

	*plan = next;
	*lifecycle = next_lifecycle;
	return 0;
}

int cedrus_a133_jpeg_job_trigger(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
				 void __iomem *base,
				 const struct cedrus_a133_jpeg_job_plan *plan)
{
	int ret;

	if (!lifecycle || !base || !plan || !plan->generation)
		return -EINVAL;
	if (plan->generation != lifecycle->active_generation)
		return -ESTALE;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_PREPARED)
		return -EBUSY;

	cedrus_a133_jpeg_select(base);
	cedrus_a133_jpeg_program_isp(base, &plan->isp);
	ret = cedrus_a133_jpeg_prepare_phase(base, &plan->codec, &plan->capture,
					     &plan->tables, plan->header,
					    sizeof(plan->header));
	if (ret)
		return ret;

	/* Publish RUNNING before the one final command that can raise an IRQ. */
	lifecycle->state = CEDRUS_A133_JPEG_JOB_RUNNING;
	cedrus_a133_jpeg_trigger(base);
	return 0;
}

int cedrus_a133_jpeg_job_complete(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
				  u64 generation,
				  const struct cedrus_a133_jpeg_job_plan *plan,
				  u32 status, u32 produced_bits,
				  struct cedrus_a133_jpeg_job_completion *completion)
{
	struct cedrus_a133_jpeg_job_completion next = { .status = status };
	int ret;

	if (!lifecycle || !generation || !plan || !completion)
		return -EINVAL;
	if (generation != lifecycle->active_generation ||
	    generation != plan->generation)
		return -ESTALE;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_RUNNING)
		return -EBUSY;
	if (!(status & (CEDRUS_JPEG_STATUS_FINISH |
			CEDRUS_JPEG_STATUS_BUFFER_FULL |
			CEDRUS_JPEG_STATUS_MB_OVERFLOW |
			CEDRUS_JPEG_STATUS_TIMEOUT)))
		return -EAGAIN;

	ret = cedrus_jpeg_validate_capture(&plan->capture, status,
					   produced_bits, &next.payload);
	if (!ret && produced_bits < CEDRUS_JPEG_HEADER_SIZE * 8U)
		ret = -ENODATA;
	if (ret)
		next.payload = 0;
	next.error = ret != 0;
	lifecycle->recovery_generation = ret ? generation : 0;
	lifecycle->active_generation = 0;
	lifecycle->state = ret ? CEDRUS_A133_JPEG_JOB_RESET_REQUIRED :
				       CEDRUS_A133_JPEG_JOB_IDLE;
	*completion = next;
	return ret;
}

int cedrus_a133_jpeg_job_abort(struct cedrus_a133_jpeg_job_lifecycle *lifecycle,
			       u64 generation)
{
	if (!lifecycle || !generation)
		return -EINVAL;
	if (lifecycle->state == CEDRUS_A133_JPEG_JOB_RESET_REQUIRED &&
	    lifecycle->recovery_generation == generation)
		return 0;
	if (generation != lifecycle->active_generation)
		return -ESTALE;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_PREPARED &&
	    lifecycle->state != CEDRUS_A133_JPEG_JOB_RUNNING)
		return -EBUSY;

	lifecycle->recovery_generation = generation;
	lifecycle->active_generation = 0;
	lifecycle->state = CEDRUS_A133_JPEG_JOB_RESET_REQUIRED;
	return 0;
}

int cedrus_a133_jpeg_job_reset_complete(struct cedrus_a133_jpeg_job_lifecycle *lifecycle)
{
	if (!lifecycle)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_RESET_REQUIRED ||
	    lifecycle->active_generation)
		return -EBUSY;

	lifecycle->recovery_generation = 0;
	lifecycle->state = CEDRUS_A133_JPEG_JOB_IDLE;
	return 0;
}

int cedrus_a133_jpeg_job_suspend(struct cedrus_a133_jpeg_job_lifecycle *lifecycle)
{
	if (!lifecycle)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_IDLE ||
	    lifecycle->active_generation)
		return -EBUSY;
	lifecycle->state = CEDRUS_A133_JPEG_JOB_SUSPENDED;
	return 0;
}

int cedrus_a133_jpeg_job_resume(struct cedrus_a133_jpeg_job_lifecycle *lifecycle)
{
	if (!lifecycle)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_JPEG_JOB_SUSPENDED ||
	    lifecycle->active_generation)
		return -EBUSY;
	lifecycle->state = CEDRUS_A133_JPEG_JOB_IDLE;
	return 0;
}

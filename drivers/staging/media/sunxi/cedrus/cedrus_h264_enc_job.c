// SPDX-License-Identifier: GPL-2.0
/* Fail-closed A133 H.264 encoder job planning. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>

#include "cedrus_h264_enc_job.h"

static int
cedrus_a133_h264_source_buffer_prepare(const struct cedrus_a133_h264_job_request *request,
				       const struct cedrus_a133_h264_job_buffers *buffers,
				       struct cedrus_a133_enc_source *source,
				       size_t *used)
{
	dma_addr_t chroma;
	dma_addr_t last;
	size_t luma_size;
	size_t source_size;

	if (check_mul_overflow((size_t)request->bytesperline,
			       (size_t)request->height, &luma_size) ||
	    check_mul_overflow(luma_size, (size_t)3, &source_size))
		return -EOVERFLOW;
	source_size /= 2;
	if (!source_size)
		return -EINVAL;
	if (buffers->source_size < source_size)
		return -ENOSPC;
	if (check_add_overflow(buffers->source, (dma_addr_t)source_size - 1,
			       &last))
		return -EOVERFLOW;
	if (last > U32_MAX)
		return -ERANGE;
	if (check_add_overflow(buffers->source, (dma_addr_t)luma_size,
			       &chroma))
		return -EOVERFLOW;

	*used = source_size;
	return cedrus_a133_h264_enc_source_prepare(request->pixelformat,
			request->width, request->height, request->bytesperline,
			buffers->source, chroma, source);
}

struct cedrus_a133_h264_dma_range {
	dma_addr_t start;
	dma_addr_t end;
};

static int
cedrus_a133_h264_dma_ranges_validate(const struct cedrus_a133_h264_job_buffers *buffers,
				     const struct cedrus_a133_h264_enc_buffer_layout *layout,
				     size_t source_size)
{
	struct cedrus_a133_h264_dma_range ranges[] = {
		{ .start = buffers->source },
		{ .start = buffers->bitstream },
		{ .start = buffers->reconstruction[0] },
		{ .start = buffers->reconstruction[1] },
		{ .start = buffers->subpixel[0] },
		{ .start = buffers->subpixel[1] },
		{ .start = buffers->motion_vectors },
		{ .start = buffers->macroblock_info },
		{ .start = buffers->work_9c },
	};
	const size_t sizes[] = {
		buffers->source_size,
		buffers->bitstream_size,
		buffers->reconstruction_size[0],
		buffers->reconstruction_size[1],
		buffers->subpixel_size[0],
		buffers->subpixel_size[1],
		buffers->motion_vectors_size,
		buffers->macroblock_info_size,
		buffers->work_9c_size,
	};
	const size_t required[] = {
		source_size,
		buffers->bitstream_size,
		layout->reconstruction,
		layout->reconstruction,
		layout->subpixel,
		layout->subpixel,
		layout->motion_vectors,
		layout->macroblock_info,
		layout->work_9c,
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(ranges); i++) {
		if (sizes[i] < required[i])
			return -ENOSPC;
		if (check_add_overflow(ranges[i].start, (dma_addr_t)sizes[i],
				       &ranges[i].end))
			return -EOVERFLOW;
		for (j = 0; j < i; j++)
			if (ranges[i].start < ranges[j].end &&
			    ranges[j].start < ranges[i].end)
				return -EINVAL;
	}

	return 0;
}

int cedrus_a133_h264_job_plan_prepare(struct cedrus_a133_h264_job_lifecycle *lifecycle,
				      struct cedrus_a133_h264_enc_rotation *rotation,
				      const struct cedrus_h264_enc_params *params,
				      const struct cedrus_a133_h264_job_request *request,
				      const struct cedrus_a133_h264_job_buffers *buffers,
				      struct cedrus_a133_h264_job_plan *plan)
{
	struct cedrus_a133_h264_job_lifecycle next_lifecycle;
	struct cedrus_a133_h264_enc_rotation next_rotation;
	struct cedrus_a133_h264_enc_frame_slot reference;
	struct cedrus_a133_h264_enc_frame_slot reconstruction;
	struct cedrus_a133_h264_job_plan next = {};
	struct cedrus_a133_enc_bitstream bitstream;
	struct cedrus_a133_enc_source source;
	size_t source_size;
	unsigned int i;
	int ret;

	if (!lifecycle || !rotation || !params || !request || !buffers || !plan)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_IDLE)
		return -EBUSY;
	if (request->slice.qp != params->qp)
		return -EINVAL;
	if (request->slice.idr) {
		/* Only the independently observed startup IDR is admitted. */
		if (rotation->started || request->slice.frame_num ||
		    request->slice.pic_order_cnt_lsb ||
		    rotation->next_frame_num)
			return -EOPNOTSUPP;
	} else if (!rotation->started) {
		return -EINVAL;
	} else if (request->slice.frame_num != rotation->next_frame_num ||
		   request->slice.pic_order_cnt_lsb !=
		   (u8)(rotation->next_frame_num * 2)) {
		/* The emitted SPS forbids gaps and uses an eight-bit type-0 POC. */
		return -EINVAL;
	}

	ret = cedrus_a133_h264_source_buffer_prepare(request, buffers,
						     &source, &source_size);
	if (ret)
		return ret;
	ret = cedrus_a133_h264_enc_bitstream_prepare(buffers->bitstream,
						     buffers->bitstream_size,
						     &bitstream);
	if (ret)
		return ret;
	ret = cedrus_a133_h264_enc_buffer_layout(request->width,
						 request->height, &next.layout);
	if (ret)
		return ret;
	ret = cedrus_a133_h264_dma_ranges_validate(buffers, &next.layout,
						   source_size);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(next.slots); i++) {
		struct cedrus_a133_h264_enc_frame_slot *slot = &next.slots[i];
		dma_addr_t rec = buffers->reconstruction[i];
		dma_addr_t sub = buffers->subpixel[i];

		ret = cedrus_a133_h264_enc_frame_slot_prepare(rec, sub, &next.layout, slot);
		if (ret)
			return ret;
	}

	next_rotation = *rotation;
	ret = cedrus_a133_h264_frame_prepare(&next_rotation, next.slots,
					     request->slice.idr, &reference,
					     &reconstruction);
	if (ret)
		return ret;
	ret = cedrus_a133_h264_dma_prepare(&source, &bitstream, &reference,
					   &reconstruction,
					   buffers->motion_vectors,
					   buffers->macroblock_info,
					   buffers->work_9c,
					   &next.layout, &next.dma);
	if (ret)
		return ret;
	ret = cedrus_a133_h264_codec_prepare(request->width, request->height,
					     params->qp, request->slice.idr,
					     &next.codec);
	if (ret)
		return ret;

	next.emit_parameter_sets = !rotation->started;
	if (next.emit_parameter_sets) {
		struct cedrus_a133_h264_vlc_sequence *sets = &next.parameter_sets;
		u32 width = request->width;
		u32 height = request->height;

		ret = cedrus_a133_h264_headers_prepare(params, width, height, sets);
		if (ret)
			return ret;
	}
	ret = cedrus_a133_h264_slice_header_prepare(params, &request->slice,
						    &next.slice_header);
	if (ret)
		return ret;

	next_lifecycle = *lifecycle;
	next_lifecycle.generation++;
	if (!next_lifecycle.generation)
		next_lifecycle.generation++;
	next_lifecycle.active_generation = next_lifecycle.generation;
	next_lifecycle.state = CEDRUS_A133_H264_JOB_PREPARED;
	next.generation = next_lifecycle.generation;

	/* Publish every part of the prepared generation as one transaction. */
	*rotation = next_rotation;
	*plan = next;
	*lifecycle = next_lifecycle;
	return 0;
}

int cedrus_a133_h264_job_trigger(struct cedrus_a133_h264_job_lifecycle *lifecycle,
				 void __iomem *base, u64 generation)
{
	if (!lifecycle || !base || !generation)
		return -EINVAL;
	if (generation != lifecycle->active_generation)
		return -ESTALE;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_PREPARED)
		return -EBUSY;

	/* Clear stale W1C status before making an immediate completion possible. */
	cedrus_a133_h264_irq_arm(base);
	lifecycle->state = CEDRUS_A133_H264_JOB_RUNNING;
	cedrus_a133_h264_trigger(base);

	return 0;
}

int cedrus_a133_h264_job_complete(struct cedrus_a133_h264_job_lifecycle *lifecycle,
				  struct cedrus_a133_h264_enc_rotation *rotation,
				  u64 generation,
				  size_t capture_capacity, u32 status,
				  u32 produced_bits,
				  struct cedrus_a133_h264_job_completion *completion)
{
	struct cedrus_a133_h264_job_lifecycle next_lifecycle;
	struct cedrus_a133_h264_enc_rotation next_rotation;
	struct cedrus_a133_h264_job_completion next = {
		.status = status,
	};
	enum cedrus_enc_irq_status irq;
	int ret;

	if (!lifecycle || !rotation || !completion || !generation ||
	    !capture_capacity)
		return -EINVAL;
	if (generation != lifecycle->active_generation)
		return -ESTALE;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_RUNNING)
		return -EBUSY;
	if (!rotation->pending)
		return -EINVAL;

	irq = cedrus_a133_h264_enc_irq_status(status);
	if (irq == CEDRUS_ENC_IRQ_NONE)
		return -EAGAIN;

	next_rotation = *rotation;
	if (irq == CEDRUS_ENC_IRQ_ERROR) {
		next.error = true;
		ret = -EIO;
	} else if (!produced_bits) {
		next.error = true;
		ret = -ENODATA;
	} else if (!IS_ALIGNED(produced_bits, 8)) {
		/* A complete H.264 access unit includes byte-alignment bits. */
		next.error = true;
		ret = -EPROTO;
	} else if ((size_t)(produced_bits / 8) > capture_capacity) {
		next.error = true;
		ret = -ENOSPC;
	} else {
		next.payload = produced_bits / 8;
		next.keyframe = rotation->pending_keyframe;
		ret = 0;
	}

	if (cedrus_a133_h264_frame_complete(&next_rotation, !ret))
		return -EINVAL;
	next_lifecycle = *lifecycle;
	next_lifecycle.active_generation = 0;
	next_lifecycle.state = ret ? CEDRUS_A133_H264_JOB_RESET_REQUIRED :
				       CEDRUS_A133_H264_JOB_IDLE;

	/* Retire the pending reference and metadata as one transaction. */
	*rotation = next_rotation;
	*completion = next;
	*lifecycle = next_lifecycle;
	return ret;
}

int cedrus_a133_h264_job_abort(struct cedrus_a133_h264_job_lifecycle *lifecycle,
			       struct cedrus_a133_h264_enc_rotation *rotation,
			       u64 generation)
{
	struct cedrus_a133_h264_enc_rotation next_rotation;

	if (!lifecycle || !rotation || !generation)
		return -EINVAL;
	if (generation != lifecycle->active_generation)
		return -ESTALE;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_PREPARED &&
	    lifecycle->state != CEDRUS_A133_H264_JOB_RUNNING)
		return -EBUSY;
	if (!rotation->pending)
		return -EINVAL;

	next_rotation = *rotation;
	if (cedrus_a133_h264_frame_complete(&next_rotation, false))
		return -EINVAL;

	/* The caller must keep IRQs quiesced until reset has completed. */
	*rotation = next_rotation;
	lifecycle->active_generation = 0;
	lifecycle->state = CEDRUS_A133_H264_JOB_RESET_REQUIRED;

	return 0;
}

int cedrus_a133_h264_job_reset_complete(struct cedrus_a133_h264_job_lifecycle *lifecycle)
{
	if (!lifecycle)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_RESET_REQUIRED ||
	    lifecycle->active_generation)
		return -EBUSY;

	lifecycle->state = CEDRUS_A133_H264_JOB_IDLE;
	return 0;
}

int cedrus_a133_h264_job_suspend(struct cedrus_a133_h264_job_lifecycle *lifecycle)
{
	if (!lifecycle)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_IDLE ||
	    lifecycle->active_generation)
		return -EBUSY;

	lifecycle->state = CEDRUS_A133_H264_JOB_SUSPENDED;
	return 0;
}

int cedrus_a133_h264_job_resume(struct cedrus_a133_h264_job_lifecycle *lifecycle)
{
	if (!lifecycle)
		return -EINVAL;
	if (lifecycle->state != CEDRUS_A133_H264_JOB_SUSPENDED ||
	    lifecycle->active_generation)
		return -EBUSY;

	lifecycle->state = CEDRUS_A133_H264_JOB_IDLE;
	return 0;
}

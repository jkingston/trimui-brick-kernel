// SPDX-License-Identifier: GPL-2.0-only
/* Implicit DMA-BUF synchronization for the A133 G2D engines. */
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/jiffies.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/ww_mutex.h>

#include "sun8i-rotate.h"

#define G2D_DMA_MAX_BUFFERS	3
#define G2D_DMA_WAIT_TIMEOUT	msecs_to_jiffies(5000)
#define G2D_DMA_WAIT_SLICE	msecs_to_jiffies(20)

struct g2d_dma_fence {
	struct dma_fence base;
	/* Protects the dma_fence signaling state. */
	spinlock_t lock;
};

struct g2d_dma_access {
	struct dma_resv *resv;
	bool write;
};

static const char *g2d_dma_fence_get_driver_name(struct dma_fence *fence)
{
	return "sun50i-g2d";
}

static const char *g2d_dma_fence_get_timeline_name(struct dma_fence *fence)
{
	return "a133-g2d";
}

static void g2d_dma_fence_release(struct dma_fence *fence)
{
	struct g2d_dma_fence *g2d_fence;

	g2d_fence = container_of(fence, struct g2d_dma_fence, base);
	kfree(g2d_fence);
}

static const struct dma_fence_ops g2d_dma_fence_ops = {
	.get_driver_name = g2d_dma_fence_get_driver_name,
	.get_timeline_name = g2d_dma_fence_get_timeline_name,
	.release = g2d_dma_fence_release,
};

static struct dma_fence *g2d_dma_fence_create(struct rotate_dev *dev)
{
	struct g2d_dma_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return NULL;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &g2d_dma_fence_ops, &fence->lock,
		       dev->fence_context, atomic64_inc_return(&dev->fence_seqno));

	return &fence->base;
}

static int g2d_dma_lock_reservations(struct g2d_dma_access *access,
				     unsigned int count,
				     struct ww_acquire_ctx *ctx)
{
	int contended = -1;
	unsigned int i;
	int ret;

	ww_acquire_init(ctx, &reservation_ww_class);
retry:
	if (contended >= 0) {
		ret = dma_resv_lock_slow_interruptible(access[contended].resv, ctx);
		if (ret)
			goto fini;
	}

	for (i = 0; i < count; i++) {
		unsigned int j;

		if ((int)i == contended)
			continue;
		ret = dma_resv_lock_interruptible(access[i].resv, ctx);
		if (!ret)
			continue;
		for (j = 0; j < i; j++)
			dma_resv_unlock(access[j].resv);
		if (contended >= (int)i)
			dma_resv_unlock(access[contended].resv);
		if (ret == -EDEADLK) {
			contended = i;
			goto retry;
		}
		goto fini;
	}
	ww_acquire_done(ctx);

	return 0;
fini:
	ww_acquire_fini(ctx);
	return ret;
}

static void g2d_dma_unlock_reservations(struct g2d_dma_access *access,
					unsigned int count,
					struct ww_acquire_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		dma_resv_unlock(access[i].resv);
	ww_acquire_fini(ctx);
}

static bool g2d_dma_ranges_overlap(struct vb2_buffer *a,
				   struct vb2_buffer *b)
{
	dma_addr_t a_start = vb2_dma_contig_plane_dma_addr(a, 0);
	dma_addr_t b_start = vb2_dma_contig_plane_dma_addr(b, 0);
	dma_addr_t a_end, b_end;

	if (check_add_overflow(a_start, vb2_get_plane_payload(a, 0), &a_end) ||
	    check_add_overflow(b_start, vb2_get_plane_payload(b, 0), &b_end))
		return true;

	return a_start < b_end && b_start < a_end;
}

int sun50i_g2d_dma_prepare(struct rotate_dev *dev, struct g2d_dma_job *job,
			   struct vb2_buffer **buffers,
			   unsigned int num_buffers,
			   unsigned int write_index)
{
	struct g2d_dma_access access[G2D_DMA_MAX_BUFFERS];
	struct ww_acquire_ctx ctx;
	struct dma_resv_iter cursor;
	struct dma_fence *fence;
	unsigned int count = 0, num_dependencies = 0;
	unsigned int i, j;
	int ret;

	if (!num_buffers || num_buffers > G2D_DMA_MAX_BUFFERS ||
	    write_index >= num_buffers)
		return -EINVAL;
	memset(job, 0, sizeof(*job));
	for (i = 0; i < num_buffers; i++) {
		struct dma_resv *resv;
		bool write = i == write_index;

		if (i != write_index &&
		    g2d_dma_ranges_overlap(buffers[i], buffers[write_index]))
			return -EINVAL;
		resv = vb2_dma_contig_plane_resv(buffers[i], 0);
		if (!resv)
			return -EINVAL;
		for (j = 0; j < count; j++)
			if (access[j].resv == resv)
				break;
		if (j == count) {
			access[count].resv = resv;
			access[count].write = write;
			count++;
		} else if (write) {
			access[j].write = true;
		}
	}

	job->fence = g2d_dma_fence_create(dev);
	if (!job->fence)
		return -ENOMEM;
	ret = g2d_dma_lock_reservations(access, count, &ctx);
	if (ret)
		goto put_fence;

	for (i = 0; i < count; i++)
		dma_resv_for_each_fence(&cursor, access[i].resv,
					dma_resv_usage_rw(access[i].write),
					fence)
			num_dependencies++;
	job->dependencies = kcalloc(num_dependencies,
				    sizeof(*job->dependencies), GFP_KERNEL);
	if (num_dependencies && !job->dependencies) {
		ret = -ENOMEM;
		goto unlock;
	}
	for (i = 0; i < count; i++)
		dma_resv_for_each_fence(&cursor, access[i].resv,
					dma_resv_usage_rw(access[i].write),
					fence)
			job->dependencies[job->num_dependencies++] =
				dma_fence_get(fence);

	for (i = 0; i < count; i++) {
		ret = dma_resv_reserve_fences(access[i].resv, 1);
		if (ret)
			goto unlock;
	}
	for (i = 0; i < count; i++)
		dma_resv_add_fence(access[i].resv, job->fence,
				   access[i].write ? DMA_RESV_USAGE_WRITE :
				   DMA_RESV_USAGE_READ);
	g2d_dma_unlock_reservations(access, count, &ctx);

	return 0;
unlock:
	g2d_dma_unlock_reservations(access, count, &ctx);
	for (i = 0; i < job->num_dependencies; i++)
		dma_fence_put(job->dependencies[i]);
	kfree(job->dependencies);
	job->dependencies = NULL;
	job->num_dependencies = 0;
put_fence:
	dma_fence_put(job->fence);
	job->fence = NULL;
	return ret;
}

int sun50i_g2d_dma_wait(struct g2d_dma_job *job,
			bool (*cancelled)(void *data), void *data)
{
	unsigned long deadline = jiffies + G2D_DMA_WAIT_TIMEOUT;
	unsigned int i;

	for (i = 0; i < job->num_dependencies; i++) {
		for (;;) {
			unsigned long timeout;
			long ret;

			if (cancelled(data))
				return -ECANCELED;
			if (time_after_eq(jiffies, deadline))
				return -ETIME;
			timeout = min(deadline - jiffies, G2D_DMA_WAIT_SLICE);
			ret = dma_fence_wait_timeout(job->dependencies[i], false,
						     max(timeout, 1UL));
			if (ret > 0)
				break;
			if (ret < 0)
				return ret;
		}
		if (dma_fence_get_status(job->dependencies[i]) < 0)
			return dma_fence_get_status(job->dependencies[i]);
	}

	return cancelled(data) ? -ECANCELED : 0;
}

void sun50i_g2d_dma_finish(struct g2d_dma_job *job, int error)
{
	unsigned int i;

	for (i = 0; i < job->num_dependencies; i++)
		dma_fence_put(job->dependencies[i]);
	kfree(job->dependencies);
	if (!job->fence)
		return;
	if (error)
		dma_fence_set_error(job->fence, error);
	dma_fence_signal(job->fence);
	dma_fence_put(job->fence);
	job->fence = NULL;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sun8i DE2 rotation driver
 *
 * Copyright (C) 2020 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>

#include "sun8i-formats.h"
#include "sun8i-rotate.h"

/* A133 file handles may outlive platform removal, but cannot touch hardware. */
static void rotate_a133_release(struct kref *ref)
{
	struct rotate_dev *dev = container_of(ref, struct rotate_dev, ref);

	if (!IS_ERR_OR_NULL(dev->m2m_dev))
		v4l2_m2m_release(dev->m2m_dev);
	put_device(dev->dev);
	kfree(dev);
}

void rotate_a133_put(void *data)
{
	struct rotate_dev *dev = data;

	kref_put(&dev->ref, rotate_a133_release);
}

static void rotate_video_release(struct video_device *vfd)
{
	struct rotate_dev *dev = container_of(vfd, struct rotate_dev, vfd);

	rotate_a133_put(dev);
}

static inline u32 rotate_read(struct rotate_dev *dev, u32 reg)
{
	return readl(dev->base + reg);
}

static inline void rotate_write(struct rotate_dev *dev, u32 reg, u32 value)
{
	writel(value, dev->base + reg);
}

static inline void rotate_set_bits(struct rotate_dev *dev, u32 reg, u32 bits)
{
	writel(readl(dev->base + reg) | bits, dev->base + reg);
}

void rotate_calc_addr_pitch(dma_addr_t buffer, u32 bytesperline, u32 height,
			    const struct rotate_format *fmt,
			    dma_addr_t *addr, u32 *pitch)
{
	u32 size;
	int i;

	for (i = 0; i < fmt->planes; i++) {
		pitch[i] = bytesperline;
		addr[i] = buffer;
		if (i > 0)
			pitch[i] /= fmt->hsub / fmt->bpp[i];
		size = pitch[i] * height;
		if (i > 0)
			size /= fmt->vsub;
		buffer += size;
	}
}

static bool rotate_uses_mixer(struct rotate_ctx *ctx)
{
	const struct rotate_format *in = rotate_find_format(ctx->src_fmt.pixelformat);
	const struct rotate_format *out = rotate_find_format(ctx->dst_fmt.pixelformat);

	if (!ctx->dev->a133 || ctx->rotate || ctx->hflip || ctx->vflip)
		return false;
	if (ctx->src_fmt.width != ctx->dst_fmt.width ||
	    ctx->src_fmt.height != ctx->dst_fmt.height ||
	    ((in->flags ^ out->flags) & ROTATE_FLAG_YUV))
		return true;
	/* Preserve the original rotator's YUV-to-planar-420 path. */
	if (in->flags & ROTATE_FLAG_YUV)
		return ctx->dst_fmt.pixelformat != V4L2_PIX_FMT_YUV420;
	return ctx->src_fmt.pixelformat != ctx->dst_fmt.pixelformat;
}

static int rotate_check_csc(struct rotate_ctx *ctx)
{
	const struct rotate_format *in = rotate_find_format(ctx->src_fmt.pixelformat);
	const struct rotate_format *out = rotate_find_format(ctx->dst_fmt.pixelformat);
	bool iyuv = in->flags & ROTATE_FLAG_YUV;
	bool oyuv = out->flags & ROTATE_FLAG_YUV;
	struct v4l2_pix_format *yuv = iyuv ? &ctx->src_fmt : &ctx->dst_fmt;
	struct v4l2_pix_format *rgb = iyuv ? &ctx->dst_fmt : &ctx->src_fmt;
	u32 enc = yuv->ycbcr_enc, yq = yuv->quantization, rq = rgb->quantization;

	if (!rotate_uses_mixer(ctx) || iyuv == oyuv)
		return 0;
	if (!enc)
		enc = V4L2_MAP_YCBCR_ENC_DEFAULT(yuv->colorspace);
	if (!yq)
		yq = V4L2_MAP_QUANTIZATION_DEFAULT(false, yuv->colorspace, enc);
	if (!rq)
		rq = V4L2_MAP_QUANTIZATION_DEFAULT(true, rgb->colorspace, enc);
	if ((enc != V4L2_YCBCR_ENC_601 && enc != V4L2_YCBCR_ENC_709) ||
	    yq != V4L2_QUANTIZATION_LIM_RANGE || rq != V4L2_QUANTIZATION_FULL_RANGE)
		return -EINVAL;
	return 0;
}

static void rotate_program(void *priv)
{
	struct rotate_ctx *ctx = priv;
	struct rotate_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst;
	const struct rotate_format *fmt;
	dma_addr_t addr[3] = {};
	u32 val, pitch[3] = {};

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_copy_metadata(src, dst);
	if (dev->a133 && dev->mixer_job) {
		sun50i_g2d_mixer(ctx,
			vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0),
			vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0));
		return;
	}

	val = ROTATE_GLB_CTL_MODE(ROTATE_MODE_COPY_ROTATE);
	if (ctx->hflip)
		val |= ROTATE_GLB_CTL_HFLIP;
	if (ctx->vflip)
		val |= ROTATE_GLB_CTL_VFLIP;
	val |= ROTATE_GLB_CTL_ROTATION(ctx->rotate / 90);
	/* Stock G2D leaves burst-length fields clear. */
	if (!dev->a133) {
		if (ctx->rotate != 90 && ctx->rotate != 270)
			val |= ROTATE_GLB_CTL_BURST_LEN(ROTATE_BURST_64);
		else
			val |= ROTATE_GLB_CTL_BURST_LEN(ROTATE_BURST_8);
	}
	rotate_write(dev, ROTATE_GLB_CTL, val);

	fmt = rotate_find_format(ctx->src_fmt.pixelformat);
	if (!fmt)
		return;

	rotate_write(dev, ROTATE_IN_FMT, ROTATE_IN_FMT_FORMAT(fmt->hw_format));

	rotate_calc_addr_pitch(vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0),
			       ctx->src_fmt.bytesperline, ctx->src_fmt.height,
			       fmt, addr, pitch);

	rotate_write(dev, ROTATE_IN_SIZE,
		     ROTATE_SIZE(ctx->src_fmt.width, ctx->src_fmt.height));

	rotate_write(dev, ROTATE_IN_PITCH0, pitch[0]);
	rotate_write(dev, ROTATE_IN_PITCH1, pitch[1]);
	rotate_write(dev, ROTATE_IN_PITCH2, pitch[2]);

	rotate_write(dev, ROTATE_IN_ADDRL0, addr[0]);
	rotate_write(dev, ROTATE_IN_ADDRL1, addr[1]);
	rotate_write(dev, ROTATE_IN_ADDRL2, addr[2]);

	rotate_write(dev, ROTATE_IN_ADDRH0, 0);
	rotate_write(dev, ROTATE_IN_ADDRH1, 0);
	rotate_write(dev, ROTATE_IN_ADDRH2, 0);

	fmt = rotate_find_format(ctx->dst_fmt.pixelformat);
	if (!fmt)
		return;

	rotate_calc_addr_pitch(vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0),
			       ctx->dst_fmt.bytesperline, ctx->dst_fmt.height,
			       fmt, addr, pitch);

	rotate_write(dev, ROTATE_OUT_SIZE,
		     ROTATE_SIZE(ctx->dst_fmt.width, ctx->dst_fmt.height));

	rotate_write(dev, ROTATE_OUT_PITCH0, pitch[0]);
	rotate_write(dev, ROTATE_OUT_PITCH1, pitch[1]);
	rotate_write(dev, ROTATE_OUT_PITCH2, pitch[2]);

	rotate_write(dev, ROTATE_OUT_ADDRL0, addr[0]);
	rotate_write(dev, ROTATE_OUT_ADDRL1, addr[1]);
	rotate_write(dev, ROTATE_OUT_ADDRL2, addr[2]);

	rotate_write(dev, ROTATE_OUT_ADDRH0, 0);
	rotate_write(dev, ROTATE_OUT_ADDRH1, 0);
	rotate_write(dev, ROTATE_OUT_ADDRH2, 0);

	rotate_set_bits(dev, ROTATE_INT, ROTATE_INT_FINISH_IRQ_EN);
	rotate_set_bits(dev, ROTATE_GLB_CTL, ROTATE_GLB_CTL_START);
}

/* A133 G2D owns both engines; idle/recovered state selects the rotator. */
static void rotate_a133_enable(struct rotate_dev *dev)
{
	writel(BIT(1), dev->top);
	writel(BIT(1), dev->top + 4);
	writel(BIT(1), dev->top + 8);
}

void rotate_a133_begin(struct rotate_dev *dev, bool mixer)
{
	/* Drain the previous ISR before changing the engine it may access. */
	disable_irq(dev->irq);
	dev->mixer_job = mixer;
	/* Shared block, serialized jobs: clear stale state in both sub-engines. */
	writel(0, dev->top + 8);
	writel(dev->mixer_job ? BIT(0) : BIT(1), dev->top);
	writel(dev->mixer_job ? BIT(0) : BIT(1), dev->top + 4);
	writel(dev->mixer_job ? BIT(0) : BIT(1), dev->top + 8);
	reinit_completion(&dev->complete);
	if (dev->mixer_job)
		writel(BIT(0), dev->top + 0x104);
	else
		rotate_write(dev, ROTATE_INT, ROTATE_INT_FINISH_IRQ);
	enable_irq(dev->irq);
}

enum vb2_buffer_state rotate_a133_wait(struct rotate_dev *dev)
{
	if (!wait_for_completion_timeout(&dev->complete, msecs_to_jiffies(500))) {
		/* Never return DMA buffers until reset has contained a timed-out job. */
		disable_irq(dev->irq);
		if (reset_control_assert(dev->rstc))
			panic("A133 G2D: cannot contain timed-out DMA");
		if (reset_control_deassert(dev->rstc))
			panic("A133 G2D: cannot recover reset");
		rotate_a133_enable(dev);
		dev->mixer_job = false;
		rotate_write(dev, ROTATE_INT, ROTATE_INT_FINISH_IRQ);
		enable_irq(dev->irq);
		return VB2_BUF_STATE_ERROR;
	}

	return VB2_BUF_STATE_DONE;
}

static void rotate_a133_work(struct work_struct *work)
{
	struct rotate_dev *dev = container_of(work, struct rotate_dev, work);
	struct rotate_ctx *ctx = dev->job;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;

	rotate_a133_begin(dev, rotate_uses_mixer(ctx));
	rotate_program(ctx);
	state = rotate_a133_wait(dev);

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
	dev->job = NULL;
	v4l2_m2m_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx);
}

static void rotate_device_run(void *priv)
{
	struct rotate_ctx *ctx = priv;

	if (!ctx->dev->a133) {
		rotate_program(priv);
		return;
	}
	ctx->dev->job = ctx;
	schedule_work(&ctx->dev->work);
}

static irqreturn_t rotate_irq(int irq, void *data)
{
	struct vb2_v4l2_buffer *buffer;
	struct rotate_dev *dev = data;
	struct rotate_ctx *ctx;
	unsigned int val;

	if (dev->a133) {
		if (dev->mixer_job) {
			val = readl(dev->top + 0x104);
			if (!(val & BIT(0)))
				return IRQ_NONE;
			writel(BIT(0), dev->top + 0x104);
			complete(&dev->complete);
			return IRQ_HANDLED;
		}
		val = rotate_read(dev, ROTATE_INT);
		if (!(val & ROTATE_INT_FINISH_IRQ))
			return IRQ_NONE;
		rotate_write(dev, ROTATE_INT, ROTATE_INT_FINISH_IRQ);
		complete(&dev->complete);
		return IRQ_HANDLED;
	}

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_NONE;
	}

	val = rotate_read(dev, ROTATE_INT);
	if (!(val & ROTATE_INT_FINISH_IRQ))
		return IRQ_NONE;

	/* clear flag and disable irq */
	rotate_write(dev, ROTATE_INT, ROTATE_INT_FINISH_IRQ);

	buffer = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(buffer, VB2_BUF_STATE_DONE);

	buffer = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(buffer, VB2_BUF_STATE_DONE);

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);

	return IRQ_HANDLED;
}

static inline struct rotate_ctx *rotate_file2ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct rotate_ctx, fh);
}

static void rotate_prepare_format(struct v4l2_pix_format *pix_fmt)
{
	unsigned int height, width, alignment, sizeimage, size, bpl;
	const struct rotate_format *fmt;
	int i;

	fmt = rotate_find_format(pix_fmt->pixelformat);
	if (!fmt)
		return;

	width = ALIGN(pix_fmt->width, fmt->hsub);
	height = ALIGN(pix_fmt->height, fmt->vsub);

	/* all pitches have to be 16 byte aligned */
	alignment = 16;
	if (fmt->planes > 1)
		alignment *= fmt->hsub / fmt->bpp[1];
	bpl = ALIGN(width * fmt->bpp[0], alignment);

	sizeimage = 0;
	for (i = 0; i < fmt->planes; i++) {
		size = bpl * height;
		if (i > 0) {
			size *= fmt->bpp[i];
			size /= fmt->hsub;
			size /= fmt->vsub;
		}
		sizeimage += size;
	}

	pix_fmt->width = width;
	pix_fmt->height = height;
	pix_fmt->bytesperline = bpl;
	pix_fmt->sizeimage = sizeimage;
}

static int rotate_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, ROTATE_NAME, sizeof(cap->driver));
	strscpy(cap->card, ROTATE_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", ROTATE_NAME);

	return 0;
}

static int rotate_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct rotate_ctx *ctx = rotate_file2ctx(file);

	return rotate_enum_fmt(f, !ctx->dev->a133);
}

static int rotate_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return rotate_enum_fmt(f, false);
}

static int rotate_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	const struct rotate_format *fmt;

	if (fsize->index != 0)
		return -EINVAL;

	fmt = rotate_find_format(fsize->pixel_format);
	if (!fmt)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = ROTATE_MIN_WIDTH;
	fsize->stepwise.min_height = ROTATE_MIN_HEIGHT;
	fsize->stepwise.max_width = ROTATE_MAX_WIDTH;
	fsize->stepwise.max_height = ROTATE_MAX_HEIGHT;
	fsize->stepwise.step_width = fmt->hsub;
	fsize->stepwise.step_height = fmt->vsub;

	return 0;
}

static int rotate_set_cap_format(struct rotate_ctx *ctx,
				 struct v4l2_pix_format *f,
				 u32 rotate)
{
	const struct rotate_format *fmt;

	fmt = rotate_find_format(ctx->src_fmt.pixelformat);
	if (!fmt)
		return -EINVAL;

	if (fmt->flags & ROTATE_FLAG_YUV)
		f->pixelformat = V4L2_PIX_FMT_YUV420;
	else
		f->pixelformat = ctx->src_fmt.pixelformat;

	f->field = V4L2_FIELD_NONE;

	if (rotate == 90 || rotate == 270) {
		f->width = ctx->src_fmt.height;
		f->height = ctx->src_fmt.width;
	} else {
		f->width = ctx->src_fmt.width;
		f->height = ctx->src_fmt.height;
	}

	rotate_prepare_format(f);

	return 0;
}

static int rotate_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rotate_ctx *ctx = rotate_file2ctx(file);

	f->fmt.pix = ctx->dst_fmt;

	return 0;
}

static int rotate_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rotate_ctx *ctx = rotate_file2ctx(file);

	f->fmt.pix = ctx->src_fmt;

	return 0;
}

static int rotate_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct rotate_ctx *ctx = rotate_file2ctx(file);

	const struct rotate_format *src = rotate_find_format(ctx->src_fmt.pixelformat);
	const struct rotate_format *dst = rotate_find_format(f->fmt.pix.pixelformat);

	if (ctx->dev->a133 && !ctx->rotate && !ctx->hflip && !ctx->vflip) {
		if (!dst)
			f->fmt.pix.pixelformat = ctx->src_fmt.pixelformat;
		f->fmt.pix.width = clamp(f->fmt.pix.width, ROTATE_MIN_WIDTH, ROTATE_MAX_WIDTH);
		f->fmt.pix.height = clamp(f->fmt.pix.height, ROTATE_MIN_HEIGHT, ROTATE_MAX_HEIGHT);
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.colorspace = ctx->src_fmt.colorspace;
		f->fmt.pix.xfer_func = ctx->src_fmt.xfer_func;
		dst = rotate_find_format(f->fmt.pix.pixelformat);
		if (src->flags & ROTATE_FLAG_YUV) {
			f->fmt.pix.ycbcr_enc = ctx->src_fmt.ycbcr_enc;
			f->fmt.pix.quantization = dst->flags & ROTATE_FLAG_YUV ?
				ctx->src_fmt.quantization : V4L2_QUANTIZATION_FULL_RANGE;
		} else if (dst->flags & ROTATE_FLAG_YUV) {
			if (!f->fmt.pix.ycbcr_enc)
				f->fmt.pix.ycbcr_enc =
					V4L2_MAP_YCBCR_ENC_DEFAULT(f->fmt.pix.colorspace);
			f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		} else {
			f->fmt.pix.ycbcr_enc = ctx->src_fmt.ycbcr_enc;
			f->fmt.pix.quantization = ctx->src_fmt.quantization;
		}
		rotate_prepare_format(&f->fmt.pix);
		return 0;
	}
	return rotate_set_cap_format(ctx, &f->fmt.pix, ctx->rotate);
}

static int rotate_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	if (!rotate_find_format(f->fmt.pix.pixelformat))
		f->fmt.pix.pixelformat = V4L2_PIX_FMT_ARGB32;

	if (f->fmt.pix.width < ROTATE_MIN_WIDTH)
		f->fmt.pix.width = ROTATE_MIN_WIDTH;
	if (f->fmt.pix.height < ROTATE_MIN_HEIGHT)
		f->fmt.pix.height = ROTATE_MIN_HEIGHT;

	if (f->fmt.pix.width > ROTATE_MAX_WIDTH)
		f->fmt.pix.width = ROTATE_MAX_WIDTH;
	if (f->fmt.pix.height > ROTATE_MAX_HEIGHT)
		f->fmt.pix.height = ROTATE_MAX_HEIGHT;

	f->fmt.pix.field = V4L2_FIELD_NONE;

	rotate_prepare_format(&f->fmt.pix);

	return 0;
}

static int rotate_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rotate_ctx *ctx = rotate_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = rotate_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ctx->dst_fmt = f->fmt.pix;

	return 0;
}

static int rotate_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rotate_ctx *ctx = rotate_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = rotate_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	/*
	 * Capture queue has to be also checked, because format and size
	 * depends on output format and size.
	 */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ctx->src_fmt = f->fmt.pix;

	/* Propagate colorspace information to capture. */
	ctx->dst_fmt.colorspace = f->fmt.pix.colorspace;
	ctx->dst_fmt.xfer_func = f->fmt.pix.xfer_func;
	ctx->dst_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->dst_fmt.quantization = f->fmt.pix.quantization;

	return rotate_set_cap_format(ctx, &ctx->dst_fmt, ctx->rotate);
}

static const struct v4l2_ioctl_ops rotate_ioctl_ops = {
	.vidioc_querycap		= rotate_querycap,

	.vidioc_enum_framesizes		= rotate_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= rotate_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= rotate_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= rotate_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= rotate_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= rotate_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out		= rotate_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= rotate_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= rotate_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int rotate_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct rotate_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (*nplanes) {
		if (sizes[0] < pix_fmt->sizeimage)
			return -EINVAL;
	} else {
		sizes[0] = pix_fmt->sizeimage;
		*nplanes = 1;
	}

	return 0;
}

static int rotate_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rotate_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix_fmt->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, pix_fmt->sizeimage);

	return 0;
}

static void rotate_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rotate_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void rotate_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct rotate_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	do {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (vbuf)
			v4l2_m2m_buf_done(vbuf, state);
	} while (vbuf);
}

static int rotate_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rotate_ctx *owner = vb2_get_drv_priv(vq);
	int error = owner->dev->resume_failed ? -EIO : rotate_check_csc(owner);

	if (owner->dev->compose_active)
		error = -EBUSY;

	if (error) {
		rotate_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
		return error;
	}

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct rotate_ctx *ctx = vb2_get_drv_priv(vq);
		struct device *dev = ctx->dev->dev;
		int ret;

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "Failed to enable module\n");

			rotate_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
			return ret;
		}
	}

	return 0;
}

static void rotate_stop_streaming(struct vb2_queue *vq)
{
	struct rotate_ctx *owner = vb2_get_drv_priv(vq);

	/* job_finish wakes streamoff before its scheduler tail has returned. */
	if (owner->dev->a133)
		flush_work(&owner->dev->work);

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct rotate_ctx *ctx = vb2_get_drv_priv(vq);

		if (ctx->dev->removing)
			pm_runtime_put_noidle(ctx->dev->dev);
		else
			pm_runtime_put(ctx->dev->dev);
	}

	rotate_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops rotate_qops = {
	.queue_setup		= rotate_queue_setup,
	.buf_prepare		= rotate_buf_prepare,
	.buf_queue		= rotate_buf_queue,
	.start_streaming	= rotate_start_streaming,
	.stop_streaming		= rotate_stop_streaming,
};

static int rotate_queue_init(void *priv, struct vb2_queue *src_vq,
			     struct vb2_queue *dst_vq)
{
	struct rotate_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->min_queued_buffers = 1;
	src_vq->ops = &rotate_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->min_queued_buffers = 2;
	dst_vq->ops = &rotate_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(dst_vq);
	if (ret)
		return ret;

	return 0;
}

static int rotate_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rotate_ctx *ctx = container_of(ctrl->handler,
					      struct rotate_ctx,
					      ctrl_handler);
	struct v4l2_pix_format fmt = ctx->dst_fmt;

	/* Mixer and rotator cannot operate on the same transaction. */
	if (ctrl->val && rotate_uses_mixer(ctx))
		return -EINVAL;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ctx->hflip = ctrl->val;
		break;
	case V4L2_CID_VFLIP:
		ctx->vflip = ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		rotate_set_cap_format(ctx, &fmt, ctrl->val);

		/* Check if capture format needs to be changed */
		if (fmt.width != ctx->dst_fmt.width ||
		    fmt.height != ctx->dst_fmt.height ||
		    fmt.bytesperline != ctx->dst_fmt.bytesperline ||
		    fmt.sizeimage != ctx->dst_fmt.sizeimage) {
			struct vb2_queue *vq;

			vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					     V4L2_BUF_TYPE_VIDEO_CAPTURE);
			if (vb2_is_busy(vq))
				return -EBUSY;

			rotate_set_cap_format(ctx, &ctx->dst_fmt, ctrl->val);
		}

		ctx->rotate = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops rotate_ctrl_ops = {
	.s_ctrl = rotate_s_ctrl,
};

static int rotate_setup_ctrls(struct rotate_ctx *ctx)
{
	int ret;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 3);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &rotate_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &rotate_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &rotate_ctrl_ops,
			  V4L2_CID_ROTATE, 0, 270, 90, 0);

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;

		v4l2_err(&ctx->dev->v4l2_dev, "control setup failed!\n");
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);

		return err;
	}

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
	if (ret)
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	return ret;
}

static int rotate_open(struct file *file)
{
	struct rotate_dev *dev = video_drvdata(file);
	struct rotate_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	if (dev->removing || (dev->a133 && !dev->ready)) {
		mutex_unlock(&dev->dev_mutex);
		return -ENODEV;
	}

	ctx = kzalloc_obj(*ctx);
	if (!ctx) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOMEM;
	}

	/* default output format */
	ctx->src_fmt.pixelformat = V4L2_PIX_FMT_ARGB32;
	ctx->src_fmt.field = V4L2_FIELD_NONE;
	ctx->src_fmt.width = 640;
	ctx->src_fmt.height = 480;
	rotate_prepare_format(&ctx->src_fmt);

	/* default capture format */
	rotate_set_cap_format(ctx, &ctx->dst_fmt, ctx->rotate);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->dev = dev;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &rotate_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_fh;
	}

	v4l2_fh_add(&ctx->fh, file);

	ret = rotate_setup_ctrls(ctx);
	if (ret)
		goto err_m2m;

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_m2m:
	v4l2_fh_del(&ctx->fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int rotate_release(struct file *file)
{
	struct rotate_dev *dev = video_drvdata(file);
	struct rotate_ctx *ctx = rotate_file2ctx(file);

	mutex_lock(&dev->dev_mutex);

	if (dev->a133) {
		v4l2_m2m_suspend(dev->m2m_dev);
		flush_work(&dev->work);
	}
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	if (dev->a133 && !dev->removing && !dev->resume_failed && !dev->compose_active)
		v4l2_m2m_resume(dev->m2m_dev);

	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations rotate_fops = {
	.owner		= THIS_MODULE,
	.open		= rotate_open,
	.release	= rotate_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device rotate_video_device = {
	.name		= ROTATE_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &rotate_fops,
	.ioctl_ops	= &rotate_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops rotate_m2m_ops = {
	.device_run	= rotate_device_run,
};

static int rotate_probe(struct platform_device *pdev)
{
	struct rotate_dev *dev;
	struct video_device *vfd;
	int irq, ret;
	bool a133 = of_device_is_compatible(pdev->dev.of_node,
					   "allwinner,sun50i-a133-g2d");

	dev = a133 ? kzalloc(sizeof(*dev), GFP_KERNEL) :
		devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->vfd = rotate_video_device;
	dev->dev = &pdev->dev;
	dev->a133 = a133;
	if (dev->a133) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(dev->dev);

		kref_init(&dev->ref);
		get_device(dev->dev);
		ret = devm_add_action_or_reset(dev->dev, rotate_a133_put, dev);
		if (ret)
			return ret;
		dev->vfd.release = rotate_video_release;

		if (!domain || !(domain->type & __IOMMU_DOMAIN_PAGING))
			return dev_err_probe(dev->dev, -EINVAL,
					     "G2D requires a translated DMA domain\n");
		ret = dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
		init_completion(&dev->complete);
		INIT_WORK(&dev->work, rotate_a133_work);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return irq;

	dev->irq = irq;
	ret = devm_request_irq(dev->dev, irq, rotate_irq,
			       dev->a133 ? IRQF_NO_AUTOEN : 0, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request IRQ\n");

		return ret;
	}

	dev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	if (dev->a133) {
		if (resource_size(platform_get_resource(pdev, IORESOURCE_MEM, 0)) <
		    0x280b8)
			return -EINVAL;
		dev->top = dev->base;
		dev->base += 0x28000;
		dev->mbus_clk = devm_clk_get(dev->dev, "ram");
		if (IS_ERR(dev->mbus_clk))
			return PTR_ERR(dev->mbus_clk);
	}

	dev->bus_clk = devm_clk_get(dev->dev, "bus");
	if (IS_ERR(dev->bus_clk)) {
		dev_err(dev->dev, "Failed to get bus clock\n");

		return PTR_ERR(dev->bus_clk);
	}

	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk)) {
		dev_err(dev->dev, "Failed to get mod clock\n");

		return PTR_ERR(dev->mod_clk);
	}

	dev->rstc = devm_reset_control_get(dev->dev, NULL);
	if (IS_ERR(dev->rstc)) {
		dev_err(dev->dev, "Failed to get reset control\n");

		return PTR_ERR(dev->rstc);
	}

	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(dev->dev, "Failed to register V4L2 device\n");

		return ret;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s",
		 rotate_video_device.name);
	video_set_drvdata(vfd, dev);

	dev->m2m_dev = v4l2_m2m_init(&rotate_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		ret = PTR_ERR(dev->m2m_dev);
		goto err_v4l2;
	}
	platform_set_drvdata(pdev, dev);
	pm_runtime_enable(dev->dev);
	if (dev->a133)
		kref_get(&dev->ref);
	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (!ret) {
		if (!dev->a133)
			return 0;
		ret = sun50i_g2d_comp_register(dev);
		if (!ret) {
			mutex_lock(&dev->dev_mutex);
			dev->ready = true;
			mutex_unlock(&dev->dev_mutex);
			return 0;
		}
		video_unregister_device(vfd);
		pm_runtime_disable(dev->dev);
		goto err_v4l2;
	}

	pm_runtime_disable(dev->dev);
	if (dev->a133)
		rotate_a133_put(dev);
	else
		v4l2_m2m_release(dev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static void rotate_remove(struct platform_device *pdev)
{
	struct rotate_dev *dev = platform_get_drvdata(pdev);

	if (dev->a133) {
		mutex_lock(&dev->dev_mutex);
		dev->removing = true;
		sun50i_g2d_comp_unregister(dev);
		video_unregister_device(&dev->vfd);
		v4l2_m2m_suspend(dev->m2m_dev);
		flush_work(&dev->work);
		if (pm_runtime_force_suspend(&pdev->dev))
			panic("A133 G2D: cannot quiesce removal");
		pm_runtime_disable(&pdev->dev);
		v4l2_device_unregister(&dev->v4l2_dev);
		mutex_unlock(&dev->dev_mutex);
		/* The last video file releases m2m and memory after its contexts. */
		return;
	}

	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	pm_runtime_force_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static int rotate_runtime_resume(struct device *device)
{
	struct rotate_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = clk_prepare_enable(dev->bus_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable bus clock\n");

		return ret;
	}

	ret = clk_prepare_enable(dev->mod_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable mod clock\n");

		goto err_bus_clk;
	}

	ret = clk_prepare_enable(dev->mbus_clk);
	if (ret)
		goto err_mod_clk;

	ret = reset_control_deassert(dev->rstc);
	if (ret) {
		dev_err(dev->dev, "Failed to apply reset\n");

		goto err_mbus_clk;
	}

	if (dev->a133) {
		rotate_a133_enable(dev);
		dev->mixer_job = false;
		rotate_write(dev, ROTATE_INT, ROTATE_INT_FINISH_IRQ);
		enable_irq(dev->irq);
	}
	return 0;

err_mbus_clk:
	clk_disable_unprepare(dev->mbus_clk);
err_mod_clk:
	clk_disable_unprepare(dev->mod_clk);
err_bus_clk:
	clk_disable_unprepare(dev->bus_clk);

	return ret;
}

static int rotate_runtime_suspend(struct device *device)
{
	struct rotate_dev *dev = dev_get_drvdata(device);

	int ret;

	if (dev->a133)
		disable_irq(dev->irq);
	ret = reset_control_assert(dev->rstc);
	if (ret && dev->a133) {
		enable_irq(dev->irq);
		return ret;
	}

	clk_disable_unprepare(dev->mbus_clk);
	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->bus_clk);

	return 0;
}

static const struct of_device_id rotate_dt_match[] = {
	{ .compatible = "allwinner,sun8i-a83t-de2-rotate" },
	{ .compatible = "allwinner,sun50i-a133-g2d" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rotate_dt_match);

static int rotate_suspend(struct device *device)
{
	struct rotate_dev *dev = dev_get_drvdata(device);
	int ret;

	if (!dev->a133)
		return 0;
	v4l2_m2m_suspend(dev->m2m_dev);
	sun50i_g2d_comp_pause(dev, true);
	ret = pm_runtime_force_suspend(device);
	if (ret) {
		sun50i_g2d_comp_pause(dev, false);
		if (!dev->compose_active)
			v4l2_m2m_resume(dev->m2m_dev);
	}
	return ret;
}

static int rotate_resume(struct device *device)
{
	struct rotate_dev *dev = dev_get_drvdata(device);
	int ret;

	if (!dev->a133)
		return 0;
	if (dev->resume_failed)
		return -EIO;
	ret = pm_runtime_force_resume(device);
	if (ret)
		dev->resume_failed = true;
	else {
		sun50i_g2d_comp_pause(dev, false);
		if (!dev->compose_active)
			v4l2_m2m_resume(dev->m2m_dev);
	}
	return ret;
}

static void rotate_shutdown(struct platform_device *pdev)
{
	struct rotate_dev *dev = platform_get_drvdata(pdev);

	if (dev->a133) {
		v4l2_m2m_suspend(dev->m2m_dev);
		flush_work(&dev->work);
		sun50i_g2d_comp_pause(dev, true);
		if (pm_runtime_force_suspend(&pdev->dev))
			panic("A133 G2D: cannot quiesce shutdown");
	}
}

static const struct dev_pm_ops rotate_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(rotate_suspend, rotate_resume)
	.runtime_resume		= rotate_runtime_resume,
	.runtime_suspend	= rotate_runtime_suspend,
};

static struct platform_driver rotate_driver = {
	.probe		= rotate_probe,
	.remove		= rotate_remove,
	.shutdown	= rotate_shutdown,
	.driver		= {
		.name		= ROTATE_NAME,
		.of_match_table	= rotate_dt_match,
		.pm		= &rotate_pm_ops,
	},
};
module_platform_driver(rotate_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@siol.net>");
MODULE_DESCRIPTION("Allwinner DE2 rotate driver");

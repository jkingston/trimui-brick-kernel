// SPDX-License-Identifier: GPL-2.0
/* Standard stateful V4L2 mem2mem frontend for complete Cedrus encoders. */

#include <linux/pm_runtime.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_enc.h"
#include "cedrus_enc_video.h"
#include "cedrus_hw.h"

static const struct v4l2_event cedrus_enc_eos_event = {
	.type = V4L2_EVENT_EOS,
};

static const struct cedrus_enc_format *
cedrus_enc_find_format(struct cedrus_ctx *ctx, u32 pixelformat, u32 direction)
{
	const struct cedrus_enc_backend_ops *ops = ctx->dev->enc_ops;
	const struct cedrus_enc_format *fallback = NULL;
	unsigned int i;

	for (i = 0; i < ops->num_formats; i++) {
		const struct cedrus_enc_format *format = &ops->formats[i];

		if (format->direction != direction)
			continue;
		if (!fallback)
			fallback = format;
		if (format->pixelformat == pixelformat)
			return format;
	}

	return fallback;
}

static void cedrus_enc_prepare_format(const struct cedrus_enc_format *format,
				      struct v4l2_pix_format *pix)
{
	u32 height;

	pix->pixelformat = format->pixelformat;
	pix->field = V4L2_FIELD_NONE;
	pix->width = ALIGN(clamp(pix->width, format->min_width,
				 format->max_width), format->width_align);
	pix->height = ALIGN(clamp(pix->height, format->min_height,
				   format->max_height), format->height_align);

	if (format->direction == CEDRUS_ENC_FORMAT_RAW) {
		pix->bytesperline = ALIGN(pix->width, format->stride_align);
		height = ALIGN(pix->height, format->height_align);
		pix->sizeimage = pix->bytesperline * height * 3 / 2;
	} else {
		pix->bytesperline = 0;
		pix->sizeimage = min_t(u32, pix->sizeimage,
				       U32_MAX - format->buffer_align + 1);
		pix->sizeimage = ALIGN(max(pix->sizeimage,
					   format->min_sizeimage),
				       format->buffer_align);
	}
}

static int cedrus_enc_try_fmt(struct cedrus_ctx *ctx,
			      struct v4l2_format *f, u32 direction)
{
	const struct cedrus_enc_format *format;

	format = cedrus_enc_find_format(ctx, f->fmt.pix.pixelformat, direction);
	if (!format)
		return -EINVAL;

	cedrus_enc_prepare_format(format, &f->fmt.pix);
	return 0;
}

static int cedrus_enc_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	strscpy(cap->driver, CEDRUS_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Cedrus encoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", CEDRUS_NAME);
	return 0;
}

static int cedrus_enc_enum_fmt(struct file *file, struct v4l2_fmtdesc *f,
			       u32 direction)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	const struct cedrus_enc_backend_ops *ops = ctx->dev->enc_ops;
	unsigned int i, index = 0;

	for (i = 0; i < ops->num_formats; i++) {
		if (ops->formats[i].direction != direction)
			continue;
		if (index++ == f->index) {
			f->pixelformat = ops->formats[i].pixelformat;
			return 0;
		}
	}
	return -EINVAL;
}

static int cedrus_enc_enum_out(struct file *file, void *priv,
			       struct v4l2_fmtdesc *f)
{
	return cedrus_enc_enum_fmt(file, f, CEDRUS_ENC_FORMAT_RAW);
}

static int cedrus_enc_enum_cap(struct file *file, void *priv,
			       struct v4l2_fmtdesc *f)
{
	return cedrus_enc_enum_fmt(file, f, CEDRUS_ENC_FORMAT_CODED);
}

static int cedrus_enc_g_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);

	f->fmt.pix = V4L2_TYPE_IS_OUTPUT(f->type) ? ctx->src_fmt : ctx->dst_fmt;
	return 0;
}

static int cedrus_enc_try_out(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	return cedrus_enc_try_fmt(cedrus_file2ctx(file), f,
				  CEDRUS_ENC_FORMAT_RAW);
}

static int cedrus_enc_try_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	return cedrus_enc_try_fmt(cedrus_file2ctx(file), f,
				  CEDRUS_ENC_FORMAT_CODED);
}

static int cedrus_enc_s_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct vb2_queue *src = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	struct vb2_queue *dst = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	int ret;

	/* Either format describes one hardware job; lock both once allocated. */
	if (vb2_is_busy(src) || vb2_is_busy(dst))
		return -EBUSY;

	ret = cedrus_enc_try_fmt(ctx, f, V4L2_TYPE_IS_OUTPUT(f->type) ?
				 CEDRUS_ENC_FORMAT_RAW : CEDRUS_ENC_FORMAT_CODED);
	if (ret)
		return ret;

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		ctx->src_fmt = f->fmt.pix;
		ctx->dst_fmt.width = f->fmt.pix.width;
		ctx->dst_fmt.height = f->fmt.pix.height;
		ctx->dst_fmt.colorspace = f->fmt.pix.colorspace;
		ctx->dst_fmt.xfer_func = f->fmt.pix.xfer_func;
		ctx->dst_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
		ctx->dst_fmt.quantization = f->fmt.pix.quantization;
		cedrus_enc_prepare_format(cedrus_enc_find_format(ctx,
			ctx->dst_fmt.pixelformat, CEDRUS_ENC_FORMAT_CODED),
			&ctx->dst_fmt);
	} else {
		ctx->dst_fmt = f->fmt.pix;
		ctx->src_fmt.width = f->fmt.pix.width;
		ctx->src_fmt.height = f->fmt.pix.height;
		ctx->src_fmt.colorspace = f->fmt.pix.colorspace;
		ctx->src_fmt.xfer_func = f->fmt.pix.xfer_func;
		ctx->src_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
		ctx->src_fmt.quantization = f->fmt.pix.quantization;
		cedrus_enc_prepare_format(cedrus_enc_find_format(ctx,
			ctx->src_fmt.pixelformat, CEDRUS_ENC_FORMAT_RAW),
			&ctx->src_fmt);
	}

	return 0;
}

static int cedrus_enc_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
				  unsigned int *nplanes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		&ctx->src_fmt : &ctx->dst_fmt;

	if (*nplanes) {
		if (*nplanes != 1 || sizes[0] < pix->sizeimage)
			return -EINVAL;
	} else {
		*nplanes = 1;
		sizes[0] = pix->sizeimage;
	}
	return 0;
}

static int cedrus_enc_buf_prepare(struct vb2_buffer *vb)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format *pix = V4L2_TYPE_IS_OUTPUT(vb->type) ?
		&ctx->src_fmt : &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix->sizeimage)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(vb->type) &&
	    vb2_get_plane_payload(vb, 0) < pix->sizeimage)
		return -EINVAL;
	if (V4L2_TYPE_IS_CAPTURE(vb->type))
		vb2_set_plane_payload(vb, 0, 0);
	return 0;
}

static int cedrus_enc_buf_out_validate(struct vb2_buffer *vb)
{
	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return vb->req_obj.req ? -EBADR : 0;
}

static void cedrus_enc_buf_queue(struct vb2_buffer *vb)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (V4L2_TYPE_IS_CAPTURE(vb->type) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vbuf->sequence = ctx->sequence_cap++;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		v4l2_event_queue_fh(&ctx->fh, &cedrus_enc_eos_event);
		return;
	}

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

void cedrus_enc_job_finish(struct cedrus_ctx *ctx)
{
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst)
		return;

	src->sequence = ctx->sequence_out++;
	dst->sequence = ctx->sequence_cap++;
	if (v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
		v4l2_event_queue_fh(&ctx->fh, &cedrus_enc_eos_event);
	}
}

static void cedrus_enc_return_buffers(struct vb2_queue *vq,
				      enum vb2_buffer_state state)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *buf;

	while ((buf = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
		v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(buf, state);
}

static int cedrus_enc_start_streaming(struct vb2_queue *vq,
				      unsigned int count)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);
	struct cedrus_dev *dev = ctx->dev;
	int ret;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, vq);
	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;
	if (!V4L2_TYPE_IS_OUTPUT(vq->type))
		return 0;
	if (unlikely(cedrus_iommu_fault_pending(dev))) {
		ret = -EIO;
		goto err_buffers;
	}

	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret < 0)
		goto err_buffers;
	ret = dev->enc_ops->start_streaming(ctx);
	if (ret)
		goto err_pm;
	return 0;

err_pm:
	pm_runtime_put(dev->dev);
err_buffers:
	cedrus_enc_return_buffers(vq, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void cedrus_enc_stop_streaming(struct vb2_queue *vq)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);

	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, vq);
	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		ctx->dev->enc_ops->stop_streaming(ctx);
		pm_runtime_put(ctx->dev->dev);
	}
	cedrus_enc_return_buffers(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops cedrus_enc_qops = {
	.queue_setup = cedrus_enc_queue_setup,
	.buf_prepare = cedrus_enc_buf_prepare,
	.buf_queue = cedrus_enc_buf_queue,
	.buf_out_validate = cedrus_enc_buf_out_validate,
	.start_streaming = cedrus_enc_start_streaming,
	.stop_streaming = cedrus_enc_stop_streaming,
};

static int cedrus_enc_queue_init(void *priv, struct vb2_queue *src,
				 struct vb2_queue *dst)
{
	struct cedrus_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct cedrus_buffer);
	src->ops = &cedrus_enc_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->dev->dev_mutex;
	src->dev = ctx->dev->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct cedrus_buffer);
	dst->ops = &cedrus_enc_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->dev->dev_mutex;
	dst->dev = ctx->dev->dev;
	return vb2_queue_init(dst);
}

static int cedrus_enc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct cedrus_ctx *ctx = container_of(ctrl->handler, struct cedrus_ctx, hdl);

	return ctx->dev->enc_ops->s_ctrl(ctx, ctrl);
}

static const struct v4l2_ctrl_ops cedrus_enc_ctrl_ops = {
	.s_ctrl = cedrus_enc_s_ctrl,
};

static int cedrus_enc_open(struct file *file)
{
	struct cedrus_dev *dev = video_drvdata(file);
	const struct cedrus_enc_backend_ops *ops = dev->enc_ops;
	struct cedrus_ctx *ctx;
	unsigned int i;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		goto err_unlock;
	ctx->dev = dev;
	ctx->is_encoder = true;
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					   cedrus_enc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_fh;
	}

	ctx->src_fmt.pixelformat = 0;
	ctx->src_fmt.width = 640;
	ctx->src_fmt.height = 480;
	cedrus_enc_prepare_format(cedrus_enc_find_format(ctx, 0,
				 CEDRUS_ENC_FORMAT_RAW), &ctx->src_fmt);
	ctx->dst_fmt.pixelformat = 0;
	ctx->dst_fmt.width = ctx->src_fmt.width;
	ctx->dst_fmt.height = ctx->src_fmt.height;
	cedrus_enc_prepare_format(cedrus_enc_find_format(ctx, 0,
				 CEDRUS_ENC_FORMAT_CODED), &ctx->dst_fmt);

	v4l2_ctrl_handler_init(&ctx->hdl, ops->num_controls);
	for (i = 0; i < ops->num_controls; i++) {
		struct v4l2_ctrl_config cfg = ops->controls[i];

		cfg.ops = &cedrus_enc_ctrl_ops;
		v4l2_ctrl_new_custom(&ctx->hdl, &cfg, NULL);
	}
	if (ctx->hdl.error) {
		ret = ctx->hdl.error;
		goto err_m2m;
	}
	ret = v4l2_ctrl_handler_setup(&ctx->hdl);
	if (ret)
		goto err_m2m;
	ctx->fh.ctrl_handler = &ctx->hdl;

	v4l2_fh_add(&ctx->fh, file);
	mutex_unlock(&dev->dev_mutex);
	return 0;

err_m2m:
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);
	return ret;

err_unlock:
	mutex_unlock(&dev->dev_mutex);
	return -ENOMEM;
}

static int cedrus_enc_release(struct file *file)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;

	mutex_lock(&dev->dev_mutex);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);
	return 0;
}

static int cedrus_enc_cmd(struct file *file, void *priv,
			  struct v4l2_encoder_cmd *cmd)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct vb2_queue *src = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	struct vb2_queue *dst = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	if (!vb2_is_streaming(src) || !vb2_is_streaming(dst))
		return 0;

	ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	if (cmd->cmd == V4L2_ENC_CMD_STOP &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &cedrus_enc_eos_event);
	if (cmd->cmd == V4L2_ENC_CMD_START)
		vb2_clear_last_buffer_dequeued(dst);

	return 0;
}

static int cedrus_enc_subscribe_event(struct v4l2_fh *fh,
				      const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 0, NULL);

	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_file_operations cedrus_enc_fops = {
	.owner = THIS_MODULE,
	.open = cedrus_enc_open,
	.release = cedrus_enc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_ioctl_ops cedrus_enc_ioctl_ops = {
	.vidioc_querycap = cedrus_enc_querycap,
	.vidioc_enum_fmt_vid_out = cedrus_enc_enum_out,
	.vidioc_g_fmt_vid_out = cedrus_enc_g_fmt,
	.vidioc_try_fmt_vid_out = cedrus_enc_try_out,
	.vidioc_s_fmt_vid_out = cedrus_enc_s_fmt,
	.vidioc_enum_fmt_vid_cap = cedrus_enc_enum_cap,
	.vidioc_g_fmt_vid_cap = cedrus_enc_g_fmt,
	.vidioc_try_fmt_vid_cap = cedrus_enc_try_cap,
	.vidioc_s_fmt_vid_cap = cedrus_enc_s_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = cedrus_enc_cmd,
	.vidioc_subscribe_event = cedrus_enc_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int cedrus_enc_register_entity(struct cedrus_dev *dev,
				      struct media_entity *entity,
				      const char *suffix,
				      struct media_pad *pads,
				      unsigned int num_pads,
				      unsigned int function)
{
	entity->obj_type = MEDIA_ENTITY_TYPE_BASE;
	if (function == MEDIA_ENT_F_IO_V4L) {
		entity->info.dev.major = VIDEO_MAJOR;
		entity->info.dev.minor = dev->enc_vfd.minor;
	}
	entity->name = devm_kasprintf(dev->dev, GFP_KERNEL, "%s-%s",
				      dev->enc_vfd.name, suffix);
	if (!entity->name)
		return -ENOMEM;
	entity->function = function;

	{
		int ret;

		ret = media_entity_pads_init(entity, num_pads, pads);
		if (ret)
			return ret;
		ret = media_device_register_entity(&dev->mdev, entity);
		if (ret) {
			media_entity_cleanup(entity);
			return ret;
		}
	}
	return 0;
}

static void cedrus_enc_media_detach(struct cedrus_dev *dev)
{
	if (!dev->enc_media_registered)
		return;

	media_devnode_remove(dev->enc_intf);
	media_entity_remove_links(&dev->enc_sink);
	media_entity_remove_links(&dev->enc_proc);
	media_entity_remove_links(&dev->enc_vfd.entity);
	media_device_unregister_entity(&dev->enc_sink);
	media_device_unregister_entity(&dev->enc_proc);
	media_device_unregister_entity(&dev->enc_vfd.entity);
	media_entity_cleanup(&dev->enc_sink);
	media_entity_cleanup(&dev->enc_proc);
	media_entity_cleanup(&dev->enc_vfd.entity);
	dev->enc_media_registered = false;
}

static int cedrus_enc_media_attach(struct cedrus_dev *dev)
{
	struct media_link *link;
	int ret;

	dev->enc_source_pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = cedrus_enc_register_entity(dev, &dev->enc_vfd.entity, "source",
					 &dev->enc_source_pad, 1,
					 MEDIA_ENT_F_IO_V4L);
	if (ret)
		return ret;

	dev->enc_proc_pads[0].flags = MEDIA_PAD_FL_SINK;
	dev->enc_proc_pads[1].flags = MEDIA_PAD_FL_SOURCE;
	ret = cedrus_enc_register_entity(dev, &dev->enc_proc, "proc",
					 dev->enc_proc_pads, 2,
					 MEDIA_ENT_F_PROC_VIDEO_ENCODER);
	if (ret)
		goto err_source;

	dev->enc_sink_pad.flags = MEDIA_PAD_FL_SINK;
	ret = cedrus_enc_register_entity(dev, &dev->enc_sink, "sink",
					 &dev->enc_sink_pad, 1,
					 MEDIA_ENT_F_IO_V4L);
	if (ret)
		goto err_proc;

	ret = media_create_pad_link(&dev->enc_vfd.entity, 0,
				    &dev->enc_proc, 0,
				    MEDIA_LNK_FL_IMMUTABLE |
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto err_sink;
	ret = media_create_pad_link(&dev->enc_proc, 1, &dev->enc_sink, 0,
				    MEDIA_LNK_FL_IMMUTABLE |
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto err_links;

	dev->enc_intf = media_devnode_create(&dev->mdev,
					     MEDIA_INTF_T_V4L_VIDEO, 0,
					     VIDEO_MAJOR,
					     dev->enc_vfd.minor);
	if (!dev->enc_intf) {
		ret = -ENOMEM;
		goto err_links;
	}
	link = media_create_intf_link(&dev->enc_vfd.entity,
				      &dev->enc_intf->intf,
				      MEDIA_LNK_FL_IMMUTABLE |
				      MEDIA_LNK_FL_ENABLED);
	if (!link) {
		ret = -ENOMEM;
		goto err_intf;
	}
	link = media_create_intf_link(&dev->enc_sink, &dev->enc_intf->intf,
				      MEDIA_LNK_FL_IMMUTABLE |
				      MEDIA_LNK_FL_ENABLED);
	if (!link) {
		ret = -ENOMEM;
		goto err_intf;
	}

	dev->enc_media_registered = true;
	return 0;

err_intf:
	media_devnode_remove(dev->enc_intf);
err_links:
	media_entity_remove_links(&dev->enc_sink);
	media_entity_remove_links(&dev->enc_proc);
	media_entity_remove_links(&dev->enc_vfd.entity);
err_sink:
	media_device_unregister_entity(&dev->enc_sink);
	media_entity_cleanup(&dev->enc_sink);
err_proc:
	media_device_unregister_entity(&dev->enc_proc);
	media_entity_cleanup(&dev->enc_proc);
err_source:
	media_device_unregister_entity(&dev->enc_vfd.entity);
	media_entity_cleanup(&dev->enc_vfd.entity);
	return ret;
}

int cedrus_enc_video_register(struct cedrus_dev *dev)
{
	struct video_device *vfd = &dev->enc_vfd;
	int ret;

	if (!dev->enc_ops)
		return 0;

	strscpy(vfd->name, "cedrus-encoder", sizeof(vfd->name));
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->fops = &cedrus_enc_fops;
	vfd->ioctl_ops = &cedrus_enc_ioctl_ops;
	vfd->minor = -1;
	vfd->release = video_device_release_empty;
	vfd->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	video_set_drvdata(vfd, dev);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;
	ret = cedrus_enc_media_attach(dev);
	if (ret) {
		video_unregister_device(vfd);
		return ret;
	}
	v4l2_info(&dev->v4l2_dev, "Encoder registered as /dev/video%d\n",
		  vfd->num);
	return 0;
}

void cedrus_enc_video_unregister(struct cedrus_dev *dev)
{
	if (dev->enc_ops && video_is_registered(&dev->enc_vfd)) {
		cedrus_enc_media_detach(dev);
		video_unregister_device(&dev->enc_vfd);
	}
}

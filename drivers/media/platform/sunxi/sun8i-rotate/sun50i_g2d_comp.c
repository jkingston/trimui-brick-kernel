// SPDX-License-Identifier: GPL-2.0-only
/* A133 G2D two-input, straight-alpha source-over compositor. */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <media/media-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>

#include "sun8i-rotate.h"

#define G2D_COMP_NODES 3
#define G2D_COMP_ALL GENMASK(2, 0)

#define G2D_MIXER_CONTROL	0x100
#define G2D_MIXER_IRQ	0x104
#define G2D_BLD_ENABLE	0x400
#define G2D_BLD_SIZE0	0x420
#define G2D_BLD_SIZE1	0x424
#define G2D_BLD_OFFSET1	0x434
#define G2D_BLD_SIZE	0x448
#define G2D_BLD_CONTROL	0x44c
#define G2D_BLD_COLOR	0x460
#define G2D_ROP	0x480
#define G2D_V0_ATTR	0x800
#define G2D_V0_SIZE	0x804
#define G2D_V0_PITCH	0x80c
#define G2D_V0_ADDR	0x818
#define G2D_V0_OVL_SIZE	0x82c
#define G2D_UI2_ATTR	0x2000
#define G2D_UI2_SIZE	0x2004
#define G2D_UI2_PITCH	0x200c
#define G2D_UI2_ADDR	0x2010
#define G2D_UI2_OVL_SIZE	0x201c
#define G2D_WB_ATTR	0x3000
#define G2D_WB_SIZE	0x3004
#define G2D_WB_PITCH	0x3008
#define G2D_WB_ADDR	0x3014

struct g2d_comp_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct g2d_comp_node {
	struct video_device vfd;
	struct vb2_queue queue;
	struct media_pad pad;
	struct g2d_comp *comp;
	struct v4l2_pix_format format;
	struct list_head buffers;
	unsigned int index;
};

struct g2d_comp {
	struct rotate_dev *dev;
	struct kref ref;
	struct media_device media;
	struct media_pipeline pipeline;
	struct v4l2_subdev subdev;
	struct media_pad pads[G2D_COMP_NODES];
	struct g2d_comp_node nodes[G2D_COMP_NODES];
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *alpha_ctrl;
	struct v4l2_rect compose;
	struct work_struct work;
	spinlock_t lock;
	unsigned int streaming;
	unsigned int sequence;
	u32 alpha;
	bool paused;
	bool broken;
};

static struct g2d_comp_node *g2d_comp_node(struct file *file)
{
	return video_drvdata(file);
}

/* Exact stock g2d_bsp_bld: video layer is destination, UI2 is source. */
static void g2d_comp_program(struct g2d_comp *comp, dma_addr_t *addr)
{
	void __iomem *b = comp->dev->top;
	struct v4l2_pix_format *bg = &comp->nodes[0].format;
	struct v4l2_pix_format *fg = &comp->nodes[1].format;
	struct v4l2_pix_format *out = &comp->nodes[2].format;
	u32 offset = (comp->compose.left ? comp->compose.left - 1 : 0) |
		(comp->compose.top ? comp->compose.top - 1 : 0) << 16;

	writel(0xff000001 | (bg->pixelformat == V4L2_PIX_FMT_XRGB32 ? 4 << 8 : 0),
	       b + G2D_V0_ATTR);
	writel(ROTATE_SIZE(bg->width, bg->height), b + G2D_V0_SIZE);
	writel(bg->bytesperline, b + G2D_V0_PITCH);
	writel(lower_32_bits(addr[0]), b + G2D_V0_ADDR);
	writel(ROTATE_SIZE(bg->width, bg->height), b + G2D_V0_OVL_SIZE);
	/* ALPHA_COMPONENT supplies alpha only when the format has none. */
	writel(comp->alpha << 24 | 1 |
	       (fg->pixelformat == V4L2_PIX_FMT_XRGB32 ? 4 << 8 | BIT(1) : 0),
	       b + G2D_UI2_ATTR);
	writel(ROTATE_SIZE(fg->width, fg->height), b + G2D_UI2_SIZE);
	writel(fg->bytesperline, b + G2D_UI2_PITCH);
	writel(lower_32_bits(addr[1]), b + G2D_UI2_ADDR);
	writel(ROTATE_SIZE(fg->width, fg->height), b + G2D_UI2_OVL_SIZE);
	writel(0xf0, b + G2D_ROP);
	writel(BIT(8) | BIT(9), b + G2D_BLD_ENABLE);
	writel(ROTATE_SIZE(bg->width, bg->height), b + G2D_BLD_SIZE0);
	writel(ROTATE_SIZE(fg->width, fg->height), b + G2D_BLD_SIZE1);
	writel(offset, b + G2D_BLD_OFFSET1);
	writel(ROTATE_SIZE(out->width, out->height), b + G2D_BLD_SIZE);
	writel(0x03010301, b + G2D_BLD_CONTROL);
	writel(0, b + G2D_BLD_COLOR);
	writel(out->pixelformat == V4L2_PIX_FMT_XRGB32 ? 4 : 0, b + G2D_WB_ATTR);
	writel(ROTATE_SIZE(out->width, out->height), b + G2D_WB_SIZE);
	writel(out->bytesperline, b + G2D_WB_PITCH);
	writel(lower_32_bits(addr[2]), b + G2D_WB_ADDR);
	/* Internal mixer reset cleared unused layers, high addresses and CSC. */
	writel(BIT(4), b + G2D_MIXER_IRQ);
	writel(BIT(31), b + G2D_MIXER_CONTROL);
}

static bool g2d_comp_ready(struct g2d_comp *comp)
{
	unsigned int i;

	if (comp->paused || comp->broken || comp->streaming != G2D_COMP_ALL)
		return false;
	for (i = 0; i < G2D_COMP_NODES; i++)
		if (list_empty(&comp->nodes[i].buffers))
			return false;
	return true;
}

static void g2d_comp_work(struct work_struct *work)
{
	struct g2d_comp *comp = container_of(work, struct g2d_comp, work);
	struct g2d_comp_buffer *buffers[G2D_COMP_NODES];
	dma_addr_t addr[G2D_COMP_NODES];
	enum vb2_buffer_state state;
	unsigned int i;

	for (;;) {
		spin_lock_irq(&comp->lock);
		if (!g2d_comp_ready(comp)) {
			spin_unlock_irq(&comp->lock);
			return;
		}
		for (i = 0; i < G2D_COMP_NODES; i++) {
			buffers[i] = list_first_entry(&comp->nodes[i].buffers,
						     struct g2d_comp_buffer, list);
			list_del(&buffers[i]->list);
			addr[i] = vb2_dma_contig_plane_dma_addr(&buffers[i]->vb.vb2_buf, 0);
		}
		spin_unlock_irq(&comp->lock);

		rotate_a133_begin(comp->dev, true);
		g2d_comp_program(comp, addr);
		state = rotate_a133_wait(comp->dev);
		for (i = 0; i < G2D_COMP_NODES; i++) {
			buffers[i]->vb.sequence = comp->sequence;
			buffers[i]->vb.field = V4L2_FIELD_NONE;
			/* The foreground timestamp identifies this paired transaction. */
			buffers[i]->vb.vb2_buf.timestamp = buffers[1]->vb.vb2_buf.timestamp;
		}
		comp->sequence++;
		for (i = 0; i < G2D_COMP_NODES; i++)
			vb2_buffer_done(&buffers[i]->vb.vb2_buf, state);
	}
}

static void g2d_comp_drain(struct g2d_comp_node *node, enum vb2_buffer_state state)
{
	struct g2d_comp_buffer *buffer, *next;
	LIST_HEAD(buffers);

	spin_lock_irq(&node->comp->lock);
	list_splice_init(&node->buffers, &buffers);
	spin_unlock_irq(&node->comp->lock);
	list_for_each_entry_safe(buffer, next, &buffers, list) {
		list_del(&buffer->list);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

static int g2d_comp_setup(struct vb2_queue *queue, unsigned int *buffers,
			  unsigned int *planes, unsigned int sizes[],
			  struct device *alloc_devs[])
{
	struct g2d_comp_node *node = vb2_get_drv_priv(queue);

	if (*planes)
		return *planes == 1 && sizes[0] >= node->format.sizeimage ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = node->format.sizeimage;
	return 0;
}

static int g2d_comp_prepare(struct vb2_buffer *buffer)
{
	struct g2d_comp_node *node = vb2_get_drv_priv(buffer->vb2_queue);

	if (vb2_plane_size(buffer, 0) < node->format.sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(buffer, 0, node->format.sizeimage);
	return 0;
}

static void g2d_comp_queue(struct vb2_buffer *buffer)
{
	struct g2d_comp_node *node = vb2_get_drv_priv(buffer->vb2_queue);
	struct g2d_comp_buffer *buf = container_of(to_vb2_v4l2_buffer(buffer),
						struct g2d_comp_buffer, vb);
	struct g2d_comp *comp = node->comp;

	spin_lock_irq(&comp->lock);
	list_add_tail(&buf->list, &node->buffers);
	if (g2d_comp_ready(comp))
		schedule_work(&comp->work);
	spin_unlock_irq(&comp->lock);
}

static int g2d_comp_validate(struct g2d_comp *comp)
{
	struct v4l2_pix_format *bg = &comp->nodes[0].format;
	struct v4l2_pix_format *fg = &comp->nodes[1].format;
	struct v4l2_pix_format *out = &comp->nodes[2].format;
	struct v4l2_rect *rect = &comp->compose;

	if (bg->width != out->width || bg->height != out->height ||
	    rect->left < 0 || rect->top < 0 ||
	    rect->width != fg->width || rect->height != fg->height ||
	    rect->width > out->width || rect->height > out->height ||
	    rect->left > out->width - rect->width ||
	    rect->top > out->height - rect->height)
		return -EINVAL;
	return 0;
}

static int g2d_comp_start(struct vb2_queue *queue, unsigned int count)
{
	struct g2d_comp_node *node = vb2_get_drv_priv(queue);
	struct g2d_comp *comp = node->comp;
	struct rotate_dev *dev = comp->dev;
	int ret;

	ret = dev->removing ? -ENODEV : dev->resume_failed ? -EIO :
		comp->broken || comp->paused ? -EPIPE : g2d_comp_validate(comp);
	if (ret)
		goto error;
	if (!comp->streaming) {
		/* Pause all m2m contexts and drain their worker before taking ownership. */
		v4l2_m2m_suspend(dev->m2m_dev);
		flush_work(&dev->work);
		ret = pm_runtime_resume_and_get(dev->dev);
		if (ret < 0)
			goto resume;
		ret = video_device_pipeline_start(&comp->nodes[2].vfd, &comp->pipeline);
		if (ret) {
			pm_runtime_put(dev->dev);
			goto resume;
		}
		dev->compose_active = true;
		comp->sequence = 0;
	}
	spin_lock_irq(&comp->lock);
	comp->streaming |= BIT(node->index);
	if (g2d_comp_ready(comp))
		schedule_work(&comp->work);
	spin_unlock_irq(&comp->lock);
	return 0;
resume:
	v4l2_m2m_resume(dev->m2m_dev);
error:
	g2d_comp_drain(node, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void g2d_comp_stop(struct vb2_queue *queue)
{
	struct g2d_comp_node *node = vb2_get_drv_priv(queue);
	struct g2d_comp *comp = node->comp;
	struct rotate_dev *dev = comp->dev;
	unsigned int i;

	spin_lock_irq(&comp->lock);
	comp->streaming &= ~BIT(node->index);
	comp->broken = true;
	spin_unlock_irq(&comp->lock);
	/* The active transaction finishes/reset-contains DMA before any return. */
	cancel_work_sync(&comp->work);
	for (i = 0; i < G2D_COMP_NODES; i++) {
		if (comp->streaming & BIT(i))
			vb2_queue_error(&comp->nodes[i].queue);
		g2d_comp_drain(&comp->nodes[i], VB2_BUF_STATE_ERROR);
	}
	if (!comp->streaming) {
		if (dev->compose_active) {
			video_device_pipeline_stop(&comp->nodes[2].vfd);
			if (dev->removing || dev->resume_failed)
				pm_runtime_put_noidle(dev->dev);
			else
				pm_runtime_put(dev->dev);
			dev->compose_active = false;
			if (!dev->removing && !dev->resume_failed && !comp->paused)
				v4l2_m2m_resume(dev->m2m_dev);
		}
		comp->broken = false;
	}
}

static const struct vb2_ops g2d_comp_queue_ops = {
	.queue_setup = g2d_comp_setup,
	.buf_prepare = g2d_comp_prepare,
	.buf_queue = g2d_comp_queue,
	.start_streaming = g2d_comp_start,
	.stop_streaming = g2d_comp_stop,
};

static int g2d_comp_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sun50i-g2d-compose", sizeof(cap->driver));
	strscpy(cap->card, "A133 G2D compositor", sizeof(cap->card));
	return 0;
}

static int g2d_comp_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index > 1)
		return -EINVAL;
	fmt->pixelformat = fmt->index ? V4L2_PIX_FMT_XRGB32 : V4L2_PIX_FMT_ARGB32;
	return 0;
}

static int g2d_comp_try_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	pix->width = clamp(pix->width, ROTATE_MIN_WIDTH, ROTATE_MAX_WIDTH);
	pix->height = clamp(pix->height, ROTATE_MIN_HEIGHT, ROTATE_MAX_HEIGHT);
	if (pix->pixelformat != V4L2_PIX_FMT_XRGB32)
		pix->pixelformat = V4L2_PIX_FMT_ARGB32;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = ALIGN(pix->width * 4, 16);
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->flags = 0;
	return 0;
}

static int g2d_comp_get_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	fmt->fmt.pix = g2d_comp_node(file)->format;
	return 0;
}

static int g2d_comp_set_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct g2d_comp_node *node = g2d_comp_node(file);
	unsigned int i;

	for (i = 0; i < G2D_COMP_NODES; i++)
		if (vb2_is_busy(&node->comp->nodes[i].queue))
			return -EBUSY;
	g2d_comp_try_fmt(file, priv, fmt);
	node->format = fmt->fmt.pix;
	if (node->index == 1) {
		v4l2_ctrl_activate(node->comp->alpha_ctrl,
				   fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_XRGB32);
		node->comp->compose = (struct v4l2_rect) {
			.width = fmt->fmt.pix.width, .height = fmt->fmt.pix.height,
		};
	}
	return 0;
}

static int g2d_comp_get_selection(struct file *file, void *priv,
				  struct v4l2_selection *sel)
{
	struct g2d_comp_node *node = g2d_comp_node(file);
	struct v4l2_pix_format *out = &node->comp->nodes[2].format;

	if (node->index != 1 || sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (sel->target) {
	case V4L2_SEL_TGT_COMPOSE:
		sel->r = node->comp->compose;
		break;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		sel->r = (struct v4l2_rect) { .width = out->width, .height = out->height };
		break;
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		sel->r = (struct v4l2_rect) {
			.width = node->format.width, .height = node->format.height,
		};
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int g2d_comp_set_selection(struct file *file, void *priv,
				  struct v4l2_selection *sel)
{
	struct g2d_comp_node *node = g2d_comp_node(file);
	struct g2d_comp *comp = node->comp;
	struct v4l2_rect previous = comp->compose;
	int ret;

	if (node->index != 1 || sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT ||
	    sel->target != V4L2_SEL_TGT_COMPOSE || sel->flags)
		return -EINVAL;
	if (comp->streaming)
		return -EBUSY;
	comp->compose = sel->r;
	ret = g2d_comp_validate(comp);
	if (ret)
		comp->compose = previous;
	return ret;
}

static int g2d_comp_ctrl(struct v4l2_ctrl *ctrl)
{
	struct g2d_comp *comp = container_of(ctrl->handler, struct g2d_comp, controls);

	if (comp->streaming)
		return -EBUSY;
	comp->alpha = ctrl->val;
	return 0;
}

static const struct v4l2_ctrl_ops g2d_comp_ctrl_ops = {
	.s_ctrl = g2d_comp_ctrl,
};

static const struct v4l2_ioctl_ops g2d_comp_ioctl_ops = {
	.vidioc_querycap = g2d_comp_querycap,
	.vidioc_enum_fmt_vid_cap = g2d_comp_enum_fmt,
	.vidioc_enum_fmt_vid_out = g2d_comp_enum_fmt,
	.vidioc_g_fmt_vid_cap = g2d_comp_get_fmt,
	.vidioc_g_fmt_vid_out = g2d_comp_get_fmt,
	.vidioc_s_fmt_vid_cap = g2d_comp_set_fmt,
	.vidioc_s_fmt_vid_out = g2d_comp_set_fmt,
	.vidioc_try_fmt_vid_cap = g2d_comp_try_fmt,
	.vidioc_try_fmt_vid_out = g2d_comp_try_fmt,
	.vidioc_g_selection = g2d_comp_get_selection,
	.vidioc_s_selection = g2d_comp_set_selection,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static int g2d_comp_open(struct file *file)
{
	struct g2d_comp_node *node = g2d_comp_node(file);
	struct rotate_dev *dev = node->comp->dev;
	int ret;

	mutex_lock(&dev->dev_mutex);
	ret = dev->removing || !dev->ready ? -ENODEV : v4l2_fh_open(file);
	mutex_unlock(&dev->dev_mutex);
	return ret;
}

static const struct v4l2_file_operations g2d_comp_fops = {
	.owner = THIS_MODULE,
	.open = g2d_comp_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void g2d_comp_release(struct kref *ref)
{
	struct g2d_comp *comp = container_of(ref, struct g2d_comp, ref);

	v4l2_ctrl_handler_free(&comp->controls);
	media_device_cleanup(&comp->media);
	rotate_a133_put(comp->dev);
	kfree(comp);
}

static void g2d_comp_video_release(struct video_device *vfd)
{
	struct g2d_comp_node *node = container_of(vfd, struct g2d_comp_node, vfd);

	media_entity_cleanup(&vfd->entity);
	kref_put(&node->comp->ref, g2d_comp_release);
}

void sun50i_g2d_comp_pause(struct rotate_dev *dev, bool pause)
{
	struct g2d_comp *comp = dev->compose;

	if (!comp)
		return;
	spin_lock_irq(&comp->lock);
	comp->paused = pause;
	if (g2d_comp_ready(comp))
		schedule_work(&comp->work);
	spin_unlock_irq(&comp->lock);
	if (pause)
		cancel_work_sync(&comp->work);
}

static const struct v4l2_subdev_ops g2d_comp_subdev_ops;

int sun50i_g2d_comp_register(struct rotate_dev *dev)
{
	static const char * const names[] = { "background", "foreground", "capture" };
	struct g2d_comp *comp;
	unsigned int i, registered = 0;
	int ret;

	comp = kzalloc_obj(*comp);
	if (!comp)
		return -ENOMEM;
	comp->dev = dev;
	kref_init(&comp->ref);
	kref_get(&dev->ref);
	spin_lock_init(&comp->lock);
	INIT_WORK(&comp->work, g2d_comp_work);
	comp->alpha = 255;
	comp->compose = (struct v4l2_rect) { .width = 640, .height = 480 };
	comp->media.dev = dev->dev;
	strscpy(comp->media.model, "A133 G2D compositor", sizeof(comp->media.model));
	media_device_init(&comp->media);
	dev->v4l2_dev.mdev = &comp->media;
	v4l2_ctrl_handler_init(&comp->controls, 1);
	comp->controls.lock = &dev->dev_mutex;
	comp->alpha_ctrl = v4l2_ctrl_new_std(&comp->controls, &g2d_comp_ctrl_ops,
			  V4L2_CID_ALPHA_COMPONENT, 0, 255, 1, 255);
	v4l2_ctrl_activate(comp->alpha_ctrl, false);
	if (comp->controls.error) {
		ret = comp->controls.error;
		goto free;
	}
	v4l2_subdev_init(&comp->subdev, &g2d_comp_subdev_ops);
	comp->subdev.owner = THIS_MODULE;
	strscpy(comp->subdev.name, "g2d-source-over", sizeof(comp->subdev.name));
	comp->subdev.entity.function = MEDIA_ENT_F_PROC_VIDEO_COMPOSER;
	for (i = 0; i < G2D_COMP_NODES; i++)
		comp->pads[i].flags = i == 2 ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&comp->subdev.entity, G2D_COMP_NODES, comp->pads);
	if (ret)
		goto free;
	ret = v4l2_device_register_subdev(&dev->v4l2_dev, &comp->subdev);
	if (ret)
		goto entity;
	for (i = 0; i < G2D_COMP_NODES; i++) {
		struct g2d_comp_node *node = &comp->nodes[i];
		struct video_device *vfd = &node->vfd;
		struct vb2_queue *queue = &node->queue;
		struct v4l2_format format = { .fmt.pix = { .width = 640, .height = 480 } };

		node->comp = comp;
		node->index = i;
		INIT_LIST_HEAD(&node->buffers);
		g2d_comp_try_fmt(NULL, NULL, &format);
		node->format = format.fmt.pix;
		queue->type = i == 2 ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
		/* No import/export until a multi-buffer reservation fence protocol exists. */
		queue->io_modes = VB2_MMAP;
		queue->drv_priv = node;
		queue->buf_struct_size = sizeof(struct g2d_comp_buffer);
		queue->ops = &g2d_comp_queue_ops;
		queue->mem_ops = &vb2_dma_contig_memops;
		queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		queue->lock = &dev->dev_mutex;
		queue->dev = dev->dev;
		queue->min_queued_buffers = 1;
		ret = vb2_queue_init(queue);
		if (ret)
			goto nodes;
		vfd->v4l2_dev = &dev->v4l2_dev;
		vfd->fops = &g2d_comp_fops;
		vfd->ioctl_ops = &g2d_comp_ioctl_ops;
		vfd->queue = queue;
		vfd->lock = &dev->dev_mutex;
		vfd->release = g2d_comp_video_release;
		vfd->vfl_dir = i == 2 ? VFL_DIR_RX : VFL_DIR_TX;
		vfd->device_caps = V4L2_CAP_STREAMING |
			(i == 2 ? V4L2_CAP_VIDEO_CAPTURE : V4L2_CAP_VIDEO_OUTPUT);
		if (i == 1)
			vfd->ctrl_handler = &comp->controls;
		snprintf(vfd->name, sizeof(vfd->name), "g2d-%s", names[i]);
		video_set_drvdata(vfd, node);
		node->pad.flags = i == 2 ? MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;
		ret = media_entity_pads_init(&vfd->entity, 1, &node->pad);
		if (ret)
			goto nodes;
		kref_get(&comp->ref);
		ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
		if (ret) {
			g2d_comp_video_release(vfd);
			goto nodes;
		}
		registered++;
		if (i == 2)
			ret = media_create_pad_link(&comp->subdev.entity, 2, &vfd->entity, 0,
						   MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		else
			ret = media_create_pad_link(&vfd->entity, 0, &comp->subdev.entity, i,
						   MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret)
			goto nodes;
	}
	ret = media_device_register(&comp->media);
	if (ret)
		goto nodes;
	dev->compose = comp;
	return 0;
nodes:
	while (registered)
		video_unregister_device(&comp->nodes[--registered].vfd);
	v4l2_device_unregister_subdev(&comp->subdev);
entity:
	media_entity_cleanup(&comp->subdev.entity);
free:
	dev->v4l2_dev.mdev = NULL;
	kref_put(&comp->ref, g2d_comp_release);
	return ret;
}

void sun50i_g2d_comp_unregister(struct rotate_dev *dev)
{
	struct g2d_comp *comp = dev->compose;
	unsigned int i;

	if (!comp)
		return;
	sun50i_g2d_comp_pause(dev, true);
	for (i = 0; i < G2D_COMP_NODES; i++) {
		vb2_queue_error(&comp->nodes[i].queue);
		g2d_comp_drain(&comp->nodes[i], VB2_BUF_STATE_ERROR);
	}
	/* Stop the graph while its entities still exist; late streamoff only puts PM. */
	if (dev->compose_active) {
		video_device_pipeline_stop(&comp->nodes[2].vfd);
		pm_runtime_put_noidle(dev->dev);
		dev->compose_active = false;
	}
	media_device_unregister(&comp->media);
	for (i = 0; i < G2D_COMP_NODES; i++)
		video_unregister_device(&comp->nodes[i].vfd);
	v4l2_device_unregister_subdev(&comp->subdev);
	media_entity_cleanup(&comp->subdev.entity);
	dev->v4l2_dev.mdev = NULL;
	dev->compose = NULL;
	kref_put(&comp->ref, g2d_comp_release);
}

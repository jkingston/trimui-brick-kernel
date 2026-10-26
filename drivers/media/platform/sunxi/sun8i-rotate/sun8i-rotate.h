/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Allwinner DE2 rotation driver
 *
 * Copyright (C) 2020 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#ifndef _SUN8I_ROTATE_H_
#define _SUN8I_ROTATE_H_

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/platform_device.h>
#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/workqueue.h>

#define ROTATE_NAME		"sun8i-rotate"

#define ROTATE_GLB_CTL			0x00
#define ROTATE_GLB_CTL_START			BIT(31)
#define ROTATE_GLB_CTL_RESET			BIT(30)
#define ROTATE_GLB_CTL_BURST_LEN(x)		((x) << 16)
#define ROTATE_GLB_CTL_HFLIP			BIT(7)
#define ROTATE_GLB_CTL_VFLIP			BIT(6)
#define ROTATE_GLB_CTL_ROTATION(x)		((x) << 4)
#define ROTATE_GLB_CTL_MODE(x)			((x) << 0)

#define ROTATE_INT			0x04
#define ROTATE_INT_FINISH_IRQ_EN		BIT(16)
#define ROTATE_INT_FINISH_IRQ			BIT(0)

#define ROTATE_IN_FMT			0x20
#define ROTATE_IN_FMT_FORMAT(x)			((x) << 0)

#define ROTATE_IN_SIZE			0x24
#define ROTATE_IN_PITCH0		0x30
#define ROTATE_IN_PITCH1		0x34
#define ROTATE_IN_PITCH2		0x38
#define ROTATE_IN_ADDRL0		0x40
#define ROTATE_IN_ADDRH0		0x44
#define ROTATE_IN_ADDRL1		0x48
#define ROTATE_IN_ADDRH1		0x4c
#define ROTATE_IN_ADDRL2		0x50
#define ROTATE_IN_ADDRH2		0x54
#define ROTATE_OUT_SIZE			0x84
#define ROTATE_OUT_PITCH0		0x90
#define ROTATE_OUT_PITCH1		0x94
#define ROTATE_OUT_PITCH2		0x98
#define ROTATE_OUT_ADDRL0		0xA0
#define ROTATE_OUT_ADDRH0		0xA4
#define ROTATE_OUT_ADDRL1		0xA8
#define ROTATE_OUT_ADDRH1		0xAC
#define ROTATE_OUT_ADDRL2		0xB0
#define ROTATE_OUT_ADDRH2		0xB4

#define ROTATE_BURST_8			0x07
#define ROTATE_BURST_16			0x0f
#define ROTATE_BURST_32			0x1f
#define ROTATE_BURST_64			0x3f

#define ROTATE_MODE_COPY_ROTATE		0x01

#define ROTATE_FORMAT_ARGB32		0x00
#define ROTATE_FORMAT_ABGR32		0x01
#define ROTATE_FORMAT_RGBA32		0x02
#define ROTATE_FORMAT_BGRA32		0x03
#define ROTATE_FORMAT_XRGB32		0x04
#define ROTATE_FORMAT_XBGR32		0x05
#define ROTATE_FORMAT_RGBX32		0x06
#define ROTATE_FORMAT_BGRX32		0x07
#define ROTATE_FORMAT_RGB24		0x08
#define ROTATE_FORMAT_BGR24		0x09
#define ROTATE_FORMAT_RGB565		0x0a
#define ROTATE_FORMAT_BGR565		0x0b
#define ROTATE_FORMAT_ARGB4444		0x0c
#define ROTATE_FORMAT_ABGR4444		0x0d
#define ROTATE_FORMAT_RGBA4444		0x0e
#define ROTATE_FORMAT_BGRA4444		0x0f
#define ROTATE_FORMAT_ARGB1555		0x10
#define ROTATE_FORMAT_ABGR1555		0x11
#define ROTATE_FORMAT_RGBA5551		0x12
#define ROTATE_FORMAT_BGRA5551		0x13

#define ROTATE_FORMAT_YUYV		0x20
#define ROTATE_FORMAT_UYVY		0x21
#define ROTATE_FORMAT_YVYU		0x22
#define ROTATE_FORMAT_VYUV		0x23
#define ROTATE_FORMAT_NV61		0x24
#define ROTATE_FORMAT_NV16		0x25
#define ROTATE_FORMAT_YUV422P		0x26
#define ROTATE_FORMAT_NV21		0x28
#define ROTATE_FORMAT_NV12		0x29
#define ROTATE_FORMAT_YUV420P		0x2A

#define ROTATE_SIZE(w, h)	(((h) - 1) << 16 | ((w) - 1))

#define ROTATE_MIN_WIDTH	8U
#define ROTATE_MIN_HEIGHT	8U
#define ROTATE_MAX_WIDTH	4096U
#define ROTATE_MAX_HEIGHT	4096U

struct rotate_ctx {
	struct v4l2_fh		fh;
	struct rotate_dev	*dev;

	struct v4l2_pix_format	src_fmt;
	struct v4l2_pix_format	dst_fmt;

	struct v4l2_ctrl_handler ctrl_handler;

	u32 hflip;
	u32 vflip;
	u32 rotate;
	bool stopping;
};

struct g2d_dma_job {
	struct dma_fence *fence;
	struct dma_fence **dependencies;
	unsigned int num_dependencies;
};

struct rotate_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;

	/* Device file mutex */
	struct mutex		dev_mutex;

	void __iomem		*base;
	void __iomem		*top;
	bool a133;
	bool ready;
	bool removing;
	bool resume_failed;
	bool quiescing;
	bool mixer_job;
	bool compose_active;
	struct g2d_comp *compose;
	struct kref ref;
	int irq;
	struct clk *mbus_clk;
	struct completion complete;
	struct work_struct work;
	struct rotate_ctx *job;
	u64 fence_context;
	atomic64_t fence_seqno;

	struct clk		*bus_clk;
	struct clk		*mod_clk;

	struct reset_control	*rstc;
};

struct rotate_format;

void rotate_calc_addr_pitch(dma_addr_t buffer, u32 bytesperline, u32 height,
			    const struct rotate_format *fmt, dma_addr_t *addr, u32 *pitch);

void sun50i_g2d_mixer(struct rotate_ctx *ctx, dma_addr_t src, dma_addr_t dst);

void rotate_a133_put(void *data);
void rotate_a133_begin(struct rotate_dev *dev, bool mixer);
enum vb2_buffer_state rotate_a133_wait(struct rotate_dev *dev);
int sun50i_g2d_comp_register(struct rotate_dev *dev);
void sun50i_g2d_comp_unregister(struct rotate_dev *dev);
void sun50i_g2d_comp_pause(struct rotate_dev *dev, bool pause);

int sun50i_g2d_dma_prepare(struct rotate_dev *dev,
			   struct g2d_dma_job *job,
			   struct vb2_buffer **buffers,
			   unsigned int num_buffers,
			   unsigned int write_index);
int sun50i_g2d_dma_wait(struct g2d_dma_job *job,
			bool (*cancelled)(void *data), void *data);
void sun50i_g2d_dma_finish(struct g2d_dma_job *job, int error);

#endif

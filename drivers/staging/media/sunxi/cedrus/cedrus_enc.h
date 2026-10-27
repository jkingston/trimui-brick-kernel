/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cedrus encoder hardware-contract helpers
 *
 * These definitions deliberately stop short of an encoder backend.  They
 * describe the A133 behavior established by the public Allwinner register
 * header, the matching vendor stack and successful runtime captures.  A
 * backend must satisfy every contract bit before a V4L2 encoder is exposed.
 */

#ifndef _CEDRUS_ENC_H_
#define _CEDRUS_ENC_H_

#include <linux/bits.h>
#include <linux/types.h>

#include <media/v4l2-ctrls.h>

struct cedrus_ctx;
struct cedrus_dev;

enum cedrus_enc_contract {
	CEDRUS_ENC_CONTRACT_ENGINE_ARBITRATION = BIT_ULL(0),
	CEDRUS_ENC_CONTRACT_SOURCE_LAYOUT      = BIT_ULL(1),
	CEDRUS_ENC_CONTRACT_BITSTREAM_WINDOW   = BIT_ULL(2),
	CEDRUS_ENC_CONTRACT_IRQ                = BIT_ULL(3),
	CEDRUS_ENC_CONTRACT_HEADERS            = BIT_ULL(4),
	CEDRUS_ENC_CONTRACT_AUXILIARY_BUFFERS  = BIT_ULL(5),
	CEDRUS_ENC_CONTRACT_REFERENCES         = BIT_ULL(6),
	CEDRUS_ENC_CONTRACT_KEYFRAMES          = BIT_ULL(7),
	CEDRUS_ENC_CONTRACT_RATE_CONTROL       = BIT_ULL(8),
	CEDRUS_ENC_CONTRACT_ABORT              = BIT_ULL(9),
	CEDRUS_ENC_CONTRACT_IOMMU_FAULT        = BIT_ULL(10),
	CEDRUS_ENC_CONTRACT_SYSTEM_SLEEP       = BIT_ULL(11),
	CEDRUS_ENC_CONTRACT_QP_FIELD           = BIT_ULL(12),
};

#define CEDRUS_ENC_CONTRACT_REQUIRED \
	(CEDRUS_ENC_CONTRACT_ENGINE_ARBITRATION | \
	 CEDRUS_ENC_CONTRACT_SOURCE_LAYOUT | \
	 CEDRUS_ENC_CONTRACT_BITSTREAM_WINDOW | \
	 CEDRUS_ENC_CONTRACT_IRQ | \
	 CEDRUS_ENC_CONTRACT_HEADERS | \
	 CEDRUS_ENC_CONTRACT_AUXILIARY_BUFFERS | \
	 CEDRUS_ENC_CONTRACT_REFERENCES | \
	 CEDRUS_ENC_CONTRACT_KEYFRAMES | \
	 CEDRUS_ENC_CONTRACT_RATE_CONTROL | \
	 CEDRUS_ENC_CONTRACT_ABORT | \
	 CEDRUS_ENC_CONTRACT_IOMMU_FAULT | \
	 CEDRUS_ENC_CONTRACT_SYSTEM_SLEEP | \
	 CEDRUS_ENC_CONTRACT_QP_FIELD)

/* Contract implemented by this source slice. */
#define CEDRUS_A133_H264_ENC_CONTRACT \
	(CEDRUS_ENC_CONTRACT_SOURCE_LAYOUT | \
	 CEDRUS_ENC_CONTRACT_BITSTREAM_WINDOW | \
	 CEDRUS_ENC_CONTRACT_IRQ | \
	 CEDRUS_ENC_CONTRACT_QP_FIELD)

enum cedrus_enc_irq_status {
	CEDRUS_ENC_IRQ_NONE,
	CEDRUS_ENC_IRQ_ERROR,
	CEDRUS_ENC_IRQ_OK,
};

enum cedrus_enc_abort_reason {
	CEDRUS_ENC_ABORT_STREAMOFF,
	CEDRUS_ENC_ABORT_WATCHDOG,
	CEDRUS_ENC_ABORT_IRQ_ERROR,
	CEDRUS_ENC_ABORT_IOMMU_FAULT,
};

#define CEDRUS_ENC_FORMAT_RAW		BIT(0)
#define CEDRUS_ENC_FORMAT_CODED	BIT(1)

struct cedrus_enc_format {
	u32 pixelformat;
	u32 direction;
	u32 min_width;
	u32 max_width;
	u32 min_height;
	u32 max_height;
	u32 width_align;
	u32 height_align;
	u32 stride_align;
	u32 buffer_align;
	u32 min_sizeimage;
};

struct cedrus_enc_backend_ops {
	u64 contract;
	const struct cedrus_enc_format *formats;
	unsigned int num_formats;
	const struct v4l2_ctrl_config *controls;
	unsigned int num_controls;
	int (*bind)(struct cedrus_dev *dev);
	int (*ctx_init)(struct cedrus_ctx *ctx);
	void (*ctx_cleanup)(struct cedrus_ctx *ctx);
	int (*s_ctrl)(struct cedrus_ctx *ctx, struct v4l2_ctrl *ctrl);
	bool (*supports_pair)(u32 raw_pixelformat, u32 coded_pixelformat);
	int (*start_streaming)(struct cedrus_ctx *ctx);
	void (*stop_streaming)(struct cedrus_ctx *ctx);
	int (*prepare_job)(struct cedrus_ctx *ctx);
	void (*trigger)(struct cedrus_ctx *ctx);
	enum cedrus_enc_irq_status (*irq_status)(struct cedrus_ctx *ctx);
	void (*irq_ack)(struct cedrus_ctx *ctx);
	int (*abort)(struct cedrus_ctx *ctx,
		     enum cedrus_enc_abort_reason reason);
	int (*reset_complete)(struct cedrus_ctx *ctx);
	int (*suspend)(struct cedrus_dev *dev);
	int (*resume)(struct cedrus_dev *dev);
	void (*unbind)(struct cedrus_dev *dev);
};

/*
 * irq_status consumes terminal metadata before returning ERROR or OK.  abort
 * must be idempotent if terminal handling has already retired the active
 * generation.  reset_complete is called only after the VE reset pulse
 * succeeds.  unbind runs after jobs and recovery work are drained and must
 * tolerate a partially initialized backend.  ctx_cleanup runs after mem2mem
 * has synchronously stopped the context and before its storage is released.
 */

/* Values to program into the A133 input preprocessor for one raw frame. */
struct cedrus_a133_enc_source {
	u32 input_size;
	u32 stride;
	u32 control;
	u32 mb_width;
	u32 input_size_reversed;
	dma_addr_t luma;
	dma_addr_t chroma;
	dma_addr_t chroma2;
};

/* Values to program into the AVC bitstream window registers. */
struct cedrus_a133_enc_bitstream {
	u32 start;
	u32 end;
	u32 offset;
	u32 limit;
};

/*
 * Sizes used by the A133 Ver2 H.264 path. The fixed-QP path owns work_9c and
 * leaves the deblock address zero. filter_history is the size of one of the
 * two history slots used only by the disabled vendor 3D-filter path.
 */
struct cedrus_a133_h264_enc_buffer_layout {
	size_t reconstruction_luma;
	size_t reconstruction_chroma;
	size_t reconstruction;
	size_t subpixel;
	size_t motion_vectors;
	size_t macroblock_info;
	size_t deblock;
	size_t work_9c;
	size_t filter_history;
};

/* Register values for one reconstructed/reference frame slot. */
struct cedrus_a133_h264_enc_frame_slot {
	u32 luma;
	u32 chroma;
	u32 subpixel;
};

int cedrus_enc_backend_validate(const struct cedrus_enc_backend_ops *ops);
int cedrus_enc_format_validate(const struct cedrus_enc_format *format);
bool cedrus_enc_control_is_standard(u32 id);

int cedrus_enc_bind(const struct cedrus_enc_backend_ops *ops,
		    struct cedrus_dev *dev);
int cedrus_enc_ctx_init(const struct cedrus_enc_backend_ops *ops,
			struct cedrus_ctx *ctx);
void cedrus_enc_ctx_cleanup(const struct cedrus_enc_backend_ops *ops,
			    struct cedrus_ctx *ctx);

int cedrus_enc_abort(const struct cedrus_enc_backend_ops *ops,
		     struct cedrus_ctx *ctx,
		     enum cedrus_enc_abort_reason reason);
int cedrus_enc_reset_complete(const struct cedrus_enc_backend_ops *ops,
			      struct cedrus_ctx *ctx);
void cedrus_enc_unbind(const struct cedrus_enc_backend_ops *ops,
		       struct cedrus_dev *dev);

int cedrus_a133_h264_enc_source_prepare(u32 pixelformat, u32 width,
					u32 height, u32 bytesperline,
					dma_addr_t luma, dma_addr_t chroma,
					struct cedrus_a133_enc_source *source);

int cedrus_a133_h264_enc_bitstream_prepare(dma_addr_t address, size_t size,
					   struct cedrus_a133_enc_bitstream *bs);

enum cedrus_enc_irq_status cedrus_a133_h264_enc_irq_status(u32 status);

int cedrus_a133_h264_enc_qp_update(u32 register_value, unsigned int qp,
				   u32 *updated);

int cedrus_a133_h264_enc_buffer_layout(u32 width, u32 height,
				       struct cedrus_a133_h264_enc_buffer_layout *layout);

int cedrus_a133_h264_enc_frame_slot_prepare(dma_addr_t reconstruction,
					    dma_addr_t subpixel,
					    const struct cedrus_a133_h264_enc_buffer_layout *layout,
					    struct cedrus_a133_h264_enc_frame_slot *slot);

#endif /* _CEDRUS_ENC_H_ */

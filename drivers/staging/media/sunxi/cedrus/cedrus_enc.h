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

struct cedrus_enc_backend_ops {
	u64 contract;
	int (*start_streaming)(void *priv);
	void (*stop_streaming)(void *priv);
	int (*prepare_job)(void *priv);
	void (*trigger)(void *priv);
	enum cedrus_enc_irq_status (*irq_status)(void *priv);
	void (*irq_ack)(void *priv);
	int (*abort)(void *priv);
	void (*iommu_fault)(void *priv);
	int (*suspend)(void *priv);
	void (*resume)(void *priv);
};

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

int cedrus_enc_backend_validate(const struct cedrus_enc_backend_ops *ops);

int cedrus_a133_h264_enc_source_prepare(u32 pixelformat, u32 width,
					u32 height, u32 bytesperline,
					dma_addr_t luma, dma_addr_t chroma,
					struct cedrus_a133_enc_source *source);

int cedrus_a133_h264_enc_bitstream_prepare(dma_addr_t address, size_t size,
					   struct cedrus_a133_enc_bitstream *bs);

enum cedrus_enc_irq_status cedrus_a133_h264_enc_irq_status(u32 status);

int cedrus_a133_h264_enc_qp_update(u32 register_value, unsigned int qp,
				   u32 *updated);

#endif /* _CEDRUS_ENC_H_ */

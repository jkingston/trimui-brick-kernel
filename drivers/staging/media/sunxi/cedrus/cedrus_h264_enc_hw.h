/* SPDX-License-Identifier: GPL-2.0 */
/* A133 H.264 encoder DMA and MMIO helpers. */

#ifndef _CEDRUS_H264_ENC_HW_H_
#define _CEDRUS_H264_ENC_HW_H_

#include <linux/io.h>
#include <linux/types.h>

#include "cedrus_enc.h"

/*
 * DMA register image for the independently observed A133 Ver2 path.
 * Packed codec fields and buffer contents intentionally remain outside this
 * structure until their meanings are established.
 */
struct cedrus_a133_h264_enc_dma {
	struct cedrus_a133_enc_source source;
	struct cedrus_a133_enc_bitstream bitstream;
	struct cedrus_a133_h264_enc_frame_slot reference;
	struct cedrus_a133_h264_enc_frame_slot reconstruction;
	u32 motion_vectors;
	u32 macroblock_info;
};

int cedrus_a133_h264_dma_prepare(const struct cedrus_a133_enc_source *source,
				 const struct cedrus_a133_enc_bitstream *bitstream,
				 const struct cedrus_a133_h264_enc_frame_slot *reference,
				 const struct cedrus_a133_h264_enc_frame_slot *reconstruction,
				 dma_addr_t motion_vectors,
				 dma_addr_t macroblock_info,
				 const struct cedrus_a133_h264_enc_buffer_layout *layout,
				 struct cedrus_a133_h264_enc_dma *dma);

void cedrus_a133_h264_program_source(void __iomem *base,
				     const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_bitstream(void __iomem *base,
					const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_frames(void __iomem *base,
				     const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_aux(void __iomem *base,
				  const struct cedrus_a133_h264_enc_dma *dma);

void cedrus_a133_h264_irq_arm(void __iomem *base);
u32 cedrus_a133_h264_irq_ack(void __iomem *base);
void cedrus_a133_h264_trigger(void __iomem *base);

#endif /* _CEDRUS_H264_ENC_HW_H_ */

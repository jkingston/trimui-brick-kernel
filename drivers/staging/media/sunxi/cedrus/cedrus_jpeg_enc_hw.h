/* SPDX-License-Identifier: GPL-2.0 */
/* A133 JPEG encoder MMIO helpers. */

#ifndef _CEDRUS_JPEG_ENC_HW_H_
#define _CEDRUS_JPEG_ENC_HW_H_

#include <linux/io.h>

#include "cedrus_jpeg.h"

struct cedrus_a133_jpeg_codec {
	u32 picture_control;
	u32 optimization;
};

/* Exact A133 main-frame ISP image captured for tightly packed NV12. */
struct cedrus_a133_jpeg_isp {
	u32 input_size;
	u32 input_stride;
	u32 control;
	u32 mb_width;
	u32 input_size_reversed;
	dma_addr_t output_luma;
	dma_addr_t output_chroma;
	dma_addr_t input_luma;
	dma_addr_t input_chroma;
	dma_addr_t input_chroma2;
};

int cedrus_a133_jpeg_isp_prepare(const struct cedrus_jpeg_source *source,
				 struct cedrus_a133_jpeg_isp *isp);
void cedrus_a133_jpeg_program_isp(void __iomem *base,
				  const struct cedrus_a133_jpeg_isp *isp);
void cedrus_a133_jpeg_select(void __iomem *base);

int cedrus_a133_jpeg_codec_prepare(unsigned int width,
				   unsigned int height,
				   unsigned int quality,
				   const struct cedrus_jpeg_tables *tables,
				   struct cedrus_a133_jpeg_codec *codec);

void cedrus_a133_jpeg_program_capture(void __iomem *base,
				      const struct cedrus_jpeg_capture *capture);
void cedrus_a133_jpeg_program_quant_tables(void __iomem *base,
					   const struct cedrus_jpeg_tables *tables);
int cedrus_a133_jpeg_prepare_phase(void __iomem *base,
				   const struct cedrus_a133_jpeg_codec *codec,
				 const struct cedrus_jpeg_capture *capture,
				 const struct cedrus_jpeg_tables *tables,
				 const u8 *header, size_t header_size);
void cedrus_a133_jpeg_trigger(void __iomem *base);
u32 cedrus_a133_jpeg_irq_read(void __iomem *base);
void cedrus_a133_jpeg_irq_ack_status(void __iomem *base, u32 status);
u32 cedrus_a133_jpeg_irq_ack(void __iomem *base);
u32 cedrus_a133_jpeg_produced_bits(void __iomem *base);

#endif /* _CEDRUS_JPEG_ENC_HW_H_ */

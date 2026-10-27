// SPDX-License-Identifier: GPL-2.0
/* A133 JPEG encoder MMIO helpers. */

#include <linux/io.h>

#include "cedrus_enc_regs.h"
#include "cedrus_jpeg_enc_hw.h"

void cedrus_a133_jpeg_program_capture(void __iomem *base,
				      const struct cedrus_jpeg_capture *capture)
{
	writel(capture->start, base + VE_ENC_AVC_BITSTREAM_START);
	writel(capture->end, base + VE_ENC_AVC_BITSTREAM_END);
	writel(capture->offset, base + VE_ENC_AVC_BITSTREAM_OFFSET);
	writel(capture->limit, base + VE_ENC_AVC_BITSTREAM_LIMIT);
}

void cedrus_a133_jpeg_program_quant_tables(void __iomem *base,
					   const struct cedrus_jpeg_tables *tables)
{
	unsigned int i;

	/* The data register is an auto-incrementing SRAM port, not an array. */
	writel(0, base + VE_ENC_AVC_SRAM_OFFSET);
	for (i = 0; i < CEDRUS_JPEG_HW_TABLE_WORDS; i++)
		writel(tables->hw[i], base + VE_ENC_AVC_SRAM_DATA);
}

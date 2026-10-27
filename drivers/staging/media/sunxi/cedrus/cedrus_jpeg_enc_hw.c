// SPDX-License-Identifier: GPL-2.0
/* A133 JPEG encoder MMIO helpers. */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/io.h>

#include "cedrus_enc_regs.h"
#include "cedrus_jpeg_enc_hw.h"

#define CEDRUS_A133_JPEG_DIMENSION_UNIT	8U
#define CEDRUS_A133_JPEG_MAX_DIMENSION	4096U
#define CEDRUS_A133_JPEG_IRQ_MASK	0xfU
#define CEDRUS_A133_JPEG_PUT_BYTE_POLLS	10000U
#define CEDRUS_A133_JPEG_DC_NUMERATOR	1024U

int cedrus_a133_jpeg_codec_prepare(unsigned int width,
				   unsigned int height,
				   unsigned int quality,
				   const struct cedrus_jpeg_tables *tables,
				   struct cedrus_a133_jpeg_codec *codec)
{
	u32 dc_chroma;
	u32 dc_luma;

	if (!tables || !codec || !width || !height)
		return -EINVAL;
	if (width > CEDRUS_A133_JPEG_MAX_DIMENSION ||
	    height > CEDRUS_A133_JPEG_MAX_DIMENSION)
		return -ERANGE;
	if (!IS_ALIGNED(width, 16) || !IS_ALIGNED(height, 16))
		return -EOPNOTSUPP;

	/* Quality 95 is the independently captured Brick command profile. */
	if (quality != 95 || tables->luma[0] != 2 || tables->chroma[0] != 2)
		return -EOPNOTSUPP;

	dc_luma = CEDRUS_A133_JPEG_DC_NUMERATOR / tables->luma[0];
	dc_chroma = CEDRUS_A133_JPEG_DC_NUMERATOR / tables->chroma[0];

	*codec = (struct cedrus_a133_jpeg_codec) {
		.picture_size = width / CEDRUS_A133_JPEG_DIMENSION_UNIT << 16 |
				height / CEDRUS_A133_JPEG_DIMENSION_UNIT,
		.picture_control = VE_ENC_JPEG_HEADER_CONTROL |
				   VE_ENC_JPEG_STUFF_ZERO |
				   FIELD_PREP(VE_ENC_JPEG_DC_LUMA, dc_luma) |
				   FIELD_PREP(VE_ENC_JPEG_DC_CHROMA, dc_chroma),
		.optimization = VE_ENC_JPEG_OPTIMIZATION_DISABLED,
	};

	return 0;
}

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

static void cedrus_a133_jpeg_irq_arm(void __iomem *base)
{
	u32 status;

	writel(CEDRUS_A133_JPEG_IRQ_MASK, base + VE_ENC_AVC_IRQ_CONTROL);
	status = readl(base + VE_ENC_AVC_STATUS);
	writel(status | VE_ENC_AVC_STATUS_LOW_MASK, base + VE_ENC_AVC_STATUS);
}

static int cedrus_a133_jpeg_put_byte(void __iomem *base, u8 value)
{
	unsigned int poll;

	writel(value, base + VE_ENC_AVC_PUT_BITS_DATA);
	writel(VE_ENC_JPEG_TRIGGER_PUT_BYTE, base + VE_ENC_AVC_TRIGGER);

	for (poll = 0; poll < CEDRUS_A133_JPEG_PUT_BYTE_POLLS; poll++)
		if (readl(base + VE_ENC_AVC_STATUS) &
		    VE_ENC_AVC_STATUS_PUT_BITS_READY)
			return 0;

	return -ETIMEDOUT;
}

int cedrus_a133_jpeg_start_phase(void __iomem *base,
				 const struct cedrus_a133_jpeg_codec *codec,
				 const struct cedrus_jpeg_capture *capture,
				 const struct cedrus_jpeg_tables *tables,
				 const u8 *header, size_t header_size)
{
	unsigned int i;
	int ret;

	if (!base || !codec || !capture || !tables || !header)
		return -EINVAL;
	if (header_size != CEDRUS_JPEG_HEADER_SIZE || capture->prefix ||
	    capture->offset)
		return -EOPNOTSUPP;
	if (capture->limit < header_size * 8U + 16U)
		return -ENOSPC;

	/* Exact stock order after the ISP source registers have been prepared. */
	cedrus_a133_jpeg_irq_arm(base);
	cedrus_a133_jpeg_program_capture(base, capture);
	writel(codec->picture_size, base + VE_ENC_AVC_PICTURE_SIZE);
	writel(VE_ENC_JPEG_HEADER_CONTROL,
	       base + VE_ENC_AVC_PICTURE_CONTROL);

	for (i = 0; i < header_size; i++) {
		ret = cedrus_a133_jpeg_put_byte(base, header[i]);
		if (ret)
			return ret;
	}

	writel(codec->picture_control, base + VE_ENC_AVC_PICTURE_CONTROL);
	writel(codec->optimization, base + VE_ENC_AVC_QP);
	cedrus_a133_jpeg_program_quant_tables(base, tables);
	writel(VE_ENC_JPEG_TRIGGER_START, base + VE_ENC_AVC_TRIGGER);

	return 0;
}

u32 cedrus_a133_jpeg_irq_ack(void __iomem *base)
{
	u32 status = readl(base + VE_ENC_AVC_STATUS);

	writel(status, base + VE_ENC_AVC_STATUS);
	return status;
}

u32 cedrus_a133_jpeg_produced_bits(void __iomem *base)
{
	return readl(base + VE_ENC_AVC_BITSTREAM_LENGTH);
}

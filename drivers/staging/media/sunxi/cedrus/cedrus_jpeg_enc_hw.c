// SPDX-License-Identifier: GPL-2.0
/* A133 JPEG encoder MMIO helpers. */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/overflow.h>

#include "cedrus_enc_regs.h"
#include "cedrus_jpeg_enc_hw.h"

#define CEDRUS_A133_JPEG_DIMENSION_UNIT	8U
#define CEDRUS_A133_JPEG_MAX_DIMENSION	4096U
#define CEDRUS_A133_JPEG_IRQ_MASK	0xfU
#define CEDRUS_A133_JPEG_PUT_BYTE_POLLS	10000U
#define CEDRUS_A133_JPEG_DC_NUMERATOR	1024U

static const u32 cedrus_a133_jpeg_isp_sram[64] = {
	0x01000000, 0x00000000, 0x00fefffa, 0x0000000a,
	0x00f7fff5, 0xffff0017, 0x00ecfff1, 0xfffd0027,
	0x00deffef, 0xfffb003a, 0x00cdffee, 0xfff8004e,
	0x00baffee, 0xfff60064, 0x00a6ffef, 0xfff3007a,
	0x0090fff1, 0xfff10090, 0x007afff3, 0xffef00a6,
	0x0064fff6, 0xffee00ba, 0x004efff8, 0xffee00cd,
	0x003afffb, 0xffef00de, 0x0027fffd, 0xfff100ec,
	0x0017ffff, 0xfff500f7, 0x000a0000, 0xfffa00fe,
	0x00000100, 0x000800f8, 0x001000f0, 0x001800e8,
	0x002000e0, 0x002800d8, 0x003000d0, 0x003800c8,
	0x004000c0, 0x004800b8, 0x005000b0, 0x005800a8,
	0x006000a0, 0x00680098, 0x00700090, 0x00780088,
	0x00800080, 0x00880078, 0x00900070, 0x00980068,
	0x00a00060, 0x00a80058, 0x00b00050, 0x00b80048,
	0x00c00040, 0x00c80038, 0x00d00030, 0x00d80028,
	0x00e00020, 0x00e80018, 0x00f00010, 0x00f80008,
};

int cedrus_a133_jpeg_isp_prepare(const struct cedrus_jpeg_source *source,
				 struct cedrus_a133_jpeg_isp *isp)
{
	dma_addr_t input_chroma2;

	if (!source || !isp || !source->width || !source->height ||
	    source->stride != source->width)
		return -EINVAL;
	if (!IS_ALIGNED(source->width, 16) ||
	    !IS_ALIGNED(source->height, 16))
		return -EINVAL;
	if (source->width / 8 > U16_MAX || source->height / 8 > U16_MAX ||
	    source->stride / 16 > U16_MAX)
		return -ERANGE;
	if (source->luma > U32_MAX || source->chroma > U32_MAX)
		return -ERANGE;
	if (check_add_overflow(source->chroma,
			       (dma_addr_t)source->chroma_size / 2,
			       &input_chroma2) || input_chroma2 > U32_MAX)
		return -ERANGE;

	*isp = (struct cedrus_a133_jpeg_isp) {
		.input_size = source->width / 8 << 16 | source->height / 8,
		.input_stride = source->stride / 16 << 16 |
				 source->stride / 16,
		.control = 0,
		.mb_width = source->width / 16,
		.input_size_reversed = source->height / 8 << 16 |
				       source->width / 8,
		/* Plain JFIF disables ISP writeback; thumbnail mode owns temps. */
		.output_luma = 0,
		.output_chroma = 0,
		.input_luma = source->luma,
		.input_chroma = source->chroma,
		.input_chroma2 = input_chroma2,
	};

	return 0;
}

void cedrus_a133_jpeg_program_isp(void __iomem *base,
				  const struct cedrus_a133_jpeg_isp *isp)
{
	unsigned int i;

	/* Preserve SetIspRegister's observed A133 main-frame write order. */
	writel(isp->input_size, base + VE_ENC_ISP_INPUT_SIZE);
	writel(0, base + VE_ENC_ISP_FIXED_38);
	writel(0, base + VE_ENC_ISP_SRAM_OFFSET);
	for (i = 0; i < ARRAY_SIZE(cedrus_a133_jpeg_isp_sram); i++)
		writel(cedrus_a133_jpeg_isp_sram[i],
		       base + VE_ENC_ISP_SRAM_DATA);
	writel(isp->output_luma, base + VE_ENC_ISP_OUTPUT_LUMA);
	writel(isp->output_chroma, base + VE_ENC_ISP_OUTPUT_CHROMA);
	writel(isp->input_stride, base + VE_ENC_ISP_INPUT_STRIDE);
	writel(isp->mb_width, base + VE_ENC_ISP_MB_WIDTH);
	writel(isp->input_size_reversed,
	       base + VE_ENC_ISP_INPUT_SIZE_REVERSED);
	writel(isp->control, base + VE_ENC_ISP_CONTROL);
	writel(isp->input_luma, base + VE_ENC_ISP_INPUT_LUMA);
	writel(isp->input_chroma, base + VE_ENC_ISP_INPUT_CHROMA);
	writel(isp->input_chroma2, base + VE_ENC_ISP_INPUT_CHROMA2);
}

void cedrus_a133_jpeg_select(void __iomem *base)
{
	writel(VE_ENC_JPEG_TRIGGER_SELECT, base + VE_ENC_AVC_TRIGGER);
}

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

int cedrus_a133_jpeg_prepare_phase(void __iomem *base,
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

	return 0;
}

void cedrus_a133_jpeg_trigger(void __iomem *base)
{
	writel(VE_ENC_JPEG_TRIGGER_START, base + VE_ENC_AVC_TRIGGER);
}

u32 cedrus_a133_jpeg_irq_read(void __iomem *base)
{
	return readl(base + VE_ENC_AVC_STATUS);
}

void cedrus_a133_jpeg_irq_ack_status(void __iomem *base, u32 status)
{
	writel(status, base + VE_ENC_AVC_STATUS);
}

u32 cedrus_a133_jpeg_irq_ack(void __iomem *base)
{
	u32 status = cedrus_a133_jpeg_irq_read(base);

	cedrus_a133_jpeg_irq_ack_status(base, status);
	return status;
}

u32 cedrus_a133_jpeg_produced_bits(void __iomem *base)
{
	return readl(base + VE_ENC_AVC_BITSTREAM_LENGTH);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner Cedrus JPEG encoder helpers
 *
 * This file contains the software-only portion of the A133 JPEG encoder
 * contract. Hardware admission and register programming remain separate so a
 * format is not advertised before the source path and recovery lifecycle are
 * implemented and qualified.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <media/v4l2-jpeg.h>

#include "cedrus_jpeg.h"

struct cedrus_jpeg_writer {
	u8 *cur;
	u8 *end;
};

static int cedrus_jpeg_put_u8(struct cedrus_jpeg_writer *writer, u8 value)
{
	if (writer->cur == writer->end)
		return -ENOSPC;

	*writer->cur++ = value;
	return 0;
}

static int cedrus_jpeg_put_be16(struct cedrus_jpeg_writer *writer, u16 value)
{
	if ((size_t)(writer->end - writer->cur) < sizeof(value))
		return -ENOSPC;

	put_unaligned_be16(value, writer->cur);
	writer->cur += sizeof(value);
	return 0;
}

static int cedrus_jpeg_put_data(struct cedrus_jpeg_writer *writer,
				const u8 *data, size_t size)
{
	if ((size_t)(writer->end - writer->cur) < size)
		return -ENOSPC;

	memcpy(writer->cur, data, size);
	writer->cur += size;
	return 0;
}

static u8 cedrus_jpeg_scale_quant(u8 value, unsigned int scale)
{
	unsigned int scaled;

	scaled = DIV_ROUND_CLOSEST((unsigned int)value * scale, 100);
	return clamp_t(unsigned int, scaled, 1, 255);
}

static u32 cedrus_jpeg_hw_quant(u8 value)
{
	u32 reciprocal = min_t(u32, U16_MAX, 65536U / value + 1);

	/*
	 * The A133 SRAM entry is a little-endian pair of 16-bit values:
	 * floor(65536 / q) + 1 followed by floor(q / 2). The reciprocal is
	 * saturated because q=1 produces 65537.
	 */
	return reciprocal | ((u32)(value >> 1) << 16);
}

int cedrus_jpeg_prepare_tables(struct cedrus_jpeg_tables *tables, unsigned int quality)
{
	unsigned int scale;
	unsigned int i;

	if (!tables || quality < 1 || quality > 100)
		return -EINVAL;

	if (quality < 50)
		scale = 5000 / quality;
	else
		scale = 2 * (100 - quality);

	for (i = 0; i < CEDRUS_JPEG_QUANT_SIZE; i++) {
		tables->luma[i] = cedrus_jpeg_scale_quant(v4l2_jpeg_ref_table_luma_qt[i],
							  scale);
		tables->chroma[i] = cedrus_jpeg_scale_quant(v4l2_jpeg_ref_table_chroma_qt[i],
							    scale);
		tables->hw[i] = cedrus_jpeg_hw_quant(tables->luma[i]);
		tables->hw[CEDRUS_JPEG_QUANT_SIZE + i] =
			cedrus_jpeg_hw_quant(tables->chroma[i]);
	}

	return 0;
}

static int cedrus_jpeg_write_dqt(struct cedrus_jpeg_writer *writer, u8 id,
				 const u8 *table)
{
	unsigned int i;
	int ret;

	ret = cedrus_jpeg_put_be16(writer, 0xffdb);
	ret |= cedrus_jpeg_put_be16(writer, CEDRUS_JPEG_QUANT_SIZE + 3);
	ret |= cedrus_jpeg_put_u8(writer, id);
	if (ret)
		return -ENOSPC;

	for (i = 0; i < CEDRUS_JPEG_QUANT_SIZE; i++) {
		u8 value = table[v4l2_jpeg_zigzag_scan_index[i]];

		ret = cedrus_jpeg_put_u8(writer, value);
		if (ret)
			return ret;
	}

	return 0;
}

static int cedrus_jpeg_write_sof(struct cedrus_jpeg_writer *writer,
				 unsigned int width, unsigned int height)
{
	static const u8 components[] = {
		1, 0x22, 0,
		2, 0x11, 1,
		3, 0x11, 1,
	};
	int ret;

	ret = cedrus_jpeg_put_be16(writer, 0xffc0);
	ret |= cedrus_jpeg_put_be16(writer, 17);
	ret |= cedrus_jpeg_put_u8(writer, 8);
	ret |= cedrus_jpeg_put_be16(writer, height);
	ret |= cedrus_jpeg_put_be16(writer, width);
	ret |= cedrus_jpeg_put_u8(writer, 3);
	ret |= cedrus_jpeg_put_data(writer, components, sizeof(components));

	return ret ? -ENOSPC : 0;
}

static int cedrus_jpeg_write_dht(struct cedrus_jpeg_writer *writer, u8 id,
				 const u8 *table, size_t size)
{
	int ret;

	ret = cedrus_jpeg_put_be16(writer, 0xffc4);
	ret |= cedrus_jpeg_put_be16(writer, size + 3);
	ret |= cedrus_jpeg_put_u8(writer, id);
	ret |= cedrus_jpeg_put_data(writer, table, size);

	return ret ? -ENOSPC : 0;
}

static int cedrus_jpeg_write_sos(struct cedrus_jpeg_writer *writer)
{
	static const u8 scan[] = {
		3,
		1, 0x00,
		2, 0x11,
		3, 0x11,
		0, 63, 0,
	};
	int ret;

	ret = cedrus_jpeg_put_be16(writer, 0xffda);
	ret |= cedrus_jpeg_put_be16(writer, 12);
	ret |= cedrus_jpeg_put_data(writer, scan, sizeof(scan));

	return ret ? -ENOSPC : 0;
}

int cedrus_jpeg_write_header(void *buffer, size_t size, unsigned int width,
			     unsigned int height,
			     const struct cedrus_jpeg_tables *tables)
{
	struct cedrus_jpeg_writer writer;
	int ret;

	if (!buffer || !tables || !width || !height ||
	    width > U16_MAX || height > U16_MAX)
		return -EINVAL;
	if (size < CEDRUS_JPEG_HEADER_SIZE)
		return -ENOSPC;

	writer.cur = buffer;
	writer.end = writer.cur + CEDRUS_JPEG_HEADER_SIZE;

	ret = cedrus_jpeg_put_be16(&writer, 0xffd8);
	ret |= cedrus_jpeg_write_dqt(&writer, 0, tables->luma);
	ret |= cedrus_jpeg_write_dqt(&writer, 1, tables->chroma);
	ret |= cedrus_jpeg_write_sof(&writer, width, height);
	ret |= cedrus_jpeg_write_dht(&writer, V4L2_JPEG_LUM_HT |
				   V4L2_JPEG_DC_HT,
				   v4l2_jpeg_ref_table_luma_dc_ht,
				   V4L2_JPEG_REF_HT_DC_LEN);
	ret |= cedrus_jpeg_write_dht(&writer, V4L2_JPEG_LUM_HT |
				   V4L2_JPEG_AC_HT,
				   v4l2_jpeg_ref_table_luma_ac_ht,
				   V4L2_JPEG_REF_HT_AC_LEN);
	ret |= cedrus_jpeg_write_dht(&writer, V4L2_JPEG_CHR_HT |
				   V4L2_JPEG_DC_HT,
				   v4l2_jpeg_ref_table_chroma_dc_ht,
				   V4L2_JPEG_REF_HT_DC_LEN);
	ret |= cedrus_jpeg_write_dht(&writer, V4L2_JPEG_CHR_HT |
				   V4L2_JPEG_AC_HT,
				   v4l2_jpeg_ref_table_chroma_ac_ht,
				   V4L2_JPEG_REF_HT_AC_LEN);
	ret |= cedrus_jpeg_write_sos(&writer);
	if (ret)
		return -ENOSPC;

	if (WARN_ON_ONCE(writer.cur - (u8 *)buffer != CEDRUS_JPEG_HEADER_SIZE))
		return -EIO;

	return CEDRUS_JPEG_HEADER_SIZE;
}

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
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/videodev2.h>

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

int cedrus_jpeg_prepare_source(struct cedrus_jpeg_source *source, u32 pixelformat,
			       unsigned int width, unsigned int height,
			       unsigned int bytesperline, size_t sizeimage,
			       dma_addr_t address)
{
	size_t luma_size;
	size_t chroma_size;
	size_t required;

	if (!source || pixelformat != V4L2_PIX_FMT_NV12)
		return -EINVAL;

	/*
	 * Stock proves the A133 JPEG path with NV12 at 320x240.
	 * Keep the initial contract on complete 16x16 MCU boundaries and tightly
	 * packed lines. In particular, stock silently programmed a 320-pixel line
	 * for a requested 336-pixel stride, so padded input must fail closed. The
	 * 4096-pixel upper bound is the declared A133 JPEG dimension limit.
	 */
	if (width < 16 || height < 16 || width > 4096 || height > 4096 ||
	    !IS_ALIGNED(width, 16) || !IS_ALIGNED(height, 16) ||
	    bytesperline != width)
		return -EINVAL;

	if (!IS_ALIGNED(address, 16))
		return -EINVAL;
	if (address > U32_MAX)
		return -ERANGE;

	if (check_mul_overflow((size_t)bytesperline, (size_t)height,
			       &luma_size))
		return -EOVERFLOW;

	chroma_size = luma_size / 2;
	if (check_add_overflow(luma_size, chroma_size, &required))
		return -EOVERFLOW;
	if (sizeimage < required)
		return -ENOSPC;
	if (required - 1 > U32_MAX - (u32)address)
		return -ERANGE;

	source->luma = address;
	source->chroma = address + luma_size;
	source->luma_size = luma_size;
	source->chroma_size = chroma_size;
	source->width = width;
	source->height = height;
	source->stride = bytesperline;

	return 0;
}

int cedrus_jpeg_prepare_capture(struct cedrus_jpeg_capture *capture,
				dma_addr_t address, size_t capacity,
				size_t prefix)
{
	dma_addr_t end;
	u32 limit;

	if (!capture || !capacity ||
	    !IS_ALIGNED(address, CEDRUS_JPEG_BITSTREAM_ALIGNMENT) ||
	    !IS_ALIGNED(capacity, CEDRUS_JPEG_BITSTREAM_ALIGNMENT))
		return -EINVAL;
	if (address > U32_MAX || capacity - 1 > U32_MAX - (u32)address)
		return -ERANGE;
	if (capacity < 2 || prefix > capacity - 2)
		return -ENOSPC;
	/* Stock caps this bit count for the Brick's low IC version 0x12010. */
	if (capacity > CEDRUS_JPEG_BITSTREAM_MAX_BITS / 8)
		return -E2BIG;
	if (check_mul_overflow((u32)capacity, 8U, &limit))
		return -EOVERFLOW;

	end = address + capacity - 1;
	capture->start = address >> 8;
	capture->end = end >> 8;
	capture->offset = prefix << 3;
	capture->limit = limit;
	capture->prefix = prefix;
	capture->capacity = capacity;

	return 0;
}

int cedrus_jpeg_finish_capture(void *buffer,
			       const struct cedrus_jpeg_capture *capture,
			       u32 status, u32 produced_bits, size_t *bytesused)
{
	size_t produced;
	u8 *data = buffer;

	if (!buffer || !capture || !bytesused)
		return -EINVAL;
	if (capture->capacity < 2 || capture->prefix > capture->capacity - 2)
		return -EINVAL;
	if (status & CEDRUS_JPEG_STATUS_BUFFER_FULL)
		return -ENOSPC;
	if (status & CEDRUS_JPEG_STATUS_MB_OVERFLOW)
		return -EIO;
	if (!(status & CEDRUS_JPEG_STATUS_FINISH))
		return -EIO;
	if (produced_bits > capture->limit)
		return -ENOSPC;
	if (produced_bits & 7)
		return -EIO;

	produced = produced_bits >> 3;
	if (produced < capture->prefix)
		return -EIO;
	if (produced > capture->capacity - 2)
		return -ENOSPC;

	/* The A133 encoder reports entropy bytes without the EOI marker. */
	data[produced] = 0xff;
	data[produced + 1] = 0xd9;
	*bytesused = produced + 2;

	return 0;
}

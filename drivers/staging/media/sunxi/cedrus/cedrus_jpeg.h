/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CEDRUS_JPEG_H_
#define _CEDRUS_JPEG_H_

#include <linux/bits.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>

#define CEDRUS_JPEG_QUANT_SIZE		64
#define CEDRUS_JPEG_HW_TABLE_WORDS	(2 * CEDRUS_JPEG_QUANT_SIZE)
#define CEDRUS_JPEG_HEADER_SIZE		623
#define CEDRUS_JPEG_BITSTREAM_ALIGNMENT	256
#define CEDRUS_JPEG_BITSTREAM_MAX_BITS	0x0fff0000U

#define CEDRUS_JPEG_STATUS_FINISH	BIT(0)
#define CEDRUS_JPEG_STATUS_BUFFER_FULL	BIT(1)
#define CEDRUS_JPEG_STATUS_MB_OVERFLOW	BIT(2)
#define CEDRUS_JPEG_STATUS_TIMEOUT	BIT(3)

struct cedrus_jpeg_tables {
	u8 luma[CEDRUS_JPEG_QUANT_SIZE];
	u8 chroma[CEDRUS_JPEG_QUANT_SIZE];
	u32 hw[CEDRUS_JPEG_HW_TABLE_WORDS];
};

struct cedrus_jpeg_source {
	dma_addr_t luma;
	dma_addr_t chroma;
	size_t luma_size;
	size_t chroma_size;
	u32 pixelformat;
	u32 width;
	u32 height;
	u32 stride;
};

struct cedrus_jpeg_capture {
	u32 start;
	u32 end;
	u32 offset;
	u32 limit;
	size_t prefix;
	size_t capacity;
};

int cedrus_jpeg_prepare_tables(struct cedrus_jpeg_tables *tables, unsigned int quality);
int cedrus_jpeg_write_header(void *buffer, size_t size, unsigned int width,
			     unsigned int height,
			     const struct cedrus_jpeg_tables *tables);
int cedrus_jpeg_prepare_source(struct cedrus_jpeg_source *source, u32 pixelformat,
			       unsigned int width, unsigned int height,
			       unsigned int bytesperline, size_t sizeimage,
			       dma_addr_t address);
int cedrus_jpeg_prepare_capture(struct cedrus_jpeg_capture *capture,
				dma_addr_t address, size_t capacity,
				size_t prefix);
int cedrus_jpeg_validate_capture(const struct cedrus_jpeg_capture *capture,
				 u32 status, u32 produced_bits,
				 size_t *bytesused);
int cedrus_jpeg_append_eoi(void *buffer, size_t capacity, size_t bytesused);
int cedrus_jpeg_finish_capture(void *buffer,
			       const struct cedrus_jpeg_capture *capture,
			       u32 status, u32 produced_bits, size_t *bytesused);

#endif

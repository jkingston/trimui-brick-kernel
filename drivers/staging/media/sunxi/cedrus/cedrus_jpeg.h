/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CEDRUS_JPEG_H_
#define _CEDRUS_JPEG_H_

#include <linux/types.h>

#define CEDRUS_JPEG_QUANT_SIZE		64
#define CEDRUS_JPEG_HW_TABLE_WORDS	(2 * CEDRUS_JPEG_QUANT_SIZE)
#define CEDRUS_JPEG_HEADER_SIZE		605

struct cedrus_jpeg_tables {
	u8 luma[CEDRUS_JPEG_QUANT_SIZE];
	u8 chroma[CEDRUS_JPEG_QUANT_SIZE];
	u32 hw[CEDRUS_JPEG_HW_TABLE_WORDS];
};

int cedrus_jpeg_prepare_tables(struct cedrus_jpeg_tables *tables, unsigned int quality);
int cedrus_jpeg_write_header(void *buffer, size_t size, unsigned int width,
			     unsigned int height,
			     const struct cedrus_jpeg_tables *tables);

#endif

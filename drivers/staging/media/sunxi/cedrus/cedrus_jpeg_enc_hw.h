/* SPDX-License-Identifier: GPL-2.0 */
/* A133 JPEG encoder MMIO helpers. */

#ifndef _CEDRUS_JPEG_ENC_HW_H_
#define _CEDRUS_JPEG_ENC_HW_H_

#include <linux/io.h>

#include "cedrus_jpeg.h"

void cedrus_a133_jpeg_program_capture(void __iomem *base,
				      const struct cedrus_jpeg_capture *capture);
void cedrus_a133_jpeg_program_quant_tables(void __iomem *base,
					   const struct cedrus_jpeg_tables *tables);

#endif /* _CEDRUS_JPEG_ENC_HW_H_ */

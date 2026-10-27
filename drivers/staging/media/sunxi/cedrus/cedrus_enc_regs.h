/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A133 encoder registers with source-correlated runtime evidence.
 *
 * Keep unresolved packed-control, header and auxiliary-buffer fields out of
 * this file.  Names and offsets match Allwinner's public encoder register
 * header; field semantics below were also observed on an A133.
 */

#ifndef _CEDRUS_ENC_REGS_H_
#define _CEDRUS_ENC_REGS_H_

#include <linux/bits.h>

/* Input preprocessor.  Address registers contain full byte addresses. */
#define VE_ENC_ISP_INPUT_SIZE		0xa00
#define VE_ENC_ISP_INPUT_STRIDE		0xa04
#define VE_ENC_ISP_CONTROL		0xa08
#define VE_ENC_ISP_MB_WIDTH		0xa14
#define VE_ENC_ISP_INPUT_SIZE_REVERSED	0xa2c
#define VE_ENC_ISP_INPUT_LUMA		0xa78
#define VE_ENC_ISP_INPUT_CHROMA		0xa7c
#define VE_ENC_ISP_INPUT_CHROMA2		0xa80

#define VE_ENC_ISP_CONTROL_PLANAR	GENMASK(31, 30)

/* AVC encoder.  Bitstream addresses are expressed in 256-byte units. */
#define VE_ENC_AVC_PICTURE_SIZE		0xb00
#define VE_ENC_AVC_PICTURE_CONTROL	0xb04
#define VE_ENC_AVC_QP			0xb08
#define VE_ENC_AVC_MOTION_CONTROL	0xb10
#define VE_ENC_AVC_IRQ_CONTROL		0xb14
#define VE_ENC_AVC_TRIGGER		0xb18
#define VE_ENC_AVC_STATUS		0xb1c
#define VE_ENC_AVC_PUT_BITS_DATA		0xb20
#define VE_ENC_AVC_MOTION_VECTORS	0xb60
#define VE_ENC_AVC_BITSTREAM_START	0xb80
#define VE_ENC_AVC_BITSTREAM_END		0xb84
#define VE_ENC_AVC_BITSTREAM_OFFSET	0xb88
#define VE_ENC_AVC_BITSTREAM_LIMIT	0xb8c
#define VE_ENC_AVC_BITSTREAM_LENGTH	0xb90
#define VE_ENC_AVC_WORK_9C		0xb9c
#define VE_ENC_AVC_SRAM_OFFSET		0xbe0
#define VE_ENC_AVC_SRAM_DATA		0xbe4
#define VE_ENC_AVC_REFERENCE_LUMA	0xba0
#define VE_ENC_AVC_REFERENCE_CHROMA	0xba4
#define VE_ENC_AVC_RECONSTRUCTION_LUMA	0xbb0
#define VE_ENC_AVC_RECONSTRUCTION_CHROMA	0xbb4
#define VE_ENC_AVC_SUBPIXEL_REFERENCE	0xbb8
#define VE_ENC_AVC_SUBPIXEL_CURRENT	0xbbc
#define VE_ENC_AVC_MACROBLOCK_INFO	0xbc0
#define VE_ENC_AVC_DEBLOCK		0xbc4

#define VE_ENC_AVC_TRIGGER_SLICE		0x8
#define VE_ENC_AVC_TRIGGER_PUT_BITS	BIT(0)
#define VE_ENC_AVC_TRIGGER_BIT_COUNT	GENMASK(13, 8)

/* Fields isolated on the stock A133 fixed-QP High/CABAC path. */
#define VE_ENC_AVC_PICTURE_CONTROL_P_SLICE	BIT(4)
#define VE_ENC_AVC_PICTURE_CONTROL_CABAC	BIT(8)
#define VE_ENC_AVC_PICTURE_CONTROL_FIX_MODE_NUM	GENMASK(11, 10)
#define VE_ENC_AVC_QP_VALUE		GENMASK(5, 0)
#define VE_ENC_AVC_QP_DYNAMIC_ME	BIT(8)
#define VE_ENC_AVC_QP_LENGTH_STRIDE	GENMASK(13, 10)
#define VE_ENC_AVC_QP_STOCK_VER2	BIT(27)

/* Low status bits are write-one-to-clear. */
#define VE_ENC_AVC_STATUS_DONE		BIT(0)
#define VE_ENC_AVC_STATUS_SIZE_ERROR	BIT(1)
#define VE_ENC_AVC_STATUS_MB_ERROR	BIT(2)
#define VE_ENC_AVC_STATUS_LOW_MASK	GENMASK(2, 0)
#define VE_ENC_AVC_STATUS_PUT_BITS_READY	BIT(9)

/* JPEG commands use the AVC encoder register bank on the A133. */
#define VE_ENC_JPEG_HEADER_CONTROL		BIT(31)
#define VE_ENC_JPEG_STUFF_ZERO			BIT(30)
#define VE_ENC_JPEG_DC_LUMA			GENMASK(10, 0)
#define VE_ENC_JPEG_DC_CHROMA			GENMASK(26, 16)
#define VE_ENC_JPEG_OPTIMIZATION_DISABLED	GENMASK(31, 30)
#define VE_ENC_JPEG_TRIGGER_PUT_BYTE		0x00010801
#define VE_ENC_JPEG_TRIGGER_START		0x00010008

#endif /* _CEDRUS_ENC_REGS_H_ */

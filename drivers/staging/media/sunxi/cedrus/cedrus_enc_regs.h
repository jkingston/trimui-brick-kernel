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
#define VE_ENC_AVC_QP			0xb08
#define VE_ENC_AVC_IRQ_CONTROL		0xb14
#define VE_ENC_AVC_TRIGGER		0xb18
#define VE_ENC_AVC_STATUS		0xb1c
#define VE_ENC_AVC_BITSTREAM_START	0xb80
#define VE_ENC_AVC_BITSTREAM_END		0xb84
#define VE_ENC_AVC_BITSTREAM_OFFSET	0xb88
#define VE_ENC_AVC_BITSTREAM_LIMIT	0xb8c
#define VE_ENC_AVC_BITSTREAM_LENGTH	0xb90

#define VE_ENC_AVC_TRIGGER_SLICE		0x8

/* Low status bits are write-one-to-clear. */
#define VE_ENC_AVC_STATUS_DONE		BIT(0)
#define VE_ENC_AVC_STATUS_SIZE_ERROR	BIT(1)
#define VE_ENC_AVC_STATUS_MB_ERROR	BIT(2)

#endif /* _CEDRUS_ENC_REGS_H_ */

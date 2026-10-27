/* SPDX-License-Identifier: GPL-2.0 */
/* A133 H.264 encoder DMA and MMIO helpers. */

#ifndef _CEDRUS_H264_ENC_HW_H_
#define _CEDRUS_H264_ENC_HW_H_

#include <linux/io.h>
#include <linux/types.h>

#include "cedrus_enc.h"

struct cedrus_h264_enc_params;

/* Packed words established for the stock fixed-QP High/CABAC path. */
struct cedrus_a133_h264_enc_codec {
	u32 picture_size;
	u32 picture_control;
	u32 qp_control;
	u32 motion_control;
};

/* DMA register image for the independently observed A133 Ver2 path. */
struct cedrus_a133_h264_enc_dma {
	struct cedrus_a133_enc_source source;
	struct cedrus_a133_enc_bitstream bitstream;
	struct cedrus_a133_h264_enc_frame_slot reference;
	struct cedrus_a133_h264_enc_frame_slot reconstruction;
	u32 motion_vectors;
	u32 macroblock_info;
	u32 work_9c;
};

/* Two-slot reference and frame sequence state, committed on completion. */
struct cedrus_a133_h264_enc_rotation {
	u32 frames_since_idr;
	u8 reference;
	u8 pending_reference;
	u8 next_frame_num;
	bool started;
	bool pending;
	bool pending_keyframe;
};

#define CEDRUS_A133_H264_VLC_MAX_COMMANDS	64

/* One stock PUT_BITS transaction: b20 payload followed by b18 command. */
struct cedrus_a133_h264_vlc_command {
	u32 value;
	u8 bits;
};

struct cedrus_a133_h264_vlc_sequence {
	struct cedrus_a133_h264_vlc_command
		commands[CEDRUS_A133_H264_VLC_MAX_COMMANDS];
	u16 count;
	u16 bit_count;
};

struct cedrus_a133_h264_slice_header {
	u8 frame_num;
	u8 pic_order_cnt_lsb;
	u8 qp;
	bool idr;
};

int cedrus_a133_h264_codec_prepare(u32 width, u32 height, unsigned int qp,
				   bool idr,
				   struct cedrus_a133_h264_enc_codec *codec);

bool cedrus_a133_h264_frame_is_idr(const struct cedrus_a133_h264_enc_rotation *rotation,
				   u32 gop_size, bool force_keyframe);
int cedrus_a133_h264_frame_prepare(struct cedrus_a133_h264_enc_rotation *rotation,
				   const struct cedrus_a133_h264_enc_frame_slot slots[2], bool idr,
				   struct cedrus_a133_h264_enc_frame_slot *reference,
				   struct cedrus_a133_h264_enc_frame_slot *reconstruction);
int cedrus_a133_h264_frame_complete(struct cedrus_a133_h264_enc_rotation *rotation,
				    bool success);

int cedrus_a133_h264_dma_prepare(const struct cedrus_a133_enc_source *source,
				 const struct cedrus_a133_enc_bitstream *bitstream,
				 const struct cedrus_a133_h264_enc_frame_slot *reference,
				 const struct cedrus_a133_h264_enc_frame_slot *reconstruction,
				 dma_addr_t motion_vectors,
				 dma_addr_t macroblock_info,
				 dma_addr_t work_9c,
				 const struct cedrus_a133_h264_enc_buffer_layout *layout,
				 struct cedrus_a133_h264_enc_dma *dma);

void cedrus_a133_h264_program_source(void __iomem *base,
				     const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_bitstream(void __iomem *base,
					const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_frames(void __iomem *base,
				     const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_aux(void __iomem *base,
				  const struct cedrus_a133_h264_enc_dma *dma);
void cedrus_a133_h264_program_codec(void __iomem *base,
				    const struct cedrus_a133_h264_enc_codec *codec);

int cedrus_a133_h264_put_bits(void __iomem *base, u32 value,
			      unsigned int count);
int cedrus_a133_h264_put_ue(void __iomem *base, u32 value);
int cedrus_a133_h264_put_se(void __iomem *base, s32 value);

int cedrus_a133_h264_headers_prepare(const struct cedrus_h264_enc_params *params,
				     u32 width, u32 height,
				     struct cedrus_a133_h264_vlc_sequence *sequence);
int cedrus_a133_h264_slice_header_prepare(const struct cedrus_h264_enc_params *params,
					  const struct cedrus_a133_h264_slice_header *slice,
					  struct cedrus_a133_h264_vlc_sequence *sequence);
int cedrus_a133_h264_vlc_emit(void __iomem *base,
			      const struct cedrus_a133_h264_vlc_sequence *sequence);
int cedrus_a133_h264_program_job(void __iomem *base,
				 const struct cedrus_a133_h264_enc_dma *dma,
				 const struct cedrus_a133_h264_enc_codec *codec,
				 const struct cedrus_a133_h264_vlc_sequence *parameter_sets,
				 const struct cedrus_a133_h264_vlc_sequence *slice_header,
				 bool emit_parameter_sets);

void cedrus_a133_h264_irq_arm(void __iomem *base);
u32 cedrus_a133_h264_irq_read(void __iomem *base);
u32 cedrus_a133_h264_bitstream_length(void __iomem *base);
void cedrus_a133_h264_irq_ack_status(void __iomem *base, u32 status);
u32 cedrus_a133_h264_irq_ack(void __iomem *base);
void cedrus_a133_h264_trigger(void __iomem *base);

#endif /* _CEDRUS_H264_ENC_HW_H_ */

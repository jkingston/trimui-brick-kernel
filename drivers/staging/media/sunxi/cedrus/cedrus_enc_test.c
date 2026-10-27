// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/videodev2.h>

#include "cedrus_enc.h"
#include "cedrus_enc_regs.h"
#include "cedrus_h264_enc.h"
#include "cedrus_h264_enc_hw.h"
#include "cedrus_h264_enc_job.h"

static const struct cedrus_enc_format cedrus_enc_test_formats[] = {
	{
		.pixelformat = V4L2_PIX_FMT_NV12,
		.direction = CEDRUS_ENC_FORMAT_RAW,
		.min_width = 16, .max_width = 4096,
		.min_height = 16, .max_height = 4096,
		.width_align = 16, .height_align = 16,
		.stride_align = 16, .buffer_align = 1,
	},
	{
		.pixelformat = V4L2_PIX_FMT_H264,
		.direction = CEDRUS_ENC_FORMAT_CODED,
		.min_width = 16, .max_width = 4096,
		.min_height = 16, .max_height = 4096,
		.width_align = 16, .height_align = 16,
		.buffer_align = 256, .min_sizeimage = 256,
	},
};

static const struct v4l2_ctrl_config cedrus_enc_test_controls[] = {
	{ .id = V4L2_CID_MPEG_VIDEO_BITRATE },
};

static bool cedrus_enc_test_pair(u32 raw_pixelformat, u32 coded_pixelformat)
{
	return raw_pixelformat == V4L2_PIX_FMT_NV12 &&
	       coded_pixelformat == V4L2_PIX_FMT_H264;
}

static int cedrus_enc_test_s_ctrl(struct cedrus_ctx *ctx, struct v4l2_ctrl *ctrl)
{
	return 0;
}

static int cedrus_enc_test_start(struct cedrus_ctx *ctx)
{
	return 0;
}

static void cedrus_enc_test_stop(struct cedrus_ctx *ctx)
{
}

static int cedrus_enc_test_prepare(struct cedrus_ctx *ctx)
{
	return 0;
}

static void cedrus_enc_test_trigger(struct cedrus_ctx *ctx)
{
}

static enum cedrus_enc_irq_status cedrus_enc_test_status(struct cedrus_ctx *ctx)
{
	return CEDRUS_ENC_IRQ_NONE;
}

static int cedrus_enc_test_abort(struct cedrus_ctx *ctx)
{
	return 0;
}

static void cedrus_enc_test_fault(struct cedrus_ctx *ctx)
{
}

static int cedrus_enc_test_suspend(struct cedrus_dev *dev)
{
	return 0;
}

static int cedrus_enc_test_resume(struct cedrus_dev *dev)
{
	return 0;
}

static int cedrus_enc_test_source(u32 pixelformat, u32 bytesperline,
				  struct cedrus_a133_enc_source *source)
{
	return cedrus_a133_h264_enc_source_prepare(pixelformat, 320, 240,
			bytesperline, 0xfafa0000, 0xfaf90000, source);
}

static void cedrus_enc_test_backend_gate(struct kunit *test)
{
	struct cedrus_enc_backend_ops ops = {
		.contract = CEDRUS_A133_H264_ENC_CONTRACT,
		.formats = cedrus_enc_test_formats,
		.num_formats = ARRAY_SIZE(cedrus_enc_test_formats),
		.controls = cedrus_enc_test_controls,
		.num_controls = ARRAY_SIZE(cedrus_enc_test_controls),
		.s_ctrl = cedrus_enc_test_s_ctrl,
		.supports_pair = cedrus_enc_test_pair,
		.start_streaming = cedrus_enc_test_start,
		.stop_streaming = cedrus_enc_test_stop,
		.prepare_job = cedrus_enc_test_prepare,
		.trigger = cedrus_enc_test_trigger,
		.irq_status = cedrus_enc_test_status,
		.irq_ack = cedrus_enc_test_stop,
		.abort = cedrus_enc_test_abort,
		.iommu_fault = cedrus_enc_test_fault,
		.suspend = cedrus_enc_test_suspend,
		.resume = cedrus_enc_test_resume,
	};

	KUNIT_EXPECT_EQ(test, cedrus_enc_backend_validate(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, cedrus_enc_backend_validate(&ops), -EOPNOTSUPP);

	ops.contract = CEDRUS_ENC_CONTRACT_REQUIRED;
	KUNIT_EXPECT_EQ(test, cedrus_enc_backend_validate(&ops), 0);

	ops.abort = NULL;
	KUNIT_EXPECT_EQ(test, cedrus_enc_backend_validate(&ops), -EINVAL);
	ops.abort = cedrus_enc_test_abort;
	ops.formats = NULL;
	KUNIT_EXPECT_EQ(test, cedrus_enc_backend_validate(&ops), -EINVAL);
}

static void cedrus_enc_test_planar_source(struct kunit *test)
{
	struct cedrus_a133_enc_source source;
	int ret;

	ret = cedrus_enc_test_source(V4L2_PIX_FMT_YUV420, 320, &source);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, source.input_size, 0x0028001eU);
	KUNIT_EXPECT_EQ(test, source.stride, 0x00140000U);
	KUNIT_EXPECT_EQ(test, source.control, 0xc0000000U);
	KUNIT_EXPECT_EQ(test, source.mb_width, 0x14U);
	KUNIT_EXPECT_EQ(test, source.input_size_reversed, 0x001e0028U);
	KUNIT_EXPECT_EQ(test, source.luma, (dma_addr_t)0xfafa0000);
	KUNIT_EXPECT_EQ(test, source.chroma, (dma_addr_t)0xfaf90000);
	KUNIT_EXPECT_EQ(test, source.chroma2, (dma_addr_t)0xfaf94b00);

	ret = cedrus_enc_test_source(V4L2_PIX_FMT_YUV420, 336, &source);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, source.stride, 0x00150000U);
	KUNIT_EXPECT_EQ(test, source.mb_width, 0x15U);
	KUNIT_EXPECT_EQ(test, source.chroma2, (dma_addr_t)0xfaf94ec0);
}

static void cedrus_enc_test_nv12_source(struct kunit *test)
{
	struct cedrus_a133_enc_source source;
	int ret;

	ret = cedrus_enc_test_source(V4L2_PIX_FMT_NV12, 320, &source);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, source.control, 0U);
	KUNIT_EXPECT_EQ(test, source.chroma2, (dma_addr_t)0xfaf94b00);
}

static void cedrus_enc_test_source_rejects_unproven_encoding(struct kunit *test)
{
	struct cedrus_a133_enc_source source;
	int ret;

	ret = cedrus_enc_test_source(V4L2_PIX_FMT_NV21, 320, &source);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = cedrus_a133_h264_enc_source_prepare(V4L2_PIX_FMT_NV12, 322,
						  240, 336, 0x100000, 0x200000,
						  &source);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = cedrus_enc_test_source(V4L2_PIX_FMT_NV12, 304, &source);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = cedrus_a133_h264_enc_source_prepare(V4L2_PIX_FMT_NV12, 4104,
						  240, 4112, 0x100000, 0x200000,
						  &source);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	ret = cedrus_a133_h264_enc_source_prepare(V4L2_PIX_FMT_NV12, 320,
						  240, 320, 1ULL << 32, 0x200000,
						  &source);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

static void cedrus_enc_test_bitstream_window(struct kunit *test)
{
	struct cedrus_a133_enc_bitstream bs;
	int ret;

	ret = cedrus_a133_h264_enc_bitstream_prepare(0xfb000000,
						     12 * SZ_1M, &bs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, bs.start, 0x00fb0000U);
	KUNIT_EXPECT_EQ(test, bs.end, 0x00fbbfffU);
	KUNIT_EXPECT_EQ(test, bs.offset, 0U);
	KUNIT_EXPECT_EQ(test, bs.limit, 0x06000000U);

	ret = cedrus_a133_h264_enc_bitstream_prepare(0xfb000001, SZ_4K, &bs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = cedrus_a133_h264_enc_bitstream_prepare(0xfb000000,
						     SZ_4K + 1, &bs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void cedrus_enc_test_irq_contract(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_enc_irq_status(0x260),
			CEDRUS_ENC_IRQ_NONE);
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_enc_irq_status(0x261),
			CEDRUS_ENC_IRQ_OK);
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_enc_irq_status(0x262),
			CEDRUS_ENC_IRQ_ERROR);
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_enc_irq_status(0x265),
			CEDRUS_ENC_IRQ_ERROR);
}

static void cedrus_enc_test_qp_field(struct kunit *test)
{
	u32 updated;
	int ret;

	ret = cedrus_a133_h264_enc_qp_update(0x0800050a, 30, &updated);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, updated, 0x0800051eU);

	ret = cedrus_a133_h264_enc_qp_update(0x0800050a, 52, &updated);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void cedrus_enc_test_buffer_layout(struct kunit *test)
{
	struct cedrus_a133_h264_enc_buffer_layout layout;
	struct cedrus_a133_h264_enc_buffer_layout invalid_layout;
	struct cedrus_a133_h264_enc_frame_slot slot;
	int ret;

	ret = cedrus_a133_h264_enc_buffer_layout(320, 240, &layout);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, layout.reconstruction_luma, (size_t)81920);
	KUNIT_EXPECT_EQ(test, layout.reconstruction_chroma, (size_t)40960);
	KUNIT_EXPECT_EQ(test, layout.reconstruction, (size_t)122880);
	KUNIT_EXPECT_EQ(test, layout.subpixel, (size_t)3744);
	KUNIT_EXPECT_EQ(test, layout.motion_vectors, (size_t)2400);
	KUNIT_EXPECT_EQ(test, layout.macroblock_info, (size_t)2560);
	KUNIT_EXPECT_EQ(test, layout.deblock, (size_t)2560);
	KUNIT_EXPECT_EQ(test, layout.work_9c, (size_t)680);
	KUNIT_EXPECT_EQ(test, layout.work_frame, (size_t)4320);

	ret = cedrus_a133_h264_enc_frame_slot_prepare(0xfafe0000, 0xfafbf000,
						      &layout, &slot);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, slot.luma, 0x00fafe00U);
	KUNIT_EXPECT_EQ(test, slot.chroma, 0x00faff40U);
	KUNIT_EXPECT_EQ(test, slot.subpixel, 0x00fafbf0U);

	ret = cedrus_a133_h264_enc_frame_slot_prepare(0xfafe0001, 0xfafbf000,
						      &layout, &slot);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	if (sizeof(dma_addr_t) > sizeof(u32)) {
		dma_addr_t boundary = (dma_addr_t)((u64)U32_MAX << 8);

		ret = cedrus_a133_h264_enc_frame_slot_prepare(0xfafe0000,
							      boundary,
							      &layout, &slot);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	}

	invalid_layout = layout;
	invalid_layout.reconstruction_chroma--;
	ret = cedrus_a133_h264_enc_frame_slot_prepare(0xfafe0000, 0xfafbf000,
						      &invalid_layout, &slot);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = cedrus_a133_h264_enc_buffer_layout(4097, 240, &layout);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

static void cedrus_enc_test_dma_register_image(struct kunit *test)
{
	struct cedrus_a133_h264_enc_buffer_layout layout;
	struct cedrus_a133_h264_enc_dma dma;
	struct cedrus_a133_h264_enc_frame_slot reconstructed;
	struct cedrus_a133_h264_enc_frame_slot reference;
	struct cedrus_a133_enc_bitstream bitstream;
	struct cedrus_a133_enc_source source;
	int ret;

	ret = cedrus_enc_test_source(V4L2_PIX_FMT_NV12, 320, &source);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_a133_h264_enc_bitstream_prepare(0xfb000000,
						     12 * SZ_1M, &bitstream);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_a133_h264_enc_buffer_layout(320, 240, &layout);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_a133_h264_enc_frame_slot_prepare(0xfafe0000,
						      0xfafbf000,
						      &layout, &reference);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_a133_h264_enc_frame_slot_prepare(0xfafc0000,
						      0xfafbe000,
						      &layout, &reconstructed);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = cedrus_a133_h264_dma_prepare(&source, &bitstream,
					   &reference, &reconstructed,
					   0xfafbd000, 0xfafbc000,
					   &layout, &dma);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, dma.motion_vectors, 0x00fafbd0U);
	KUNIT_EXPECT_EQ(test, dma.macroblock_info, 0x00fafbc0U);
	KUNIT_EXPECT_EQ(test, dma.reference.luma, 0x00fafe00U);
	KUNIT_EXPECT_EQ(test, dma.reference.chroma, 0x00faff40U);
	KUNIT_EXPECT_EQ(test, dma.reference.subpixel, 0x00fafbf0U);
	KUNIT_EXPECT_EQ(test, dma.reconstruction.luma, 0x00fafc00U);
	KUNIT_EXPECT_EQ(test, dma.reconstruction.chroma, 0x00fafd40U);
	KUNIT_EXPECT_EQ(test, dma.reconstruction.subpixel, 0x00fafbe0U);

	ret = cedrus_a133_h264_dma_prepare(&source, &bitstream,
					   &reference, &reconstructed,
					   0xfafbd001, 0xfafbc000,
					   &layout, &dma);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	layout.motion_vectors = 0;
	ret = cedrus_a133_h264_dma_prepare(&source, &bitstream,
					   &reference, &reconstructed,
					   0xfafbd000, 0xfafbc000,
					   &layout, &dma);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void cedrus_enc_test_codec_register_image(struct kunit *test)
{
	struct cedrus_a133_h264_enc_codec codec;
	int ret;

	ret = cedrus_a133_h264_codec_prepare(320, 240, 30, true, &codec);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, codec.picture_size, 0x0028001eU);
	KUNIT_EXPECT_EQ(test, codec.picture_control, 0x00000500U);
	KUNIT_EXPECT_EQ(test,
			FIELD_GET(VE_ENC_AVC_PICTURE_CONTROL_FIX_MODE_NUM,
				  codec.picture_control),
			1U);
	KUNIT_EXPECT_EQ(test, codec.qp_control, 0x0800051eU);
	KUNIT_EXPECT_EQ(test, codec.motion_control, 0x00001200U);

	ret = cedrus_a133_h264_codec_prepare(320, 240, 30, false, &codec);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, codec.picture_control, 0x00000510U);
	ret = cedrus_a133_h264_codec_prepare(128, 128, 30, true, &codec);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, codec.picture_size, 0x00100010U);
	KUNIT_EXPECT_EQ(test, codec.qp_control, 0x0800041eU);

	/* 64 macroblocks require two stock 48-macroblock length units. */
	ret = cedrus_a133_h264_codec_prepare(1024, 240, 30, true, &codec);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, codec.picture_size, 0x0080001eU);
	KUNIT_EXPECT_EQ(test, codec.qp_control, 0x0800091eU);

	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_codec_prepare(320, 240, 52, true, &codec),
		-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_codec_prepare(321, 240, 30, true, &codec),
		-EOPNOTSUPP);
}

static void cedrus_enc_test_header_put_bits(struct kunit *test)
{
	void __iomem *base;
	u32 *registers;
	int ret;

	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;

	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY,
	       base + VE_ENC_AVC_STATUS);
	ret = cedrus_a133_h264_put_bits(base, 0x12345678, 32);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA),
			0x12345678U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER),
			0x00002001U);

	writel(0, base + VE_ENC_AVC_STATUS);
	writel(0, base + VE_ENC_AVC_PUT_BITS_DATA);
	writel(0, base + VE_ENC_AVC_TRIGGER);
	ret = cedrus_a133_h264_put_bits(base, 1, 1);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0U);

	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_put_bits(base, 0, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_put_bits(base, 0, 64),
			-EINVAL);
}

static void cedrus_enc_test_header_exp_golomb(struct kunit *test)
{
	void __iomem *base;
	u32 *registers;
	int ret;

	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;
	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY,
	       base + VE_ENC_AVC_STATUS);

	ret = cedrus_a133_h264_put_ue(base, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA), 1U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0x00000101U);

	ret = cedrus_a133_h264_put_ue(base, 65535);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA),
			0x00010000U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0x00002101U);

	ret = cedrus_a133_h264_put_se(base, 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA), 2U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0x00000301U);

	ret = cedrus_a133_h264_put_se(base, -1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA), 3U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0x00000301U);

	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_put_ue(base, U32_MAX),
			-EOVERFLOW);
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_put_se(base, S32_MIN),
			-EOVERFLOW);
}

static const struct cedrus_a133_h264_vlc_command stock_header_commands[] = {
	{ 0x00, 8 }, { 0x00, 8 }, { 0x00, 8 }, { 0x01, 8 },
	{ 0x67, 8 }, { 0x64, 8 }, { 0x00, 8 }, { 0x33, 8 },
	{ 0x01, 1 }, { 0x02, 3 }, { 0x01, 1 }, { 0x01, 1 },
	{ 0x00, 1 }, { 0x00, 1 }, { 0x05, 5 }, { 0x01, 1 },
	{ 0x05, 5 }, { 0x02, 3 }, { 0x00, 1 }, { 0x14, 9 },
	{ 0x0f, 7 }, { 0x01, 1 }, { 0x00, 1 }, { 0x00, 1 },
	{ 0x00, 1 }, { 0x01, 1 }, { 0x00, 4 }, { 0x00, 8 },
	{ 0x00, 8 }, { 0x00, 8 }, { 0x01, 8 }, { 0x68, 8 },
	{ 0x01, 1 }, { 0x01, 1 }, { 0x01, 1 }, { 0x00, 1 },
	{ 0x01, 1 }, { 0x01, 1 }, { 0x01, 1 }, { 0x00, 1 },
	{ 0x00, 2 }, { 0x01, 1 }, { 0x01, 1 }, { 0x01, 1 },
	{ 0x01, 1 }, { 0x00, 1 }, { 0x00, 1 }, { 0x00, 1 },
	{ 0x00, 1 }, { 0x01, 1 }, { 0x01, 1 }, { 0x00, 4 },
};

static const struct cedrus_a133_h264_vlc_command stock_idr_commands[] = {
	{ 0x00, 8 }, { 0x00, 8 }, { 0x00, 8 }, { 0x01, 8 },
	{ 0x65, 8 }, { 0x01, 1 }, { 0x08, 7 }, { 0x01, 1 },
	{ 0x00, 8 }, { 0x01, 1 }, { 0x00, 8 }, { 0x00, 1 },
	{ 0x00, 1 }, { 0x08, 7 }, { 0x01, 1 }, { 0x01, 1 },
	{ 0x01, 1 },
};

static const struct cedrus_a133_h264_vlc_command stock_first_p_commands[] = {
	{ 0x00, 8 }, { 0x00, 8 }, { 0x00, 8 }, { 0x01, 8 },
	{ 0x41, 8 }, { 0x01, 1 }, { 0x06, 5 }, { 0x01, 1 },
	{ 0x01, 8 }, { 0x02, 8 }, { 0x00, 1 }, { 0x00, 1 },
	{ 0x00, 1 }, { 0x02, 3 }, { 0x08, 7 }, { 0x01, 1 },
	{ 0x01, 1 }, { 0x01, 1 },
};

static void
cedrus_enc_test_vlc_commands(struct kunit *test,
			     const struct cedrus_a133_h264_vlc_sequence *sequence,
			     const struct cedrus_a133_h264_vlc_command *expected,
			     size_t expected_count)
{
	unsigned int i;

	KUNIT_ASSERT_EQ(test, (size_t)sequence->count, expected_count);
	for (i = 0; i < expected_count; i++) {
		KUNIT_EXPECT_EQ_MSG(test, sequence->commands[i].value,
				    expected[i].value, "command %u payload", i);
		KUNIT_EXPECT_EQ_MSG(test, sequence->commands[i].bits,
				    expected[i].bits, "command %u length", i);
	}
}

static size_t
cedrus_enc_test_vlc_render(const struct cedrus_a133_h264_vlc_sequence *sequence,
			   u8 *data, size_t size)
{
	size_t bit = 0;
	unsigned int i, remaining;

	memset(data, 0, size);
	if (DIV_ROUND_UP(sequence->bit_count, 8) > size)
		return 0;

	for (i = 0; i < sequence->count; i++) {
		for (remaining = sequence->commands[i].bits;
		     remaining; remaining--) {
			u8 value = remaining > 32 ? 0 :
				   (sequence->commands[i].value >>
				    (remaining - 1)) & 1;

			data[bit / 8] |= value << (7 - bit % 8);
			bit++;
		}
	}

	return DIV_ROUND_UP(bit, 8);
}

static void cedrus_enc_test_header_vlc_sequence(struct kunit *test)
{
	static const u8 stock_headers[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33,
		0xac, 0x2c, 0xa8, 0x14, 0x1f, 0x10,
		0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x30,
	};
	struct cedrus_h264_enc_params params = {
		.profile_idc = 100,
		.level_idc = 51,
		.qp = 30,
		.cabac = true,
		.gop_size = 30,
	};
	struct cedrus_a133_h264_vlc_sequence sequence;
	u8 rendered[64];
	int ret;

	ret = cedrus_a133_h264_headers_prepare(&params, 320, 240,
					       &sequence);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sequence.bit_count, (u16)176);
	cedrus_enc_test_vlc_commands(test, &sequence, stock_header_commands,
				     ARRAY_SIZE(stock_header_commands));
	KUNIT_ASSERT_EQ(test,
			cedrus_enc_test_vlc_render(&sequence, rendered,
						   sizeof(rendered)),
			sizeof(stock_headers));
	KUNIT_EXPECT_MEMEQ(test, rendered, stock_headers, sizeof(stock_headers));

	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_headers_prepare(&params, 0, 240,
							 &sequence),
			-EINVAL);
	params.profile_idc = 77;
	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_headers_prepare(&params, 320, 240,
							 &sequence),
			-EOPNOTSUPP);
}

static void cedrus_enc_test_slice_vlc_sequences(struct kunit *test)
{
	static const u8 stock_idr_prefix[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x40, 0x01, 0x1c,
	};
	static const u8 stock_p_prefix[] = {
		0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x02, 0x04, 0x10, 0x8e,
	};
	struct cedrus_h264_enc_params params = {
		.profile_idc = 100,
		.level_idc = 51,
		.qp = 30,
		.cabac = true,
		.gop_size = 30,
	};
	struct cedrus_a133_h264_slice_header slice = {
		.qp = 30,
		.idr = true,
	};
	struct cedrus_a133_h264_vlc_sequence sequence;
	u8 rendered[32];
	int ret;

	ret = cedrus_a133_h264_slice_header_prepare(&params, &slice,
						    &sequence);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sequence.bit_count, (u16)78);
	cedrus_enc_test_vlc_commands(test, &sequence, stock_idr_commands,
				     ARRAY_SIZE(stock_idr_commands));
	KUNIT_ASSERT_EQ(test,
			cedrus_enc_test_vlc_render(&sequence, rendered,
						   sizeof(rendered)),
			sizeof(stock_idr_prefix));
	KUNIT_EXPECT_MEMEQ(test, rendered, stock_idr_prefix,
			   sizeof(stock_idr_prefix));

	slice = (struct cedrus_a133_h264_slice_header) {
		.frame_num = 1,
		.pic_order_cnt_lsb = 2,
		.qp = 30,
	};
	ret = cedrus_a133_h264_slice_header_prepare(&params, &slice,
						    &sequence);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sequence.bit_count, (u16)79);
	cedrus_enc_test_vlc_commands(test, &sequence, stock_first_p_commands,
				     ARRAY_SIZE(stock_first_p_commands));
	KUNIT_ASSERT_EQ(test,
			cedrus_enc_test_vlc_render(&sequence, rendered,
						   sizeof(rendered)),
			sizeof(stock_p_prefix));
	KUNIT_EXPECT_MEMEQ(test, rendered, stock_p_prefix,
			   sizeof(stock_p_prefix));

	slice.qp = 52;
	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_slice_header_prepare(&params, &slice,
							      &sequence),
			-ERANGE);
}

static void cedrus_enc_test_vlc_emit_failure(struct kunit *test)
{
	struct cedrus_a133_h264_vlc_sequence sequence = {
		.commands = { { 0x67, 8 }, { 1, 1 } },
		.count = 2,
		.bit_count = 9,
	};
	void __iomem *base;
	u32 *registers;
	int ret;

	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;

	ret = cedrus_a133_h264_vlc_emit(base, &sequence);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0U);

	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY,
	       base + VE_ENC_AVC_STATUS);
	ret = cedrus_a133_h264_vlc_emit(base, &sequence);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA), 1U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER), 0x00000101U);

	sequence.commands[0].bits = 0;
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_vlc_emit(base, &sequence),
			-EINVAL);
	sequence.commands[0].bits = 8;
	sequence.bit_count = 8;
	KUNIT_EXPECT_EQ(test, cedrus_a133_h264_vlc_emit(base, &sequence),
			-EINVAL);
}

static void cedrus_enc_test_reference_rotation(struct kunit *test)
{
	const struct cedrus_a133_h264_enc_frame_slot slots[2] = {
		{ .luma = 0x00fafe00, .chroma = 0x00faff40,
		  .subpixel = 0x00fafbf0 },
		{ .luma = 0x00fafc00, .chroma = 0x00fafd40,
		  .subpixel = 0x00fafbe0 },
	};
	struct cedrus_a133_h264_enc_rotation rotation = {};
	struct cedrus_a133_h264_enc_frame_slot reference;
	struct cedrus_a133_h264_enc_frame_slot reconstruction;
	unsigned int frame;
	int ret;

	ret = cedrus_a133_h264_frame_prepare(&rotation, slots, true,
					     &reference, &reconstruction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reference.luma, slots[0].luma);
	KUNIT_EXPECT_EQ(test, reconstruction.luma, slots[0].luma);
	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_frame_prepare(&rotation, slots, true,
						       &reference, &reconstruction),
		-EBUSY);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_h264_frame_complete(&rotation, true), 0);

	/* Five captured P frames alternate 0->1, 1->0, 0->1, 1->0, 0->1. */
	for (frame = 1; frame < 6; frame++) {
		u8 old = (frame - 1) & 1;
		u8 next = frame & 1;

		ret = cedrus_a133_h264_frame_prepare(&rotation, slots, false,
						     &reference,
						     &reconstruction);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, reference.luma, slots[old].luma);
		KUNIT_EXPECT_EQ(test, reconstruction.luma, slots[next].luma);
		KUNIT_ASSERT_EQ(test,
				cedrus_a133_h264_frame_complete(&rotation, true), 0);
	}

	ret = cedrus_a133_h264_frame_prepare(&rotation, slots, false,
					     &reference, &reconstruction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_h264_frame_complete(&rotation, false), 0);
	KUNIT_EXPECT_EQ(test, rotation.reference, (u8)1);

	KUNIT_EXPECT_EQ(test,
			cedrus_a133_h264_frame_prepare(&rotation, slots, true,
						       &reference, &reconstruction),
		-EOPNOTSUPP);
}

static void cedrus_enc_test_standard_controls(struct kunit *test)
{
	struct cedrus_h264_enc_controls controls = {
		.profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.level = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
		.entropy_mode = V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC,
		.gop_size = 30,
		.i_qp = 30,
		.p_qp = 30,
	};
	struct cedrus_h264_enc_params params;

	KUNIT_ASSERT_EQ(test, cedrus_h264_enc_controls_prepare(&controls,
							       &params), 0);
	KUNIT_EXPECT_EQ(test, params.profile_idc, (u8)100);
	KUNIT_EXPECT_EQ(test, params.level_idc, (u8)51);
	KUNIT_EXPECT_TRUE(test, params.cabac);
	KUNIT_EXPECT_EQ(test, params.qp, (u8)30);
	KUNIT_EXPECT_EQ(test, params.gop_size, 30U);

	controls.profile = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
	KUNIT_EXPECT_EQ(test, cedrus_h264_enc_controls_prepare(&controls,
							       &params),
			-EOPNOTSUPP);
	controls.profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	controls.p_qp = 29;
	KUNIT_EXPECT_EQ(test, cedrus_h264_enc_controls_prepare(&controls,
							       &params),
			-EOPNOTSUPP);
	controls.p_qp = 30;
	controls.frame_rc_enable = true;
	KUNIT_EXPECT_EQ(test, cedrus_h264_enc_controls_prepare(&controls,
							       &params),
			-EOPNOTSUPP);
}

static void cedrus_enc_test_stock_headers(struct kunit *test)
{
	static const u8 expected[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33,
		0xac, 0x2c, 0xa8, 0x14, 0x1f, 0x10, 0x00, 0x00,
		0x00, 0x01, 0x68, 0xee, 0x3c, 0x30,
	};
	static const u8 cropped_expected[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33,
		0xac, 0x2c, 0xa8, 0x15, 0x08, 0x59, 0x65, 0x40,
		0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x30,
	};
	struct cedrus_h264_enc_params params = {
		.profile_idc = 100,
		.level_idc = 51,
		.qp = 30,
		.cabac = true,
		.gop_size = 30,
	};
	u8 header[64] = {};
	size_t bytes_used = 0;
	int ret;

	ret = cedrus_h264_enc_headers(&params, 320, 240, header,
				      sizeof(header), &bytes_used);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, bytes_used, sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, header, expected, sizeof(expected));

	memset(header, 0, sizeof(header));
	ret = cedrus_h264_enc_headers(&params, 328, 248, header,
				      sizeof(header), &bytes_used);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, bytes_used, sizeof(cropped_expected));
	KUNIT_EXPECT_MEMEQ(test, header, cropped_expected,
			   sizeof(cropped_expected));

	ret = cedrus_h264_enc_headers(&params, 3072, 3072, header,
				      sizeof(header), &bytes_used);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = cedrus_h264_enc_headers(&params, 3088, 3072, header,
				      sizeof(header), &bytes_used);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	ret = cedrus_h264_enc_headers(&params, 320, 240, header, 8,
				      &bytes_used);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	ret = cedrus_h264_enc_headers(&params, 321, 240, header,
				      sizeof(header), &bytes_used);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	ret = cedrus_h264_enc_headers(&params, 4096, 4096, header,
				      sizeof(header), &bytes_used);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

static void cedrus_enc_test_gop_policy(struct kunit *test)
{
	struct cedrus_h264_enc_gop gop = {};

	KUNIT_EXPECT_TRUE(test, cedrus_h264_enc_gop_next(&gop, 3, false));
	KUNIT_EXPECT_FALSE(test, cedrus_h264_enc_gop_next(&gop, 3, false));
	KUNIT_EXPECT_FALSE(test, cedrus_h264_enc_gop_next(&gop, 3, false));
	KUNIT_EXPECT_TRUE(test, cedrus_h264_enc_gop_next(&gop, 3, false));
	KUNIT_EXPECT_TRUE(test, cedrus_h264_enc_gop_next(&gop, 3, true));
	KUNIT_EXPECT_FALSE(test, cedrus_h264_enc_gop_next(&gop, 0, false));
}

static void cedrus_enc_test_job_plan(struct kunit *test)
{
	struct cedrus_h264_enc_params params = {
		.profile_idc = 100,
		.level_idc = 51,
		.qp = 30,
		.cabac = true,
		.gop_size = 30,
	};
	struct cedrus_a133_h264_job_request request = {
		.pixelformat = V4L2_PIX_FMT_NV12,
		.width = 320,
		.height = 240,
		.bytesperline = 320,
		.slice = { .qp = 30, .idr = true },
	};
	struct cedrus_a133_h264_job_buffers buffers = {
		.source = 0x20000000,
		.source_size = 320 * 240 * 3 / 2,
		.bitstream = 0x21000000,
		.bitstream_size = SZ_1M,
		.reconstruction = { 0x22000000, 0x22020000 },
		.subpixel = { 0x22040000, 0x22041000 },
		.motion_vectors = 0x22042000,
		.macroblock_info = 0x22043000,
	};
	struct cedrus_a133_h264_enc_rotation rotation = {};
	struct cedrus_a133_h264_job_plan plan;
	int ret;

	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, plan.emit_parameter_sets);
	KUNIT_EXPECT_EQ(test, plan.parameter_sets.bit_count, (u16)176);
	KUNIT_EXPECT_EQ(test, plan.slice_header.bit_count, (u16)78);
	KUNIT_EXPECT_EQ(test, plan.dma.source.luma, (dma_addr_t)0x20000000);
	KUNIT_EXPECT_EQ(test, plan.dma.source.chroma,
			(dma_addr_t)0x20012c00);
	KUNIT_EXPECT_EQ(test, plan.dma.bitstream.start, 0x00210000U);
	KUNIT_EXPECT_EQ(test, plan.codec.picture_size, 0x0028001eU);
	KUNIT_EXPECT_EQ(test, plan.codec.picture_control, 0x00000500U);
	KUNIT_EXPECT_EQ(test, plan.codec.qp_control, 0x0800051eU);
	KUNIT_EXPECT_EQ(test, plan.dma.reference.luma, 0x00220000U);
	KUNIT_EXPECT_EQ(test, plan.dma.reconstruction.luma, 0x00220000U);
	KUNIT_EXPECT_TRUE(test, rotation.pending);
	KUNIT_EXPECT_FALSE(test, rotation.started);

	KUNIT_ASSERT_EQ(test,
			cedrus_a133_h264_frame_complete(&rotation, true), 0);
	KUNIT_EXPECT_EQ(test, rotation.next_frame_num, (u8)1);
	request.slice = (struct cedrus_a133_h264_slice_header) {
		.frame_num = 1,
		.pic_order_cnt_lsb = 2,
		.qp = 30,
	};
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, plan.emit_parameter_sets);
	KUNIT_EXPECT_EQ(test, plan.parameter_sets.count, (u16)0);
	KUNIT_EXPECT_EQ(test, plan.slice_header.bit_count, (u16)79);
	KUNIT_EXPECT_EQ(test, plan.codec.picture_control, 0x00000510U);
	KUNIT_EXPECT_EQ(test, plan.dma.reference.luma, 0x00220000U);
	KUNIT_EXPECT_EQ(test, plan.dma.reconstruction.luma, 0x00220200U);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_h264_frame_complete(&rotation, false), 0);
	KUNIT_EXPECT_EQ(test, rotation.reference, (u8)0);
	KUNIT_EXPECT_EQ(test, rotation.next_frame_num, (u8)1);

	/* A failed P frame is retried with the same frame_num and POC. */
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_h264_frame_complete(&rotation, true), 0);
	KUNIT_EXPECT_EQ(test, rotation.next_frame_num, (u8)2);
	request.slice.frame_num = 2;
	request.slice.pic_order_cnt_lsb = 4;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_h264_frame_complete(&rotation, false), 0);
}

static void cedrus_enc_test_job_plan_fail_closed(struct kunit *test)
{
	struct cedrus_h264_enc_params params = {
		.profile_idc = 100,
		.level_idc = 51,
		.qp = 30,
		.cabac = true,
		.gop_size = 30,
	};
	struct cedrus_a133_h264_job_request request = {
		.pixelformat = V4L2_PIX_FMT_YUV420,
		.width = 320,
		.height = 240,
		.bytesperline = 320,
		.slice = { .qp = 30, .idr = true },
	};
	struct cedrus_a133_h264_job_buffers buffers = {
		.source = 0x20000000,
		.source_size = 320 * 240 * 3 / 2,
		.bitstream = 0x21000000,
		.bitstream_size = SZ_1M,
		.reconstruction = { 0x22000000, 0x22020000 },
		.subpixel = { 0x22040000, 0x22041000 },
		.motion_vectors = 0x22042000,
		.macroblock_info = 0x22043000,
	};
	const struct cedrus_a133_h264_enc_rotation initial = {};
	struct cedrus_a133_h264_enc_rotation rotation = initial;
	struct cedrus_a133_h264_job_plan plan = {};
	int ret;

	request.height = 0;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &rotation, &initial, sizeof(rotation));

	request.height = 240;
	buffers.source_size--;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	KUNIT_EXPECT_MEMEQ(test, &rotation, &initial, sizeof(rotation));

	buffers.source_size++;
	request.slice.qp = 29;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &rotation, &initial, sizeof(rotation));

	request.slice.qp = 30;
	buffers.macroblock_info = buffers.motion_vectors;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &rotation, &initial, sizeof(rotation));

	buffers.macroblock_info = 0x22043000;
	buffers.source = 0xffff0000;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	KUNIT_EXPECT_MEMEQ(test, &rotation, &initial, sizeof(rotation));

	buffers.source = 0x20000000;
	rotation.started = true;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_TRUE(test, rotation.started);
	KUNIT_EXPECT_FALSE(test, rotation.pending);

	request.slice.idr = false;
	request.slice.frame_num = 2;
	request.slice.pic_order_cnt_lsb = 4;
	rotation.next_frame_num = 1;
	ret = cedrus_a133_h264_job_plan_prepare(&rotation, &params, &request,
						&buffers, &plan);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, rotation.pending);
}

static void cedrus_enc_test_frontend_contract(struct kunit *test)
{
	struct cedrus_enc_format format = cedrus_enc_test_formats[0];
	bool standard;

	KUNIT_EXPECT_EQ(test, cedrus_enc_format_validate(&format), 0);
	standard = cedrus_enc_control_is_standard(V4L2_CID_MPEG_VIDEO_BITRATE);
	KUNIT_EXPECT_TRUE(test, standard);
	KUNIT_EXPECT_FALSE(test, cedrus_enc_control_is_standard(V4L2_CID_PRIVATE_BASE));

	format.stride_align = 3;
	KUNIT_EXPECT_EQ(test, cedrus_enc_format_validate(&format), -EINVAL);
	format = cedrus_enc_test_formats[1];
	format.stride_align = 16;
	KUNIT_EXPECT_EQ(test, cedrus_enc_format_validate(&format), -EINVAL);
}

static struct kunit_case cedrus_enc_test_cases[] = {
	KUNIT_CASE(cedrus_enc_test_frontend_contract),
	KUNIT_CASE(cedrus_enc_test_backend_gate),
	KUNIT_CASE(cedrus_enc_test_planar_source),
	KUNIT_CASE(cedrus_enc_test_nv12_source),
	KUNIT_CASE(cedrus_enc_test_source_rejects_unproven_encoding),
	KUNIT_CASE(cedrus_enc_test_bitstream_window),
	KUNIT_CASE(cedrus_enc_test_irq_contract),
	KUNIT_CASE(cedrus_enc_test_qp_field),
	KUNIT_CASE(cedrus_enc_test_buffer_layout),
	KUNIT_CASE(cedrus_enc_test_dma_register_image),
	KUNIT_CASE(cedrus_enc_test_codec_register_image),
	KUNIT_CASE(cedrus_enc_test_header_put_bits),
	KUNIT_CASE(cedrus_enc_test_header_exp_golomb),
	KUNIT_CASE(cedrus_enc_test_header_vlc_sequence),
	KUNIT_CASE(cedrus_enc_test_slice_vlc_sequences),
	KUNIT_CASE(cedrus_enc_test_vlc_emit_failure),
	KUNIT_CASE(cedrus_enc_test_reference_rotation),
	KUNIT_CASE(cedrus_enc_test_standard_controls),
	KUNIT_CASE(cedrus_enc_test_stock_headers),
	KUNIT_CASE(cedrus_enc_test_gop_policy),
	KUNIT_CASE(cedrus_enc_test_job_plan),
	KUNIT_CASE(cedrus_enc_test_job_plan_fail_closed),
	{}
};

static struct kunit_suite cedrus_enc_test_suite = {
	.name = "cedrus-encoder-contract",
	.test_cases = cedrus_enc_test_cases,
};

kunit_test_suite(cedrus_enc_test_suite);

MODULE_LICENSE("GPL");

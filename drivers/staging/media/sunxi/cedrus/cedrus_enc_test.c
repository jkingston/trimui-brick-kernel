// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/videodev2.h>

#include "cedrus_enc.h"

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
	{}
};

static struct kunit_suite cedrus_enc_test_suite = {
	.name = "cedrus-encoder-contract",
	.test_cases = cedrus_enc_test_cases,
};

kunit_test_suite(cedrus_enc_test_suite);

MODULE_LICENSE("GPL");

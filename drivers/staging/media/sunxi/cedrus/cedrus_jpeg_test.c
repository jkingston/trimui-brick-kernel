// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/sizes.h>
#include <linux/videodev2.h>

#include <media/v4l2-jpeg.h>

#include "cedrus_jpeg.h"
#include "cedrus_enc_regs.h"
#include "cedrus_jpeg_enc_hw.h"
#include "cedrus_jpeg_enc_job.h"

static const u8 stock_quality_95_luma_zigzag[CEDRUS_JPEG_QUANT_SIZE] = {
	2, 1, 1, 1, 1, 1, 2, 1, 1, 1, 2, 2, 2, 2, 2, 4,
	3, 2, 2, 2, 2, 5, 4, 4, 3, 4, 6, 5, 6, 6, 6, 5,
	6, 6, 6, 7, 9, 8, 6, 7, 9, 7, 6, 6, 8, 11, 8, 9,
	10, 10, 10, 10, 10, 6, 8, 11, 12, 11, 10, 12, 9, 10, 10, 10,
};

static const u8 stock_quality_95_chroma_zigzag[CEDRUS_JPEG_QUANT_SIZE] = {
	2, 2, 2, 2, 2, 2, 5, 3, 3, 5, 10, 7, 6, 7, 10, 10,
	10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
};

static void cedrus_jpeg_tables_match_stock(struct kunit *test)
{
	struct cedrus_jpeg_tables tables;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 95), 0);

	for (i = 0; i < CEDRUS_JPEG_QUANT_SIZE; i++) {
		unsigned int index = v4l2_jpeg_zigzag_scan_index[i];

		KUNIT_EXPECT_EQ(test, tables.luma[index],
				stock_quality_95_luma_zigzag[i]);
		KUNIT_EXPECT_EQ(test, tables.chroma[index],
				stock_quality_95_chroma_zigzag[i]);
	}

	/* Exact A133 SRAM words for q=2, q=1 and q=5. */
	KUNIT_EXPECT_EQ(test, tables.hw[0], 0x00018001U);
	KUNIT_EXPECT_EQ(test, tables.hw[1], 0x0000ffffU);
	KUNIT_EXPECT_EQ(test,
			tables.hw[CEDRUS_JPEG_QUANT_SIZE + 3], 0x00023334U);
}

static void cedrus_jpeg_quality_bounds(struct kunit *test)
{
	struct cedrus_jpeg_tables tables;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 101), -EINVAL);

	KUNIT_ASSERT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 25), 0);
	KUNIT_EXPECT_EQ(test, tables.luma[0], (u8)32);
	KUNIT_EXPECT_EQ(test, tables.hw[0], 0x00100801U);
	KUNIT_ASSERT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 50), 0);
	KUNIT_EXPECT_EQ(test, tables.luma[0], (u8)16);
	KUNIT_EXPECT_EQ(test, tables.hw[0], 0x00081001U);

	KUNIT_ASSERT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 100), 0);

	for (i = 0; i < CEDRUS_JPEG_QUANT_SIZE; i++) {
		KUNIT_EXPECT_EQ(test, tables.luma[i], 1);
		KUNIT_EXPECT_EQ(test, tables.chroma[i], 1);
		KUNIT_EXPECT_EQ(test, tables.hw[i], 0x0000ffffU);
		KUNIT_EXPECT_EQ(test,
				tables.hw[CEDRUS_JPEG_QUANT_SIZE + i],
				0x0000ffffU);
	}
}

static void cedrus_jpeg_header_is_standard_baseline(struct kunit *test)
{
	static const u8 stock_plain_jpeg_prefix[] = {
		0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10,
		'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00,
		0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
	};
	struct v4l2_jpeg_reference quantization_tables[V4L2_JPEG_MAX_TABLES] = {};
	struct v4l2_jpeg_reference huffman_tables[V4L2_JPEG_MAX_TABLES] = {};
	struct v4l2_jpeg_scan_header scan = {};
	struct cedrus_jpeg_tables tables;
	struct v4l2_jpeg_header parsed = {
		.scan = &scan,
		.quantization_tables = quantization_tables,
		.huffman_tables = huffman_tables,
	};
	u8 *header;
	int ret;

	header = kunit_kzalloc(test, CEDRUS_JPEG_HEADER_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, header);
	KUNIT_ASSERT_EQ(test, cedrus_jpeg_prepare_tables(&tables, 95), 0);
	KUNIT_ASSERT_EQ(test,
			cedrus_jpeg_write_header(header, CEDRUS_JPEG_HEADER_SIZE,
						 320, 240, &tables),
			(int)CEDRUS_JPEG_HEADER_SIZE);
	KUNIT_EXPECT_MEMEQ(test, header, stock_plain_jpeg_prefix,
			   sizeof(stock_plain_jpeg_prefix));

	ret = v4l2_jpeg_parse_header(header, CEDRUS_JPEG_HEADER_SIZE, &parsed);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, parsed.frame.width, (u16)320);
	KUNIT_EXPECT_EQ(test, parsed.frame.height, (u16)240);
	KUNIT_EXPECT_EQ(test, parsed.frame.precision, (u8)8);
	KUNIT_EXPECT_EQ(test, parsed.frame.num_components, (u8)3);
	KUNIT_EXPECT_EQ(test, parsed.frame.subsampling,
			V4L2_JPEG_CHROMA_SUBSAMPLING_420);
	KUNIT_EXPECT_EQ(test, parsed.num_dqt, 2U);
	KUNIT_EXPECT_EQ(test, parsed.num_dht, 4U);
	KUNIT_EXPECT_EQ(test, parsed.ecs_offset,
			(size_t)CEDRUS_JPEG_HEADER_SIZE);
	KUNIT_EXPECT_MEMEQ(test, parsed.quantization_tables[0].start,
			   stock_quality_95_luma_zigzag,
			   CEDRUS_JPEG_QUANT_SIZE);
	KUNIT_EXPECT_MEMEQ(test, parsed.quantization_tables[1].start,
			   stock_quality_95_chroma_zigzag,
			   CEDRUS_JPEG_QUANT_SIZE);
}

static int cedrus_jpeg_test_prepare_source(struct cedrus_jpeg_source *source,
					   u32 pixelformat,
					   unsigned int width,
					   unsigned int stride,
					   size_t sizeimage)
{
	return cedrus_jpeg_prepare_source(source, pixelformat, width, 240,
					  stride, sizeimage, 0xfbbe0000);
}

static int prepare_capture(struct cedrus_jpeg_capture *capture,
			   dma_addr_t address, size_t capacity)
{
	return cedrus_jpeg_prepare_capture(capture, address, capacity,
					   CEDRUS_JPEG_HEADER_SIZE);
}

static int finish_capture(void *buffer,
			  const struct cedrus_jpeg_capture *capture,
			  u32 status, u32 produced_bits, size_t *bytesused)
{
	return cedrus_jpeg_finish_capture(buffer, capture, status,
					  produced_bits, bytesused);
}

static void cedrus_jpeg_nv12_source_contract(struct kunit *test)
{
	struct cedrus_jpeg_source source;
	int ret;

	ret = cedrus_jpeg_test_prepare_source(&source, V4L2_PIX_FMT_NV12,
					      320, 320, 320 * 240 * 3 / 2);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, source.luma, (dma_addr_t)0xfbbe0000);
	KUNIT_EXPECT_EQ(test, source.chroma, (dma_addr_t)0xfbbf2c00);
	KUNIT_EXPECT_EQ(test, source.luma_size, (size_t)76800);
	KUNIT_EXPECT_EQ(test, source.chroma_size, (size_t)38400);
	KUNIT_EXPECT_EQ(test, source.stride, 320U);

	ret = cedrus_jpeg_test_prepare_source(&source, V4L2_PIX_FMT_YUV420,
					      320, 320, 320 * 240 * 3 / 2);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = cedrus_jpeg_test_prepare_source(&source, V4L2_PIX_FMT_NV12,
					      320, 336, 336 * 240 * 3 / 2);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = cedrus_jpeg_test_prepare_source(&source, V4L2_PIX_FMT_NV12,
					      318, 318, 318 * 240 * 3 / 2);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = cedrus_jpeg_test_prepare_source(&source, V4L2_PIX_FMT_NV12,
					      320, 320, 320 * 240);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	ret = cedrus_jpeg_prepare_source(&source, V4L2_PIX_FMT_NV12,
					 320, 240, 320, 320 * 240 * 3 / 2,
					 0xfbbe0001);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = cedrus_jpeg_prepare_source(&source, V4L2_PIX_FMT_NV12,
					 320, 240, 320, 320 * 240 * 3 / 2,
					 0xffff0000);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	ret = cedrus_jpeg_prepare_source(&source, V4L2_PIX_FMT_NV12,
					 320, 240, 320, 320 * 240 * 3 / 2,
					 0x100000000ULL);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

static void cedrus_jpeg_capture_contract(struct kunit *test)
{
	struct cedrus_jpeg_capture capture;
	int ret;

	ret = prepare_capture(&capture, 0xfbc00000, SZ_4M);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, capture.start, 0x00fbc000U);
	KUNIT_EXPECT_EQ(test, capture.end, 0x00ffffffU);
	KUNIT_EXPECT_EQ(test, capture.offset,
			CEDRUS_JPEG_HEADER_SIZE * 8U);
	KUNIT_EXPECT_EQ(test, capture.limit, 0x02000000U);

	ret = prepare_capture(&capture, 0xfbc00001, SZ_4M);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = prepare_capture(&capture, 0xfbc00000, SZ_4M - 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = cedrus_jpeg_prepare_capture(&capture, 0xfbc00000, 512, 511);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	ret = prepare_capture(&capture, 0xffff0000, SZ_1M);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	ret = prepare_capture(&capture, 0x10000000,
			      CEDRUS_JPEG_BITSTREAM_MAX_BITS / 8 + 256);
	KUNIT_EXPECT_EQ(test, ret, -E2BIG);
}

static void cedrus_jpeg_capture_completion(struct kunit *test)
{
	struct cedrus_jpeg_capture capture;
	u8 *buffer;
	size_t bytesused = 0;
	u32 status;
	int ret;

	buffer = kunit_kzalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buffer);
	ret = prepare_capture(&capture, 0x10000000, 4096);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = finish_capture(buffer, &capture, CEDRUS_JPEG_STATUS_FINISH,
			     (CEDRUS_JPEG_HEADER_SIZE + 100) * 8, &bytesused);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, bytesused,
			(size_t)CEDRUS_JPEG_HEADER_SIZE + 102);
	KUNIT_EXPECT_EQ(test, buffer[CEDRUS_JPEG_HEADER_SIZE + 100],
			(u8)0xff);
	KUNIT_EXPECT_EQ(test, buffer[CEDRUS_JPEG_HEADER_SIZE + 101],
			(u8)0xd9);

	status = CEDRUS_JPEG_STATUS_FINISH | CEDRUS_JPEG_STATUS_BUFFER_FULL;
	ret = finish_capture(buffer, &capture, status,
			     (CEDRUS_JPEG_HEADER_SIZE + 100) * 8, &bytesused);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	status = CEDRUS_JPEG_STATUS_FINISH | CEDRUS_JPEG_STATUS_MB_OVERFLOW;
	ret = finish_capture(buffer, &capture, status,
			     (CEDRUS_JPEG_HEADER_SIZE + 100) * 8, &bytesused);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	ret = finish_capture(buffer, &capture, 0,
			     (CEDRUS_JPEG_HEADER_SIZE + 100) * 8, &bytesused);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	ret = finish_capture(buffer, &capture, CEDRUS_JPEG_STATUS_FINISH,
			     CEDRUS_JPEG_HEADER_SIZE * 8 + 1, &bytesused);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	ret = finish_capture(buffer, &capture, CEDRUS_JPEG_STATUS_FINISH,
			     (CEDRUS_JPEG_HEADER_SIZE - 1) * 8, &bytesused);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	ret = finish_capture(buffer, &capture, CEDRUS_JPEG_STATUS_FINISH,
			     (capture.capacity - 1) * 8, &bytesused);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
}

static void cedrus_jpeg_a133_command_profile(struct kunit *test)
{
	struct cedrus_a133_jpeg_codec codec;
	struct cedrus_jpeg_capture capture;
	struct cedrus_jpeg_tables tables;
	u8 *header;
	u32 *registers;
	void __iomem *base;
	int ret;

	header = kunit_kzalloc(test, CEDRUS_JPEG_HEADER_SIZE, GFP_KERNEL);
	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, header);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;

	ret = cedrus_jpeg_prepare_tables(&tables, 95);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_jpeg_write_header(header, CEDRUS_JPEG_HEADER_SIZE,
				       320, 240, &tables);
	KUNIT_ASSERT_EQ(test, ret, (int)CEDRUS_JPEG_HEADER_SIZE);
	ret = cedrus_a133_jpeg_codec_prepare(320, 240, 95, &tables, &codec);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, codec.picture_control, 0xc2000200U);
	KUNIT_EXPECT_EQ(test, codec.optimization, 0xc0000000U);

	ret = cedrus_jpeg_prepare_capture(&capture, 0xfbc00000, SZ_4M, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY,
	       base + VE_ENC_AVC_STATUS);
	cedrus_a133_jpeg_select(base);
	ret = cedrus_a133_jpeg_prepare_phase(base, &codec, &capture, &tables,
					     header, CEDRUS_JPEG_HEADER_SIZE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	cedrus_a133_jpeg_trigger(base);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PICTURE_SIZE), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PICTURE_CONTROL),
			codec.picture_control);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_QP),
			codec.optimization);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_BITSTREAM_START),
			capture.start);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_BITSTREAM_END),
			capture.end);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER),
			0x00010008U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_PUT_BITS_DATA),
			(u32)header[CEDRUS_JPEG_HEADER_SIZE - 1]);

	ret = cedrus_a133_jpeg_codec_prepare(320, 240, 94, &tables, &codec);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	capture.prefix = CEDRUS_JPEG_HEADER_SIZE;
	capture.offset = CEDRUS_JPEG_HEADER_SIZE * 8U;
	cedrus_a133_jpeg_select(base);
	ret = cedrus_a133_jpeg_prepare_phase(base, &codec, &capture, &tables,
					     header, CEDRUS_JPEG_HEADER_SIZE);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
}

static void cedrus_jpeg_a133_command_timeout(struct kunit *test)
{
	struct cedrus_a133_jpeg_codec codec;
	struct cedrus_jpeg_capture capture;
	struct cedrus_jpeg_tables tables;
	u8 *header;
	u32 *registers;
	void __iomem *base;
	int ret;

	header = kunit_kzalloc(test, CEDRUS_JPEG_HEADER_SIZE, GFP_KERNEL);
	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, header);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;
	ret = cedrus_jpeg_prepare_tables(&tables, 95);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_a133_jpeg_codec_prepare(320, 240, 95, &tables, &codec);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = cedrus_jpeg_prepare_capture(&capture, 0xfbc00000, SZ_4M, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	cedrus_a133_jpeg_select(base);
	ret = cedrus_a133_jpeg_prepare_phase(base, &codec, &capture, &tables,
					     header, CEDRUS_JPEG_HEADER_SIZE);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER),
			VE_ENC_JPEG_TRIGGER_PUT_BYTE);
}

static void cedrus_jpeg_a133_plain_isp_contract(struct kunit *test)
{
	struct cedrus_a133_jpeg_isp isp;
	struct cedrus_jpeg_source source;
	u32 *registers;
	void __iomem *base;
	int ret;

	ret = cedrus_jpeg_prepare_source(&source, V4L2_PIX_FMT_NV12,
					 320, 240, 320,
					 320 * 240 * 3 / 2, 0xfbbe0000);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_isp_prepare(&source, &isp), 0);
	KUNIT_EXPECT_EQ(test, isp.input_size, 0x0028001eU);
	KUNIT_EXPECT_EQ(test, isp.input_stride, 0x00140014U);
	KUNIT_EXPECT_EQ(test, isp.control, 0U);
	KUNIT_EXPECT_EQ(test, isp.mb_width, 0x14U);
	KUNIT_EXPECT_EQ(test, isp.input_size_reversed, 0x001e0028U);
	KUNIT_EXPECT_EQ(test, isp.input_chroma2, (dma_addr_t)0xfbbf7700);

	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;
	cedrus_a133_jpeg_program_isp(base, &isp);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_OUTPUT_LUMA), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_OUTPUT_CHROMA), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_INPUT_LUMA),
			0xfbbe0000U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_INPUT_CHROMA),
			0xfbbf2c00U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_INPUT_CHROMA2),
			0xfbbf7700U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_FIXED_38), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_SRAM_OFFSET), 0U);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_ISP_SRAM_DATA),
			0x00f80008U);
}

static void cedrus_jpeg_a133_job_lifecycle(struct kunit *test)
{
	struct cedrus_a133_jpeg_job_buffers buffers = {
		.source = 0xfbbe0000,
		.source_size = 320 * 240 * 3 / 2,
		.bitstream = 0xf7000000,
		.bitstream_size = SZ_4M,
	};
	const struct cedrus_a133_jpeg_job_request request = {
		.pixelformat = V4L2_PIX_FMT_NV12,
		.width = 320,
		.height = 240,
		.bytesperline = 320,
		.quality = 95,
	};
	struct cedrus_a133_jpeg_job_completion completion;
	struct cedrus_a133_jpeg_job_lifecycle lifecycle;
	struct cedrus_a133_jpeg_job_plan plan;
	u32 *registers;
	void __iomem *base;
	u64 generation;
	int ret;

	cedrus_a133_jpeg_job_init(&lifecycle);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_prepare(&lifecycle, &request, &buffers, &plan), 0);
	KUNIT_EXPECT_EQ(test, lifecycle.state, CEDRUS_A133_JPEG_JOB_PREPARED);
	generation = plan.generation;

	registers = kunit_kzalloc(test, 0xc00, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, registers);
	base = (void __iomem *)registers;
	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY, base + VE_ENC_AVC_STATUS);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_trigger(&lifecycle, base, &plan), 0);
	KUNIT_EXPECT_EQ(test, lifecycle.state, CEDRUS_A133_JPEG_JOB_RUNNING);
	KUNIT_EXPECT_EQ(test, readl(base + VE_ENC_AVC_TRIGGER),
			VE_ENC_JPEG_TRIGGER_START);

	ret = cedrus_a133_jpeg_job_complete(&lifecycle, generation, &plan, 0,
					    4096 * 8, &completion);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = cedrus_a133_jpeg_job_complete(&lifecycle, generation, &plan,
					    CEDRUS_JPEG_STATUS_FINISH,
					    4096 * 8, &completion);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, completion.payload, (size_t)4098);
	KUNIT_EXPECT_EQ(test, lifecycle.state, CEDRUS_A133_JPEG_JOB_IDLE);
	KUNIT_EXPECT_EQ(test, lifecycle.active_generation, 0ULL);
	ret = cedrus_a133_jpeg_job_complete(&lifecycle, generation, &plan,
					    CEDRUS_JPEG_STATUS_FINISH,
					    4096 * 8, &completion);
	KUNIT_EXPECT_EQ(test, ret, -ESTALE);

	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_prepare(&lifecycle, &request,
						     &buffers, &plan), 0);
	generation = plan.generation;
	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY,
	       base + VE_ENC_AVC_STATUS);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_trigger(&lifecycle, base, &plan), 0);
	ret = cedrus_a133_jpeg_job_complete(&lifecycle, generation, &plan,
					    CEDRUS_JPEG_STATUS_FINISH |
					    CEDRUS_JPEG_STATUS_BUFFER_FULL,
					    0, &completion);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, completion.payload, (size_t)0);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_reset_complete(&lifecycle), 0);

	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_prepare(&lifecycle, &request,
						     &buffers, &plan), 0);
	generation = plan.generation;
	writel(VE_ENC_AVC_STATUS_PUT_BITS_READY,
	       base + VE_ENC_AVC_STATUS);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_trigger(&lifecycle, base, &plan), 0);
	ret = cedrus_a133_jpeg_job_complete(&lifecycle, generation, &plan,
					    CEDRUS_JPEG_STATUS_FINISH, 0,
					    &completion);
	KUNIT_EXPECT_EQ(test, ret, -ENODATA);
	KUNIT_EXPECT_EQ(test, completion.payload, (size_t)0);
	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_reset_complete(&lifecycle), 0);

	KUNIT_ASSERT_EQ(test,
			cedrus_a133_jpeg_job_prepare(&lifecycle, &request, &buffers, &plan), 0);
	generation = plan.generation;
	KUNIT_ASSERT_EQ(test, cedrus_a133_jpeg_job_abort(&lifecycle, generation), 0);
	KUNIT_EXPECT_EQ(test, cedrus_a133_jpeg_job_abort(&lifecycle, generation), 0);
	KUNIT_EXPECT_EQ(test, lifecycle.state,
			CEDRUS_A133_JPEG_JOB_RESET_REQUIRED);
	KUNIT_ASSERT_EQ(test, cedrus_a133_jpeg_job_reset_complete(&lifecycle), 0);
	KUNIT_ASSERT_EQ(test, cedrus_a133_jpeg_job_suspend(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, lifecycle.state, CEDRUS_A133_JPEG_JOB_SUSPENDED);
	KUNIT_ASSERT_EQ(test, cedrus_a133_jpeg_job_resume(&lifecycle), 0);
}

static void cedrus_jpeg_a133_job_rejects_unproven_or_aliasing(struct kunit *test)
{
	struct cedrus_a133_jpeg_job_buffers buffers = {
		.source = 0xfbbe0000,
		.source_size = 320 * 240 * 3 / 2,
		.bitstream = 0xf7000000,
		.bitstream_size = SZ_4M,
	};
	struct cedrus_a133_jpeg_job_request request = {
		.pixelformat = V4L2_PIX_FMT_NV12,
		.width = 320,
		.height = 240,
		.bytesperline = 320,
		.quality = 95,
	};
	struct cedrus_a133_jpeg_job_lifecycle lifecycle = {};
	struct cedrus_a133_jpeg_job_plan plan;

	request.quality = 94;
	KUNIT_EXPECT_EQ(test,
			cedrus_a133_jpeg_job_prepare(&lifecycle, &request, &buffers, &plan),
		-EOPNOTSUPP);
	request.quality = 95;
	buffers.bitstream = buffers.source;
	KUNIT_EXPECT_EQ(test,
			cedrus_a133_jpeg_job_prepare(&lifecycle, &request, &buffers, &plan),
		-EINVAL);
}

static struct kunit_case cedrus_jpeg_test_cases[] = {
	KUNIT_CASE(cedrus_jpeg_tables_match_stock),
	KUNIT_CASE(cedrus_jpeg_quality_bounds),
	KUNIT_CASE(cedrus_jpeg_header_is_standard_baseline),
	KUNIT_CASE(cedrus_jpeg_nv12_source_contract),
	KUNIT_CASE(cedrus_jpeg_capture_contract),
	KUNIT_CASE(cedrus_jpeg_capture_completion),
	KUNIT_CASE(cedrus_jpeg_a133_command_profile),
	KUNIT_CASE(cedrus_jpeg_a133_command_timeout),
	KUNIT_CASE(cedrus_jpeg_a133_plain_isp_contract),
	KUNIT_CASE(cedrus_jpeg_a133_job_lifecycle),
	KUNIT_CASE(cedrus_jpeg_a133_job_rejects_unproven_or_aliasing),
	{},
};

static struct kunit_suite cedrus_jpeg_test_suite = {
	.name = "cedrus-jpeg",
	.test_cases = cedrus_jpeg_test_cases,
};

kunit_test_suite(cedrus_jpeg_test_suite);

MODULE_DESCRIPTION("KUnit tests for Cedrus JPEG helpers");
MODULE_LICENSE("GPL");

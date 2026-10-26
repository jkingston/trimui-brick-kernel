// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <media/v4l2-jpeg.h>

#include "cedrus_jpeg.h"

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

static struct kunit_case cedrus_jpeg_test_cases[] = {
	KUNIT_CASE(cedrus_jpeg_tables_match_stock),
	KUNIT_CASE(cedrus_jpeg_quality_bounds),
	KUNIT_CASE(cedrus_jpeg_header_is_standard_baseline),
	{},
};

static struct kunit_suite cedrus_jpeg_test_suite = {
	.name = "cedrus-jpeg",
	.test_cases = cedrus_jpeg_test_cases,
};

kunit_test_suite(cedrus_jpeg_test_suite);

MODULE_DESCRIPTION("KUnit tests for Cedrus JPEG helpers");
MODULE_LICENSE("GPL");

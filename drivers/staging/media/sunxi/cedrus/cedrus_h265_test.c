// SPDX-License-Identifier: GPL-2.0-or-later

#include <kunit/test.h>

#include "cedrus_h265.h"

static void cedrus_h265_entry_points_contract(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, cedrus_h265_entry_points_match(0, 1));
	KUNIT_EXPECT_TRUE(test, cedrus_h265_entry_points_match(3, 3));
	KUNIT_EXPECT_FALSE(test, cedrus_h265_entry_points_match(3, 1));
}

static struct kunit_case cedrus_h265_test_cases[] = {
	KUNIT_CASE(cedrus_h265_entry_points_contract),
	{},
};

static struct kunit_suite cedrus_h265_test_suite = {
	.name = "cedrus-h265",
	.test_cases = cedrus_h265_test_cases,
};

kunit_test_suite(cedrus_h265_test_suite);

MODULE_DESCRIPTION("KUnit tests for Cedrus H.265 helpers");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "core-internal.h"

static void hwrng_quality_normal_source_test(struct kunit *test)
{
	struct hwrng rng = { };

	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 0), 0);
	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 1), 0);
	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 1024), 0);
	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 1025), -EINVAL);
}

static void hwrng_quality_zero_credit_source_test(struct kunit *test)
{
	struct hwrng rng = {
		.no_entropy_credit = true,
	};

	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 0), 0);
	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 1), -EPERM);
	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 1024), -EPERM);
	KUNIT_EXPECT_EQ(test, hwrng_validate_quality(&rng, 1025), -EINVAL);
}

static struct kunit_case hwrng_quality_test_cases[] = {
	KUNIT_CASE(hwrng_quality_normal_source_test),
	KUNIT_CASE(hwrng_quality_zero_credit_source_test),
	{ }
};

static struct kunit_suite hwrng_quality_test_suite = {
	.name = "hwrng-quality",
	.test_cases = hwrng_quality_test_cases,
};

kunit_test_suite(hwrng_quality_test_suite);

MODULE_DESCRIPTION("KUnit tests for the hwrng quality policy");
MODULE_LICENSE("GPL");

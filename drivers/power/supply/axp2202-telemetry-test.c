// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "axp2202-telemetry.h"

static const u32 brick_temperature_table[AXP2202_TEMP_POINTS] = {
	4591, 2781, 2190, 1738, 1390, 1118, 906, 606,
	415, 290, 244, 206, 175, 150, 110, 83,
};

static const int brick_temperatures[AXP2202_TEMP_POINTS] = {
	-250, -150, -100, -50, 0, 50, 100, 200,
	300, 400, 450, 500, 550, 600, 700, 800,
};

static void axp2202_temperature_table_test(struct kunit *test)
{
	const u32 *temperature_table = brick_temperature_table;
	u32 table[AXP2202_TEMP_POINTS];
	bool valid;
	int i;

	valid = axp2202_temperature_table_valid(temperature_table,
						ARRAY_SIZE(brick_temperature_table));
	KUNIT_EXPECT_TRUE(test, valid);
	valid = axp2202_temperature_table_valid(temperature_table,
						AXP2202_TEMP_POINTS - 1);
	KUNIT_EXPECT_FALSE(test, valid);

	memcpy(table, brick_temperature_table, sizeof(table));
	table[5] = table[4];
	valid = axp2202_temperature_table_valid(table, ARRAY_SIZE(table));
	KUNIT_EXPECT_FALSE(test, valid);
	memcpy(table, brick_temperature_table, sizeof(table));
	table[15] = 0;
	valid = axp2202_temperature_table_valid(table, ARRAY_SIZE(table));
	KUNIT_EXPECT_FALSE(test, valid);
	memcpy(table, brick_temperature_table, sizeof(table));
	table[0] = AXP2202_TEMP_MAX_MV + 1;
	valid = axp2202_temperature_table_valid(table, ARRAY_SIZE(table));
	KUNIT_EXPECT_FALSE(test, valid);

	for (i = 0; i < AXP2202_TEMP_POINTS; i++) {
		int millivolts = temperature_table[i];
		int temperature;

		temperature = axp2202_temperature_from_mv(temperature_table, millivolts);
		KUNIT_EXPECT_EQ(test, temperature, brick_temperatures[i]);
	}
}

static void axp2202_temperature_exhaustive_test(struct kunit *test)
{
	const u32 *temperature_table = brick_temperature_table;
	int previous = 800;
	int raw;

	for (raw = 0; raw < BIT(14); raw++) {
		int millivolts = raw * 500 / 1000;
		int temperature;

		temperature = axp2202_temperature_from_mv(temperature_table, millivolts);

		KUNIT_ASSERT_GE(test, temperature, -250);
		KUNIT_ASSERT_LE(test, temperature, 800);
		if (raw)
			KUNIT_ASSERT_LE(test, temperature, previous);
		previous = temperature;
	}
}

static void axp2202_adc_decode_exhaustive_test(struct kunit *test)
{
	unsigned int high, low;

	for (high = 0; high <= U8_MAX; high++)
		for (low = 0; low <= U8_MAX; low++)
			KUNIT_ASSERT_EQ(test, axp2202_adc_raw(high, low),
					(u16)(((high & 0x3f) << 8) | low));
}

static int expected_health(unsigned int status, unsigned int fault)
{
	if (!(status & BIT(3)))
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	if (status & BIT(2))
		return POWER_SUPPLY_HEALTH_DEAD;
	if (fault & BIT(1))
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (fault & BIT(0))
		return POWER_SUPPLY_HEALTH_COLD;
	return POWER_SUPPLY_HEALTH_GOOD;
}

static void axp2202_health_exhaustive_test(struct kunit *test)
{
	unsigned int status, fault;

	for (status = 0; status <= U8_MAX; status++)
		for (fault = 0; fault <= U8_MAX; fault++) {
			int actual = axp2202_health_from_registers(status, fault);
			int expected = expected_health(status, fault);

			KUNIT_ASSERT_EQ(test, actual, expected);
		}
}

static struct kunit_case axp2202_telemetry_test_cases[] = {
	KUNIT_CASE(axp2202_temperature_table_test),
	KUNIT_CASE(axp2202_temperature_exhaustive_test),
	KUNIT_CASE(axp2202_adc_decode_exhaustive_test),
	KUNIT_CASE(axp2202_health_exhaustive_test),
	{}
};

static struct kunit_suite axp2202_telemetry_test_suite = {
	.name = "axp2202-telemetry",
	.test_cases = axp2202_telemetry_test_cases,
};

kunit_test_suite(axp2202_telemetry_test_suite);

MODULE_LICENSE("GPL");

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AXP2202_TELEMETRY_H
#define __AXP2202_TELEMETRY_H

#include <linux/power_supply.h>
#include <linux/types.h>

#define AXP2202_TEMP_POINTS	16
#define AXP2202_TEMP_MAX_MV	8191

static inline bool axp2202_temperature_table_valid(const u32 *table,
						   size_t count)
{
	size_t i;

	if (count != AXP2202_TEMP_POINTS)
		return false;
	for (i = 0; i < count; i++) {
		if (!table[i] || table[i] > AXP2202_TEMP_MAX_MV)
			return false;
		if (i && table[i - 1] <= table[i])
			return false;
	}

	return true;
}

static inline int axp2202_temperature_from_mv(const u32 *table, int mv)
{
	static const int temperatures[AXP2202_TEMP_POINTS] = {
		-250, -150, -100, -50, 0, 50, 100, 200,
		300, 400, 450, 500, 550, 600, 700, 800,
	};
	int i;

	if (mv >= table[0])
		return temperatures[0];
	if (mv <= table[AXP2202_TEMP_POINTS - 1])
		return temperatures[AXP2202_TEMP_POINTS - 1];

	for (i = 0; i < AXP2202_TEMP_POINTS - 1; i++) {
		int temperature;

		if (mv < table[i + 1])
			continue;
		temperature = temperatures[i];
		temperature += (table[i] - mv) *
			(temperatures[i + 1] - temperatures[i]) /
			(table[i] - table[i + 1]);
		return temperature;
	}

	return temperatures[AXP2202_TEMP_POINTS - 1];
}

static inline u16 axp2202_adc_raw(u8 high, u8 low)
{
	return ((high & 0x3f) << 8) | low;
}

static inline int axp2202_health_from_registers(unsigned int status,
						unsigned int fault)
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

#endif /* __AXP2202_TELEMETRY_H */

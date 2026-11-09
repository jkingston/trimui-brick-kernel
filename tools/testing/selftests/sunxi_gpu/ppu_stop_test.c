// SPDX-License-Identifier: GPL-2.0-only
// Host MMIO-sequence regression for the stock stopped GPU power-off path.
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define __iomem
#define BIT(n) (1U << (n))
static uint32_t registers[16];
static struct { unsigned int offset; uint32_t value; } writes[32];
static unsigned int count;
static unsigned int rejected_offset;
static uint32_t readl(const void *address)
{
	return *(const uint32_t *)address;
}
static void writel(uint32_t value, void *address)
{
	unsigned int offset = (uint8_t *)address - (uint8_t *)registers;
	assert(count < 32 && offset < sizeof(registers) && offset % 4 == 0);
	writes[count].offset = offset;
	writes[count++].value = value;
	if (offset != rejected_offset) *(uint32_t *)address = value;
}
static void udelay(unsigned int us) { (void)us; }
#include "../../../../drivers/pmdomain/sunxi/sun50i-a133-gpu-off.h"

static void test_stopped_off(unsigned int initial_status)
{
	memset(registers, 0, sizeof(registers));
	count = 0;
	rejected_offset = ~0U;
	registers[0] = registers[1] = 1;
	registers[0x20 / 4] = 0x80 | initial_status;
	registers[0x24 / 4] = 0x40 | 2; /* HW, preserve unrelated bits */
	registers[0x2c / 4] = 0x20;
	assert(brick_ppu_stopped_power_off(registers));
	assert(count == 12);
	assert(writes[0].offset == 0x2c && writes[0].value == 0x20);
	assert(writes[1].offset == 0x24 && writes[1].value == 0x41);
	assert(writes[2].offset == 0x20 && writes[2].value == 0x81);
	/* Isolation/gating follows SW/HOST metadata, never precedes it. */
	assert(writes[3].offset == 0x08 && writes[3].value == 2);
	assert(writes[4].offset == 0x08 && writes[4].value == 3);
	assert(registers[0] == 0 && registers[1] == 0);
	assert(registers[2] == 3 && registers[0x20 / 4] == 2);
	assert(registers[0x24 / 4] == 0x41 && registers[0x2c / 4] == 0);
}

int main(void)
{
	assert(brick_ppu_stopped_accessible(1, 1, 0, 0, 2, 0, 2));
	assert(brick_ppu_stopped_accessible(1, 1, 0, 1, 2, 0, 2));
	assert(brick_ppu_stopped_accessible(1, 1, 2, 1, 2, 0, 2));
	assert(!brick_ppu_stopped_accessible(1, 1, 2, 0, 2, 0, 2));
	assert(!brick_ppu_stopped_accessible(0, 0, 3, 2, 1, 0, 1));
	assert(!brick_ppu_stopped_accessible(1, 1, 0, 1, 2, 1, 2));
	assert(!brick_ppu_stopped_accessible(1, 1, 0, 1, 1, 0, 2));
	test_stopped_off(0); /* Regression: stopped firmware may leave PPU in 3D. */
	test_stopped_off(1);
	for (unsigned int offset = 0x20; offset <= 0x24; offset += 4) {
		memset(registers, 0, sizeof(registers));
		count = 0; rejected_offset = offset;
		registers[0] = registers[1] = 1;
		registers[0x24 / 4] = 2;
		assert(!brick_ppu_stopped_power_off(registers));
		assert(count <= 3); /* Never isolate, gate or reset after failed handoff. */
		assert(registers[0] == 1 && registers[1] == 1);
	}
	puts("PASS: stock stopped SW/HOST -> ALLOFF write ordering");
	return 0;
}

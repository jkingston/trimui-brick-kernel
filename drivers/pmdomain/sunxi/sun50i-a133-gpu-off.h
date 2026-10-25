/* SPDX-License-Identifier: GPL-2.0-only */
/* Research only. Caller must own stopped SW/HOST and drain GPU IRQ first. */
static void brick_ppu_power_off(void __iomem *base)
{
	writel(readl(base + 0x08) | BIT(1), base + 0x08);
	writel(readl(base + 0x08) | BIT(0), base + 0x08);
	writel(0, base + 0x04);
	udelay(1);
	udelay(1);
	writel(0, base + 0x00);
	writel(0xff0000, base + 0x10);
	writel(0xff, base + 0x10);
	writel(2, base + 0x20);
	writel(1, base + 0x28);
	writel(0, base + 0x2c);
}

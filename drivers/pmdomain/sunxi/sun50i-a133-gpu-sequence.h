/* SPDX-License-Identifier: GPL-2.0 */
/* A133 revision 6 GPU domain/reset sequence; caller validates state. */
static void brick_ppu_power_on(void __iomem *base)
{
	static const unsigned int bits[] = {23, 18, 16, 19, 20, 21, 22, 17};
	unsigned int i;
	for (i = 0; i < 8; i++) {
		writel(BIT(i), base + 0x0c);
		udelay(1);
	}
	writel(1, base + 0x04);
	udelay(1);
	writel(readl(base + 0x08) & ~BIT(0), base + 0x08);
	for (i = 0; i < ARRAY_SIZE(bits); i++) {
		writel(BIT(bits[i]), base + 0x0c);
		udelay(i < 2 ? 10 : 5);
	}
	writel(readl(base + 0x08) & ~BIT(1), base + 0x08);
	udelay(1);
	writel(1, base + 0x00);
	udelay(10);
	writel(0, base + 0x20);
	writel(1, base + 0x28);
	/* Hardware control is selected by the caller; keep request IRQ masked. */
	writel(0, base + 0x2c);
}

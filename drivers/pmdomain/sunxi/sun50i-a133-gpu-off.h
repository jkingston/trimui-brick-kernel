/* SPDX-License-Identifier: GPL-2.0-only */
/* Research only. Caller must own stopped SW/HOST and drain GPU IRQ first. */
/* Firmware can report HOST before 3D isolation changes. On-device idle trace:
 * reset=1 gate=1 isolation=0 status=1 control=2 mask=0. This tuple is accessible
 * after a verified stop; HOST alone must never authorize power removal.
 */
static bool brick_ppu_stopped_accessible(unsigned int reset, unsigned int gate,
	unsigned int isolation, unsigned int status, unsigned int control,
	unsigned int mask, unsigned int expected_mode)
{
	if (!(reset & 1) || !(gate & 1) || (control & 3) != expected_mode || (mask & 1))
		return false;
	return (!(status & 3) && !(isolation & 3) && expected_mode == 2) ||
	       ((status & 3) == 1 && (!(isolation & 3) || (isolation & 3) == 2));
}

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

/* Stock 1.1.1 pvrsrvkm.ko sunxiPostPowerState (0x86d20..0x86da8):
 * mask request IRQ, select SW=1, read back, then write HOST=1. HOST is
 * protocol state, NOT proof that firmware or IRQ handlers have stopped.
 * Caller must hold the DDK/provider locks, consume successful same-transaction
 * firmware-stop evidence and synchronously drain the GPU IRQ before entry.
 * The subsequent internal-off sequence preserves the stock power behaviour.
 */
static bool brick_ppu_stopped_power_off(void __iomem *base)
{
	writel(readl(base + 0x2c) & ~1U, base + 0x2c);
	writel((readl(base + 0x24) & ~3U) | 1, base + 0x24);
	if ((readl(base + 0x24) & 3) != 1)
		return false;
	writel((readl(base + 0x20) & ~3U) | 1, base + 0x20);
	if ((readl(base + 0x20) & 3) != 1)
		return false;
	brick_ppu_power_off(base);
	return true;
}

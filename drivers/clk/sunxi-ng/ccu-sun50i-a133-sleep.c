// SPDX-License-Identifier: GPL-2.0-only
/* A133 stock-chain CCU context. */
#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/firmware/sun50i-a133-native.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/sun50i-a133-ccu-sleep.h>

#include "ccu-sun50i-a133-sleep.h"

#define A133_STOCK_MONITOR_UID		0x8000ff01
#define A133_STOCK_UID_WORD0		0x83b53a5b
#define A133_STOCK_UID_WORD1		0x40b9c594
#define A133_STOCK_UID_WORD2		0xfcf29153
#define A133_STOCK_UID_WORD3		0x48862954
struct a133_context_reg {
	u16 offset;
	u32 mask, saved;
};

/*
 * The stock SCP restores these fields before returning to the application
 * CPUs.  A changed tuple means that Linux cannot safely continue.
 * Stock excludes COM PLL because its powered-down rail may reconstruct a
 * different, currently unused tuple; consumers program it when enabled.
 */
static struct a133_context_reg main_retained[] = {
	{ 0x000, 0xafffff03 },
	{ 0x500, 0x07000303 },
	{ 0x800, 0x03000003 },
	{ 0x80c, 0x00010001 },
};

/*
 * The stock SCP reconstructs MBUS in stages and intentionally leaves its
 * reset bit clear.  Restore the complete pre-suspend MBUS tuple only after
 * the retained DDR/peripheral PLLs have been verified and locked.
 */
static struct a133_context_reg main_handoff[] = {
	{ 0x540, 0xc3000007 },
};

static struct a133_context_reg r_critical[] = {
	{ 0x000, 0x0300031f },
	{ 0x00c, 0x00000003 },
	{ 0x010, 0x0300031f },
	{ 0x20c, 0x00010001 },
};

/* Writable fields from the A133 registered peripheral descriptors. Alias
 * fields share one snapshot; unrelated bits and status are never replayed.
 */
static struct a133_context_reg main_periph[] = {
	{ 0x79c, 0x00010001 },
	{ 0x510, 0x03000303 },
	{ 0x51c, 0x03000303 },
	{ 0x520, 0x03000303 },
	{ 0x524, 0x03000303 },
	{ 0x60c, 0x00030003 },
	{ 0x600, 0x8100000f },
	{ 0x604, 0x8100000f },
	{ 0x804, 0x00000727 },
	{ 0x63c, 0x00010001 },
	{ 0x630, 0x8100000f },
	{ 0x61c, 0x00010001 },
	{ 0x610, 0x8100000f },
	{ 0x640, 0x8700000f },
	{ 0x67c, 0x00010001 },
	{ 0x670, 0x81000003 },
	{ 0x68c, 0x00010001 },
	{ 0x680, 0x8100030f },
	{ 0x69c, 0x00010001 },
	{ 0x690, 0x81000007 },
	{ 0x70c, 0x00010001 },
	{ 0x71c, 0x00010001 },
	{ 0x72c, 0x00010001 },
	{ 0x73c, 0x00010001 },
	{ 0x740, 0x80000000 },
	{ 0x78c, 0x00010001 },
	{ 0x7ac, 0x00010001 },
	{ 0x7bc, 0x00000001 },
	{ 0x82c, 0x00010001 },
	{ 0x810, 0x8700030f },
	{ 0x814, 0x8700030f },
	{ 0x830, 0x8300030f },
	{ 0x84c, 0x00070007 },
	{ 0x834, 0x8300030f },
	{ 0x838, 0x8300030f },
	{ 0x90c, 0x007f007f },
	{ 0x91c, 0x003f003f },
	{ 0x93c, 0x00010001 },
	{ 0x96c, 0x00070007 },
	{ 0x940, 0x8700030f },
	{ 0x944, 0x8700030f },
	{ 0x948, 0x8700030f },
	{ 0x970, 0xc0000000 },
	{ 0x974, 0xc0000000 },
	{ 0x97c, 0x00030003 },
	{ 0x9ec, 0x00010001 },
	{ 0x9cc, 0x00010001 },
	{ 0x9c0, 0x8f00030f },
	{ 0x9fc, 0x00010001 },
	{ 0xa20, 0x000f000f },
	{ 0xa10, 0x83000300 },
	{ 0xa14, 0x83000300 },
	{ 0xa18, 0x83000300 },
	{ 0xa1c, 0x83000300 },
	{ 0xa2c, 0x00010001 },
	{ 0xa24, 0x83000300 },
	{ 0xa4c, 0x00010001 },
	{ 0xa40, 0x83000300 },
	{ 0xa5c, 0x00010001 },
	{ 0xa50, 0x8300000f },
	{ 0xa54, 0x8300000f },
	{ 0xa58, 0x8300000f },
	{ 0xa70, 0xe3000000 },
	{ 0xa74, 0xe3000000 },
	{ 0xa8c, 0x01330133 },
	{ 0xb5c, 0x00010001 },
	{ 0xabc, 0x00010001 },
	{ 0xacc, 0x00010001 },
	{ 0xb7c, 0x00030003 },
	{ 0xb60, 0x87000000 },
	{ 0xb64, 0x87000000 },
	{ 0xbac, 0x00030000 },
	{ 0xb4c, 0x00010001 },
	{ 0xb24, 0x8300000f },
	{ 0xc1c, 0x00010001 },
	{ 0xc04, 0x8700000f },
	{ 0xc2c, 0x00010001 },
	{ 0xc20, 0x8700000f },
	{ 0xc08, 0x8700001f },
	{ 0xc0c, 0x8700001f },
	{ 0xbfc, 0x00010001 },
	{ 0xbf0, 0x8100030f },
	{ 0xa9c, 0x00010001 },
};

static struct a133_context_reg r_periph[] = {
	{ 0x13c, 0x00010001 },
	{ 0x130, 0x83000000 },
	{ 0x19c, 0x00030003 },
	{ 0x1cc, 0x00010001 },
	{ 0x1c0, 0x8100031f },
	{ 0x17c, 0x00010001 },
	{ 0x18c, 0x00010001 },
};

/* Pattern first, then writable factors/enable/lock-enable; never replay lock. */
static struct a133_context_reg pll_regs[] = {
	{ 0x020, 0xa900ff03 }, { 0x028, 0xa900ff03 },
	{ 0x030, 0xa900ff03 }, { 0x040, 0xa900ff03 },
	{ 0x048, 0xa900ff03 }, { 0x050, 0xa900ff03 },
	{ 0x068, 0xa900ff03 }, { 0x058, 0xa900ff03 },
	{ 0x078, 0xa93fff03 },
};

static u32 patterns[ARRAY_SIZE(pll_regs)];
static void __iomem *bases[2];
static bool enabled, saved;
static int context_error;

static bool a133_firmware_suspend_ready(void);

int sun50i_a133_ccu_deep_ready(void)
{
	if (!READ_ONCE(enabled))
		return -EOPNOTSUPP;
	return READ_ONCE(context_error);
}
EXPORT_SYMBOL_GPL(sun50i_a133_ccu_deep_ready);

static void save_regs(void __iomem *base, struct a133_context_reg *regs, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		regs[i].saved = readl(base + regs[i].offset) & regs[i].mask;
}

static int check_regs(void __iomem *base, struct a133_context_reg *regs,
		      size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		u32 value;

		value = readl(base + regs[i].offset) & regs[i].mask;

		if (value != regs[i].saved) {
			pr_err("A133 %s CCU retention %#x: expected %#x, got %#x\n",
			       base == bases[1] ? "R" : "main", regs[i].offset,
			       regs[i].saved, value);
			return -EIO;
		}
	}
	return 0;
}

static int restore_regs(void __iomem *base, struct a133_context_reg *regs,
			size_t n)
{
	size_t i;
	u32 value;

	for (i = 0; i < n; i++) {
		value = readl(base + regs[i].offset);
		if ((value & regs[i].mask) == regs[i].saved)
			continue;
		writel((value & ~regs[i].mask) | regs[i].saved, base + regs[i].offset);
		if ((readl(base + regs[i].offset) & regs[i].mask) != regs[i].saved)
			return -EIO;
	}
	return 0;
}

static int a133_context_suspend(void *unused)
{
	size_t i;

	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;
	if (!enabled || context_error)
		return context_error ?: -EOPNOTSUPP;
	if (!a133_firmware_suspend_ready())
		return -EIO;
	/* Never reprogram peripheral PLL0 while the CPU is using its bypass. */
	if (((readl(bases[0] + 0x500) >> 24) & 7) != 3)
		return -EBUSY;
	save_regs(bases[0], main_retained, ARRAY_SIZE(main_retained));
	save_regs(bases[0], main_handoff, ARRAY_SIZE(main_handoff));
	save_regs(bases[1], r_critical, ARRAY_SIZE(r_critical));
	save_regs(bases[0], pll_regs, ARRAY_SIZE(pll_regs));
	for (i = 0; i < ARRAY_SIZE(pll_regs); i++)
		patterns[i] = readl(bases[0] + pll_regs[i].offset + 0x100);
	save_regs(bases[0], main_periph, ARRAY_SIZE(main_periph));
	save_regs(bases[1], r_periph, ARRAY_SIZE(r_periph));
	saved = true;
	return 0;
}

static int a133_critical_ready(void)
{
	static const u16 critical_plls[] = { 0x000, 0x010 };
	size_t i;
	u32 value;
	int ret;

	/* Never retune a live parent of the SCP, memory or system buses. */
	if ((((readl(bases[1]) >> 24) & 3) == 3 ||
	     ((readl(bases[1] + 0x10) >> 24) & 3) == 3 ||
	     ((readl(bases[0] + 0x540) >> 24) & 3) >= 2 ||
	     ((readl(bases[0] + 0x510) >> 24) & 3) == 3 ||
	     ((readl(bases[0] + 0x51c) >> 24) & 3) == 3 ||
	     ((readl(bases[0] + 0x520) >> 24) & 3) == 3 ||
	     ((readl(bases[0] + 0x524) >> 24) & 3) == 3) &&
	    ((readl(bases[0] + 0x20) & pll_regs[0].mask) != pll_regs[0].saved ||
	     readl(bases[0] + 0x120) != patterns[0])) {
		pr_err("A133 critical parent PLL0 differs; refusing live retune\n");
		return -EIO;
	}
	if (((readl(bases[0] + 0x800) >> 24) & 3) == 1 &&
	    ((readl(bases[0] + 0x28) & pll_regs[1].mask) != pll_regs[1].saved ||
	     readl(bases[0] + 0x128) != patterns[1])) {
		pr_err("A133 DRAM parent PLL1 differs; refusing live retune\n");
		return -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(critical_plls); i++) {
		if (!(readl(bases[0] + critical_plls[i]) & BIT(31)))
			continue;
		ret = readl_poll_timeout_atomic(bases[0] + critical_plls[i], value,
						value & BIT(28), 1, 1000);
		if (ret) {
			pr_err("A133 critical PLL lock timeout at %#x\n", critical_plls[i]);
			return ret;
		}
	}
	return 0;
}

static void a133_context_resume(void *unused)
{
	size_t i;
	u32 value;
	int ret;

	if (!saved)
		return;
	saved = false;
	ret = check_regs(bases[0], main_retained, ARRAY_SIZE(main_retained));
	if (!ret)
		ret = check_regs(bases[1], r_critical, ARRAY_SIZE(r_critical));
	if (ret)
		goto failed;
	/*
	 * The SCP has already resumed DRAM through this live PLL.  Replaying the
	 * stock 4.9 cache here can stall the CPU/DRAM path, so accept the locked
	 * SCP result instead of retuning active memory.
	 */
	ret = a133_critical_ready();
	if (ret)
		goto failed;
	for (i = 0; i < ARRAY_SIZE(pll_regs); i++) {
		value = readl(bases[0] + pll_regs[i].offset + 0x100);
		if (value != patterns[i])
			writel(patterns[i], bases[0] + pll_regs[i].offset + 0x100);
		if (readl(bases[0] + pll_regs[i].offset + 0x100) != patterns[i]) {
			pr_err("A133 PLL pattern restore failed at %#x\n",
			       pll_regs[i].offset + 0x100);
			ret = -EIO;
			goto failed;
		}
		ret = restore_regs(bases[0], &pll_regs[i], 1);
		if (ret)
			goto failed;
		if (!(pll_regs[i].saved & BIT(31)))
			continue;
		ret = readl_poll_timeout_atomic(bases[0] + pll_regs[i].offset, value,
						value & BIT(28), 1, 1000);
		if (ret) {
			pr_err("A133 PLL lock timeout at %#x\n", pll_regs[i].offset);
			goto failed;
		}
	}
	ret = restore_regs(bases[0], main_handoff, ARRAY_SIZE(main_handoff));
	if (ret)
		goto failed;
	ret = restore_regs(bases[0], main_periph, ARRAY_SIZE(main_periph));
	if (!ret)
		ret = restore_regs(bases[1], r_periph, ARRAY_SIZE(r_periph));
	if (!ret)
		return;
failed:
	WRITE_ONCE(context_error, ret);
	/*
	 * syscore resume callbacks cannot return an error.  Continuing would
	 * enable early IRQs and run noirq device callbacks with physical clocks
	 * and resets that no longer match the CCF state.  Stop here, while one
	 * CPU is online and interrupts remain disabled, before any later syscore
	 * or device callback can touch that indeterminate hardware.
	 */
	panic("A133 CCU deep context restore failed: %d", ret);
}

static const struct syscore_ops a133_context_ops = {
	.suspend = a133_context_suspend,
	.resume = a133_context_resume,
};

static struct syscore a133_context = { .ops = &a133_context_ops };

static bool a133_firmware_suspend_ready(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(A133_STOCK_MONITOR_UID, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((u32)res.a0 == A133_STOCK_UID_WORD0 &&
	    (u32)res.a1 == A133_STOCK_UID_WORD1 &&
	    (u32)res.a2 == A133_STOCK_UID_WORD2 &&
	    (u32)res.a3 == A133_STOCK_UID_WORD3)
		return true;

	arm_smccc_smc(A133_NATIVE_UID, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((u32)res.a0 != A133_NATIVE_UID_WORD0 ||
	    (u32)res.a1 != A133_NATIVE_UID_WORD1 ||
	    (u32)res.a2 != A133_NATIVE_UID_WORD2 ||
	    (u32)res.a3 != A133_NATIVE_UID_WORD3)
		return false;

	arm_smccc_smc(A133_NATIVE_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((u32)res.a0 != A133_NATIVE_VERSION_MAJOR)
		return false;

	arm_smccc_smc(A133_NATIVE_CAPABILITIES, 0, 0, 0, 0, 0, 0, 0, &res);
	return (s32)res.a0 == A133_NATIVE_SUCCESS &&
	       ((u32)res.a1 & A133_NATIVE_CAP_SYSTEM_SUSPEND);
}

static void a133_context_remove(void *data)
{
	unsigned long bank = (unsigned long)data - 1;

	if (enabled) {
		WRITE_ONCE(enabled, false);
		unregister_syscore(&a133_context);
	}
	bases[bank] = NULL;
}

int sun50i_a133_ccu_sleep_init(struct device *dev, void __iomem *base, bool r_ccu)
{
	struct resource *resource;
	int ret;

	if (!of_property_read_bool(dev->of_node, "allwinner,stock-suspend"))
		return 0;
	if (!of_machine_is_compatible("trimui,brick"))
		return -ENODEV;
	resource = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 0);
	if (!resource || resource->start != (r_ccu ? 0x07010000 : 0x03001000) ||
	    resource_size(resource) < (r_ccu ? 0x210 : 0xc30))
		return -EINVAL;
	if (!a133_firmware_suspend_ready())
		return -ENODEV;
	if (bases[r_ccu])
		return -EBUSY;
	bases[r_ccu] = base;
	ret = devm_add_action_or_reset(dev, a133_context_remove,
				       (void *)(unsigned long)(r_ccu + 1));
	if (ret)
		return ret;
	if (bases[0] && bases[1]) {
		register_syscore_early_resume(&a133_context);
		WRITE_ONCE(enabled, true);
	}
	return 0;
}

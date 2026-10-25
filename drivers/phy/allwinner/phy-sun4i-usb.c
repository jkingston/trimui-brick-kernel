// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner sun4i USB phy driver
 *
 * Copyright (C) 2014-2015 Hans de Goede <hdegoede@redhat.com>
 *
 * Based on code from
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 *
 * Modelled after: Samsung S5P/Exynos SoC series MIPI CSIS/DSIM DPHY driver
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 * Author: Sylwester Nawrocki <s.nawrocki@samsung.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/extcon-provider.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-sun4i-usb.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/usb/of.h>
#include <linux/usb/role.h>
#include <linux/workqueue.h>

#define REG_ISCR			0x00
#define REG_PHYCTL_A10			0x04
#define REG_PHYBIST			0x08
#define REG_PHYTUNE			0x0c
#define REG_PHYCTL_A33			0x10
#define REG_PHY_OTGCTL			0x20

#define REG_HCI_PHY_CTL			0x10

#define PHYCTL_DATA			BIT(7)

#define OTGCTL_ROUTE_MUSB		BIT(0)

#define SUNXI_AHB_ICHR8_EN		BIT(10)
#define SUNXI_AHB_INCR4_BURST_EN	BIT(9)
#define SUNXI_AHB_INCRX_ALIGN_EN	BIT(8)
#define SUNXI_ULPI_BYPASS_EN		BIT(0)

/* ISCR, Interface Status and Control bits */
#define ISCR_ID_PULLUP_EN		(1 << 17)
#define ISCR_DPDM_PULLUP_EN	(1 << 16)
/* sunxi has the phy id/vbus pins not connected, so we use the force bits */
#define ISCR_FORCE_ID_MASK	(3 << 14)
#define ISCR_FORCE_ID_LOW		(2 << 14)
#define ISCR_FORCE_ID_HIGH	(3 << 14)
#define ISCR_FORCE_VBUS_MASK	(3 << 12)
#define ISCR_FORCE_VBUS_LOW	(2 << 12)
#define ISCR_FORCE_VBUS_HIGH	(3 << 12)

/* Common Control Bits for Both PHYs */
#define PHY_PLL_BW			0x03
#define PHY_RES45_CAL_EN		0x0c

/* Private Control Bits for Each PHY */
#define PHY_TX_AMPLITUDE_TUNE		0x20
#define PHY_TX_SLEWRATE_TUNE		0x22
#define PHY_VBUSVALID_TH_SEL		0x25
#define PHY_PULLUP_RES_SEL		0x27
#define PHY_OTG_FUNC_EN			0x28
#define PHY_VBUS_DET_EN			0x29
#define PHY_DISCON_TH_SEL		0x2a
#define PHY_SQUELCH_DETECT		0x3c

/* A83T specific control bits for PHY0 */
#define PHY_CTL_VBUSVLDEXT		BIT(5)
#define PHY_CTL_SIDDQ			BIT(3)
#define PHY_CTL_H3_SIDDQ		BIT(1)

/* A83T specific control bits for PHY2 HSIC */
#define SUNXI_EHCI_HS_FORCE		BIT(20)
#define SUNXI_HSIC_CONNECT_DET		BIT(17)
#define SUNXI_HSIC_CONNECT_INT		BIT(16)
#define SUNXI_HSIC			BIT(1)

#define MAX_PHYS			4

/*
 * Note do not raise the debounce time, we must report Vusb high within 100ms
 * otherwise we get Vbus errors
 */
#define DEBOUNCE_TIME			msecs_to_jiffies(50)
#define POLL_TIME			msecs_to_jiffies(250)

struct sun4i_usb_phy_cfg {
	int hsic_index;
	u32 disc_thresh;
	u32 hci_phy_ctl_clear;
	u32 phy0_ctl_clear;
	u32 phy0_ctl_init_clear;
	bool needs_phy1_clock;
	u8 phyctl_offset;
	bool dedicated_clocks;
	bool phy0_dual_route;
	bool needs_phy2_siddq;
	bool siddq_in_base;
	bool poll_vbusen;
	int missing_phys;
};

struct sun4i_usb_phy_data {
	void __iomem *base;
	const struct sun4i_usb_phy_cfg *cfg;
	enum usb_dr_mode dr_mode;
	bool role_switch;
	spinlock_t reg_lock; /* guard access to phyctl reg */
	int num_phys;
	struct reset_control *phy1_bus_reset;
	struct sun4i_usb_phy {
		struct phy *phy;
		void __iomem *pmu;
		struct regulator *vbus;
		struct reset_control *reset;
		struct clk *clk;
		struct clk *clk2;
		bool regulator_on;
		int vbus_error;
		int index;
	} phys[MAX_PHYS];
	/* phy0 / otg related variables */
	struct extcon_dev *extcon;
	bool phy0_init;
	struct gpio_desc *id_det_gpio;
	struct gpio_desc *vbus_det_gpio;
	struct power_supply *vbus_power_supply;
	struct notifier_block vbus_power_nb;
	bool vbus_power_nb_registered;
	bool force_session_end;
	int id_det_irq;
	int vbus_det_irq;
	int id_det;
	int vbus_det;
	struct delayed_work detect;
};

#define to_sun4i_usb_phy_data(phy) \
	container_of((phy), struct sun4i_usb_phy_data, phys[(phy)->index])

static void sun4i_usb_phy0_update_iscr(struct phy *_phy, u32 clr, u32 set)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	u32 iscr;

	iscr = readl(data->base + REG_ISCR);
	iscr &= ~clr;
	iscr |= set;
	writel(iscr, data->base + REG_ISCR);
}

static void sun4i_usb_phy0_set_id_detect(struct phy *phy, u32 val)
{
	if (val)
		val = ISCR_FORCE_ID_HIGH;
	else
		val = ISCR_FORCE_ID_LOW;

	sun4i_usb_phy0_update_iscr(phy, ISCR_FORCE_ID_MASK, val);
}

static void sun4i_usb_phy0_set_vbus_detect(struct phy *phy, u32 val)
{
	if (val)
		val = ISCR_FORCE_VBUS_HIGH;
	else
		val = ISCR_FORCE_VBUS_LOW;

	sun4i_usb_phy0_update_iscr(phy, ISCR_FORCE_VBUS_MASK, val);
}

static void sun4i_usb_phy_write(struct sun4i_usb_phy *phy, u32 addr, u32 data,
				int len)
{
	struct sun4i_usb_phy_data *phy_data = to_sun4i_usb_phy_data(phy);
	u32 temp, usbc_bit = BIT(phy->index * 2);
	void __iomem *phyctl = phy_data->base + phy_data->cfg->phyctl_offset;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&phy_data->reg_lock, flags);

	if (phy_data->cfg->phyctl_offset == REG_PHYCTL_A33) {
		/* SoCs newer than A33 need us to set phyctl to 0 explicitly */
		writel(0, phyctl);
	}

	for (i = 0; i < len; i++) {
		temp = readl(phyctl);

		/* clear the address portion */
		temp &= ~(0xff << 8);

		/* set the address */
		temp |= ((addr + i) << 8);
		writel(temp, phyctl);

		/* set the data bit and clear usbc bit*/
		temp = readb(phyctl);
		if (data & 0x1)
			temp |= PHYCTL_DATA;
		else
			temp &= ~PHYCTL_DATA;
		temp &= ~usbc_bit;
		writeb(temp, phyctl);

		/* pulse usbc_bit */
		temp = readb(phyctl);
		temp |= usbc_bit;
		writeb(temp, phyctl);

		temp = readb(phyctl);
		temp &= ~usbc_bit;
		writeb(temp, phyctl);

		data >>= 1;
	}

	spin_unlock_irqrestore(&phy_data->reg_lock, flags);
}

static void sun4i_usb_phy_passby(struct sun4i_usb_phy *phy, int enable)
{
	struct sun4i_usb_phy_data *phy_data = to_sun4i_usb_phy_data(phy);
	u32 bits, reg_value;

	if (!phy->pmu)
		return;

	bits = SUNXI_AHB_ICHR8_EN | SUNXI_AHB_INCR4_BURST_EN |
		SUNXI_AHB_INCRX_ALIGN_EN | SUNXI_ULPI_BYPASS_EN;

	/* A83T USB2 is HSIC */
	if (phy_data->cfg->hsic_index &&
	    phy->index == phy_data->cfg->hsic_index)
		bits |= SUNXI_EHCI_HS_FORCE | SUNXI_HSIC_CONNECT_INT |
			SUNXI_HSIC;

	reg_value = readl(phy->pmu);

	if (enable)
		reg_value |= bits;
	else
		reg_value &= ~bits;

	writel(reg_value, phy->pmu);
}

/* PHY0 keeps a shared vote on the Brick PHY1 clock/reset domain. */
static int sun4i_usb_phy1_clock_enable(struct sun4i_usb_phy_data *data)
{
	struct sun4i_usb_phy *phy1 = &data->phys[1];
	int ret;

	ret = clk_prepare_enable(phy1->clk2);
	if (ret)
		return ret;
	ret = reset_control_deassert(data->phy1_bus_reset);
	if (ret)
		goto disable_bus_clock;
	ret = clk_prepare_enable(phy1->clk);
	if (ret)
		goto assert_bus_reset;
	ret = reset_control_deassert(phy1->reset);
	if (ret)
		goto disable_phy_clock;
	return 0;

disable_phy_clock:
	clk_disable_unprepare(phy1->clk);
assert_bus_reset:
	reset_control_assert(data->phy1_bus_reset);
disable_bus_clock:
	clk_disable_unprepare(phy1->clk2);
	return ret;
}

static void sun4i_usb_phy1_clock_disable(struct sun4i_usb_phy_data *data)
{
	struct sun4i_usb_phy *phy1 = &data->phys[1];

	reset_control_assert(phy1->reset);
	clk_disable_unprepare(phy1->clk);
	reset_control_assert(data->phy1_bus_reset);
	clk_disable_unprepare(phy1->clk2);
}

static void sun4i_usb_phy0_reroute(struct sun4i_usb_phy_data *data, int id_det);

static int sun4i_usb_phy_init(struct phy *_phy)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	int ret;
	u32 val;

	if (!phy->index && data->role_switch &&
	    data->dr_mode == USB_DR_MODE_UNKNOWN)
		return -EAGAIN;

	/* OHCI1 can be the first consumer, before EHCI1 or PHY0 probes. */
	if (data->cfg->needs_phy1_clock && phy->index == 1) {
		ret = sun4i_usb_phy1_clock_enable(data);
		if (ret)
			return ret;
		goto clocks_ready;
	}

	ret = clk_prepare_enable(phy->clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(phy->clk2);
	if (ret) {
		clk_disable_unprepare(phy->clk);
		return ret;
	}

	ret = reset_control_deassert(phy->reset);
	if (ret) {
		clk_disable_unprepare(phy->clk2);
		clk_disable_unprepare(phy->clk);
		return ret;
	}

clocks_ready:
	if (data->cfg->needs_phy1_clock && phy->index == 0) {
		ret = sun4i_usb_phy1_clock_enable(data);
		if (ret) {
			reset_control_assert(phy->reset);
			clk_disable_unprepare(phy->clk2);
			clk_disable_unprepare(phy->clk);
			return ret;
		}
		/* Stock waits again once all four USB clocks are enabled. */
		udelay(10);
	}

	/* Some PHYs on some SoCs need the help of PHY2 to work. */
	if (data->cfg->needs_phy2_siddq && phy->index != 2) {
		struct sun4i_usb_phy *phy2 = &data->phys[2];

		ret = clk_prepare_enable(phy2->clk);
		if (ret) {
			reset_control_assert(phy->reset);
			clk_disable_unprepare(phy->clk2);
			clk_disable_unprepare(phy->clk);
			return ret;
		}

		ret = reset_control_deassert(phy2->reset);
		if (ret) {
			clk_disable_unprepare(phy2->clk);
			reset_control_assert(phy->reset);
			clk_disable_unprepare(phy->clk2);
			clk_disable_unprepare(phy->clk);
			return ret;
		}

		/*
		 * This extra clock is just needed to access the
		 * REG_HCI_PHY_CTL PMU register for PHY2.
		 */
		ret = clk_prepare_enable(phy2->clk2);
		if (ret) {
			reset_control_assert(phy2->reset);
			clk_disable_unprepare(phy2->clk);
			reset_control_assert(phy->reset);
			clk_disable_unprepare(phy->clk2);
			clk_disable_unprepare(phy->clk);
			return ret;
		}

		if (phy2->pmu && data->cfg->hci_phy_ctl_clear) {
			val = readl(phy2->pmu + REG_HCI_PHY_CTL);
			val &= ~data->cfg->hci_phy_ctl_clear;
			writel(val, phy2->pmu + REG_HCI_PHY_CTL);
		}

		clk_disable_unprepare(phy->clk2);
	}

	if (phy->pmu && data->cfg->hci_phy_ctl_clear) {
		val = readl(phy->pmu + REG_HCI_PHY_CTL);
		val &= ~data->cfg->hci_phy_ctl_clear;
		writel(val, phy->pmu + REG_HCI_PHY_CTL);
	}

	if (data->cfg->siddq_in_base) {
		if (phy->index == 0) {
			val = readl(data->base + data->cfg->phyctl_offset);
			val &= ~data->cfg->phy0_ctl_init_clear;
			val |= PHY_CTL_VBUSVLDEXT;
			val &= ~PHY_CTL_SIDDQ;
			writel(val, data->base + data->cfg->phyctl_offset);
		}
	} else {
		/* Enable USB 45 Ohm resistor calibration */
		if (phy->index == 0)
			sun4i_usb_phy_write(phy, PHY_RES45_CAL_EN, 0x01, 1);

		/* Adjust PHY's magnitude and rate */
		sun4i_usb_phy_write(phy, PHY_TX_AMPLITUDE_TUNE, 0x14, 5);

		/* Disconnect threshold adjustment */
		sun4i_usb_phy_write(phy, PHY_DISCON_TH_SEL,
				    data->cfg->disc_thresh, 2);
	}

	sun4i_usb_phy_passby(phy, !data->role_switch || phy->index ||
			     data->dr_mode == USB_DR_MODE_HOST);

	if (phy->index == 0) {
		data->phy0_init = true;

		/* Enable pull-ups */
		sun4i_usb_phy0_update_iscr(_phy, 0, ISCR_DPDM_PULLUP_EN);
		sun4i_usb_phy0_update_iscr(_phy, 0, ISCR_ID_PULLUP_EN);

		/* Board diagnostic quirk, recovered from the stock USB init. */
		if (data->cfg->phy0_ctl_clear) {
			/* Match stock ordering before clearing the control bit. */
			sun4i_usb_phy0_set_id_detect(_phy,
				data->dr_mode != USB_DR_MODE_HOST);
			sun4i_usb_phy0_set_vbus_detect(_phy, 1);
			sun4i_usb_phy0_reroute(data,
				data->dr_mode != USB_DR_MODE_HOST);
			val = readl(data->base + data->cfg->phyctl_offset);
			writel(val & ~data->cfg->phy0_ctl_clear,
			       data->base + data->cfg->phyctl_offset);
			dev_info(&_phy->dev, "stock PHY0 control: %#x -> %#x\n",
				 val, readl(data->base + data->cfg->phyctl_offset));
		}
		if (data->cfg->needs_phy1_clock) {
			void __iomem *phy1_ctl = data->phys[1].pmu + REG_HCI_PHY_CTL;

			/* Stock device_enable/resume clears PHY1 SIDDQ after
			 * PHY0 setup. Clocks alone leave this analog block off.
			 */
			val = readl(phy1_ctl);
			writel(val & ~data->cfg->hci_phy_ctl_clear, phy1_ctl);
			dev_info(&_phy->dev, "stock PHY1 control: %#x -> %#x\n",
				 val, readl(phy1_ctl));
		}

		/* Force ISCR and cable state updates */
		data->id_det = -1;
		data->vbus_det = -1;
		if (!data->role_switch)
			queue_delayed_work(system_percpu_wq, &data->detect, 0);
	}

	return 0;
}

static int sun4i_usb_phy_exit(struct phy *_phy)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);

	if (phy->index == 0) {
		if (data->cfg->siddq_in_base) {
			void __iomem *phyctl = data->base +
				data->cfg->phyctl_offset;

			writel(readl(phyctl) | PHY_CTL_SIDDQ, phyctl);
		}

		/* Disable pull-ups */
		sun4i_usb_phy0_update_iscr(_phy, ISCR_DPDM_PULLUP_EN, 0);
		sun4i_usb_phy0_update_iscr(_phy, ISCR_ID_PULLUP_EN, 0);
		data->phy0_init = false;
	}

	if (data->cfg->needs_phy2_siddq && phy->index != 2) {
		struct sun4i_usb_phy *phy2 = &data->phys[2];

		clk_disable_unprepare(phy2->clk);
		reset_control_assert(phy2->reset);
	}

	sun4i_usb_phy_passby(phy, 0);
	if (data->cfg->needs_phy1_clock && phy->index == 1) {
		sun4i_usb_phy1_clock_disable(data);
		return 0;
	}
	reset_control_assert(phy->reset);
	clk_disable_unprepare(phy->clk2);
	clk_disable_unprepare(phy->clk);

	if (data->cfg->needs_phy1_clock && phy->index == 0)
		sun4i_usb_phy1_clock_disable(data);

	return 0;
}

static int sun4i_usb_phy0_get_id_det(struct sun4i_usb_phy_data *data)
{
	switch (data->dr_mode) {
	case USB_DR_MODE_OTG:
		if (data->id_det_gpio)
			return gpiod_get_value_cansleep(data->id_det_gpio);
		else
			return 1; /* Fallback to peripheral mode */
	case USB_DR_MODE_HOST:
		return 0;
	case USB_DR_MODE_PERIPHERAL:
	default:
		return 1;
	}
}

static int sun4i_usb_phy0_get_vbus_det(struct sun4i_usb_phy_data *data)
{
	if (data->vbus_det_gpio)
		return gpiod_get_value_cansleep(data->vbus_det_gpio);

	if (data->vbus_power_supply) {
		union power_supply_propval val;
		int r;

		r = power_supply_get_property(data->vbus_power_supply,
					      POWER_SUPPLY_PROP_PRESENT, &val);
		if (r == 0)
			return val.intval;
	}

	/* Fallback: report vbus as high */
	return 1;
}

static bool sun4i_usb_phy0_have_vbus_det(struct sun4i_usb_phy_data *data)
{
	return data->vbus_det_gpio || data->vbus_power_supply;
}

static bool sun4i_usb_phy0_poll(struct sun4i_usb_phy_data *data)
{
	if ((data->id_det_gpio && data->id_det_irq <= 0) ||
	    (data->vbus_det_gpio && data->vbus_det_irq <= 0))
		return true;

	/*
	 * The A31/A23/A33 companion pmics (AXP221/AXP223) do not
	 * generate vbus change interrupts when the board is driving
	 * vbus using the N_VBUSEN pin on the pmic, so we must poll
	 * when using the pmic for vbus-det _and_ we're driving vbus.
	 */
	if (data->cfg->poll_vbusen && data->vbus_power_supply &&
	    data->phys[0].regulator_on)
		return true;

	return false;
}

static int sun4i_usb_phy_power_on(struct phy *_phy)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	int ret;

	if (!phy->index && data->role_switch &&
	    data->dr_mode != USB_DR_MODE_HOST)
		return 0;

	if (phy->vbus_error)
		return phy->vbus_error;

	if (data->cfg->needs_phy1_clock && !data->role_switch &&
	    phy->index == 0 && data->dr_mode == USB_DR_MODE_HOST) {
		ret = gpiod_get_value_cansleep(data->id_det_gpio);
		if (ret)
			return ret < 0 ? ret : 0;
	}

	if (!phy->vbus || phy->regulator_on)
		return 0;

	/* For phy0 only turn on Vbus if we don't have an ext. Vbus */
	if (phy->index == 0 && sun4i_usb_phy0_have_vbus_det(data) &&
				data->vbus_det) {
		dev_warn(&_phy->dev, "External vbus detected, not enabling our own vbus\n");
		return 0;
	}

	ret = regulator_enable(phy->vbus);
	if (ret) {
		if (data->cfg->needs_phy1_clock)
			phy->vbus_error = ret;
		return ret;
	}

	phy->regulator_on = true;

	/* We must report Vbus high within OTG_TIME_A_WAIT_VRISE msec. */
	if (phy->index == 0 && sun4i_usb_phy0_poll(data))
		mod_delayed_work(system_percpu_wq, &data->detect, DEBOUNCE_TIME);

	return 0;
}

static int sun4i_usb_phy_power_off(struct phy *_phy)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	int ret;

	if (phy->vbus_error)
		return phy->vbus_error;

	if (!phy->vbus || !phy->regulator_on)
		return 0;

	ret = regulator_disable(phy->vbus);
	if (ret && data->cfg->needs_phy1_clock) {
		/* A parent failure may already have consumed our child vote.
		 * Do not retry or enable again with unknown regulator ownership.
		 */
		phy->vbus_error = ret;
		return ret;
	}
	phy->regulator_on = false;

	/*
	 * phy0 vbus typically slowly discharges, sometimes this causes the
	 * Vbus gpio to not trigger an edge irq on Vbus off, so force a rescan.
	 */
	if (phy->index == 0 && !data->role_switch &&
	    !sun4i_usb_phy0_poll(data))
		mod_delayed_work(system_percpu_wq, &data->detect, POLL_TIME);

	return 0;
}

/* The fixed host keeps its HCDs bound; the ID input only gates source power. */
static int sun4i_usb_phy_host_vbus(struct phy *phy0)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(phy0);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	int id, ret;

	id = gpiod_get_value_cansleep(data->id_det_gpio);
	if (id || !phy0->power_count) {
		ret = sun4i_usb_phy_power_off(phy0);
		return ret ? ret : (id < 0 ? id : 0);
	}

	return sun4i_usb_phy_power_on(phy0);
}

static int sun4i_usb_phy_set_mode(struct phy *_phy,
				  enum phy_mode mode, int submode)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	int new_mode;

	if (phy->index != 0) {
		if (mode == PHY_MODE_USB_HOST)
			return 0;
		return -EINVAL;
	}

	switch (mode) {
	case PHY_MODE_USB_HOST:
		new_mode = USB_DR_MODE_HOST;
		break;
	case PHY_MODE_USB_DEVICE:
		new_mode = USB_DR_MODE_PERIPHERAL;
		break;
	case PHY_MODE_USB_OTG:
		new_mode = USB_DR_MODE_OTG;
		break;
	default:
		return -EINVAL;
	}

	/* The Brick profiles have one fixed controller owner per boot. */
	if (data->cfg->needs_phy1_clock) {
		if (new_mode != data->dr_mode)
			return -EOPNOTSUPP;
		if (new_mode == USB_DR_MODE_HOST || data->role_switch)
			return 0;
	}

	if (new_mode != data->dr_mode) {
		dev_info(&_phy->dev, "Changing dr_mode to %d\n", new_mode);
		data->dr_mode = new_mode;
	}

	data->id_det = -1; /* Force reprocessing of id */
	data->force_session_end = true;
	queue_delayed_work(system_percpu_wq, &data->detect, 0);

	return 0;
}

/* Commit policy only with the bottom controllers fully quiesced. The next
 * init applies routing with clocks held; this operation does no off-clock MMIO.
 */
int sun4i_usb_phy_set_role(struct phy *_phy, enum usb_role role)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);
	struct sun4i_usb_phy_data *data = to_sun4i_usb_phy_data(phy);
	int ret = 0;

	if (phy->index || !data->role_switch)
		return -EOPNOTSUPP;
	if (role != USB_ROLE_NONE && role != USB_ROLE_DEVICE && role != USB_ROLE_HOST)
		return -EINVAL;
	mutex_lock(&_phy->mutex);
	if (_phy->init_count || _phy->power_count || data->phy0_init)
		ret = -EBUSY;
	else if (phy->vbus_error || phy->regulator_on)
		ret = phy->vbus_error ? phy->vbus_error : -EBUSY;
	else
		data->dr_mode = role == USB_ROLE_HOST ? USB_DR_MODE_HOST :
			role == USB_ROLE_DEVICE ? USB_DR_MODE_PERIPHERAL :
			USB_DR_MODE_UNKNOWN;
	mutex_unlock(&_phy->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(sun4i_usb_phy_set_role);

void sun4i_usb_phy_set_squelch_detect(struct phy *_phy, bool enabled)
{
	struct sun4i_usb_phy *phy = phy_get_drvdata(_phy);

	sun4i_usb_phy_write(phy, PHY_SQUELCH_DETECT, enabled ? 0 : 2, 2);
}
EXPORT_SYMBOL_GPL(sun4i_usb_phy_set_squelch_detect);

static const struct phy_ops sun4i_usb_phy_ops = {
	.init		= sun4i_usb_phy_init,
	.exit		= sun4i_usb_phy_exit,
	.power_on	= sun4i_usb_phy_power_on,
	.power_off	= sun4i_usb_phy_power_off,
	.set_mode	= sun4i_usb_phy_set_mode,
	.owner		= THIS_MODULE,
};

static void sun4i_usb_phy0_reroute(struct sun4i_usb_phy_data *data, int id_det)
{
	u32 regval;

	regval = readl(data->base + REG_PHY_OTGCTL);
	if (id_det == 0) {
		/* Host mode. Route phy0 to EHCI/OHCI */
		regval &= ~OTGCTL_ROUTE_MUSB;
	} else {
		/* Peripheral mode. Route phy0 to MUSB */
		regval |= OTGCTL_ROUTE_MUSB;
	}
	writel(regval, data->base + REG_PHY_OTGCTL);
}

static void sun4i_usb_phy0_id_vbus_det_scan(struct work_struct *work)
{
	struct sun4i_usb_phy_data *data =
		container_of(work, struct sun4i_usb_phy_data, detect.work);
	struct phy *phy0 = data->phys[0].phy;
	struct sun4i_usb_phy *phy;
	bool force_session_end, id_notify = false, vbus_notify = false;
	int id_det, vbus_det;

	if (!phy0 || data->role_switch)
		return;

	phy = phy_get_drvdata(phy0);
	id_det = sun4i_usb_phy0_get_id_det(data);
	vbus_det = sun4i_usb_phy0_get_vbus_det(data);

	mutex_lock(&phy0->mutex);

	if (!data->phy0_init) {
		mutex_unlock(&phy0->mutex);
		return;
	}

	if (data->cfg->needs_phy1_clock && data->dr_mode == USB_DR_MODE_HOST) {
		int ret = sun4i_usb_phy_host_vbus(phy0);

		mutex_unlock(&phy0->mutex);
		if (ret)
			dev_err_ratelimited(&phy0->dev, "Host VBUS update failed: %d\n", ret);
		if (sun4i_usb_phy0_poll(data))
			queue_delayed_work(system_percpu_wq, &data->detect, POLL_TIME);
		return;
	}

	force_session_end = data->force_session_end;
	data->force_session_end = false;

	if (id_det != data->id_det) {
		/* id-change, force session end if we've no vbus detection */
		if (data->dr_mode == USB_DR_MODE_OTG &&
		    !sun4i_usb_phy0_have_vbus_det(data))
			force_session_end = true;

		/* When entering host mode (id = 0) force end the session now */
		if (force_session_end && id_det == 0) {
			sun4i_usb_phy0_set_vbus_detect(phy0, 0);
			msleep(200);
			sun4i_usb_phy0_set_vbus_detect(phy0, 1);
		}
		sun4i_usb_phy0_set_id_detect(phy0, id_det);
		data->id_det = id_det;
		id_notify = true;
	}

	if (vbus_det != data->vbus_det) {
		sun4i_usb_phy0_set_vbus_detect(phy0, vbus_det);
		data->vbus_det = vbus_det;
		vbus_notify = true;
	}

	mutex_unlock(&phy0->mutex);

	if (id_notify) {
		extcon_set_state_sync(data->extcon, EXTCON_USB_HOST,
					!id_det);
		/* When leaving host mode force end the session here */
		if (force_session_end && id_det == 1) {
			mutex_lock(&phy0->mutex);
			sun4i_usb_phy0_set_vbus_detect(phy0, 0);
			msleep(1000);
			sun4i_usb_phy0_set_vbus_detect(phy0, 1);
			mutex_unlock(&phy0->mutex);
		}

		/* Enable PHY0 passby for host mode only. */
		sun4i_usb_phy_passby(phy, !id_det);

		/* Re-route PHY0 if necessary */
		if (data->cfg->phy0_dual_route)
			sun4i_usb_phy0_reroute(data, id_det);
	}

	if (vbus_notify)
		extcon_set_state_sync(data->extcon, EXTCON_USB, vbus_det);

	if (sun4i_usb_phy0_poll(data))
		queue_delayed_work(system_percpu_wq, &data->detect, POLL_TIME);
}

static irqreturn_t sun4i_usb_phy0_id_vbus_det_irq(int irq, void *dev_id)
{
	struct sun4i_usb_phy_data *data = dev_id;

	/* vbus or id changed, let the pins settle and then scan them */
	mod_delayed_work(system_percpu_wq, &data->detect, DEBOUNCE_TIME);

	return IRQ_HANDLED;
}

static int sun4i_usb_phy0_vbus_notify(struct notifier_block *nb,
				      unsigned long val, void *v)
{
	struct sun4i_usb_phy_data *data =
		container_of(nb, struct sun4i_usb_phy_data, vbus_power_nb);
	struct power_supply *psy = v;

	/* Properties on the vbus_power_supply changed, scan vbus_det */
	if (val == PSY_EVENT_PROP_CHANGED && psy == data->vbus_power_supply)
		mod_delayed_work(system_percpu_wq, &data->detect, DEBOUNCE_TIME);

	return NOTIFY_OK;
}

static struct phy *sun4i_usb_phy_xlate(struct device *dev,
					const struct of_phandle_args *args)
{
	struct sun4i_usb_phy_data *data = dev_get_drvdata(dev);

	if (args->args[0] >= data->num_phys)
		return ERR_PTR(-ENODEV);

	if (data->cfg->missing_phys & BIT(args->args[0]))
		return ERR_PTR(-ENODEV);

	return data->phys[args->args[0]].phy;
}

static void sun4i_usb_phy_shutdown(struct platform_device *pdev)
{
	struct sun4i_usb_phy_data *data = dev_get_drvdata(&pdev->dev);
	int i, ret;

	if (!data->cfg->needs_phy1_clock)
		return;

	/* HCD shutdown quiesces transfers but does not drop its PHY power vote. */
	for (i = 0; i < data->num_phys; i++) {
		struct phy *phy = data->phys[i].phy;

		if (!i && data->dr_mode != USB_DR_MODE_HOST)
			continue;
		mutex_lock(&phy->mutex);
		if (!i)
			data->phy0_init = false;
		ret = sun4i_usb_phy_power_off(phy);
		mutex_unlock(&phy->mutex);
		if (ret)
			dev_err(&pdev->dev,
				"Cannot disable USB%d VBUS at shutdown: %d\n", i, ret);
	}
	cancel_delayed_work_sync(&data->detect);
}

static void sun4i_usb_phy_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun4i_usb_phy_data *data = dev_get_drvdata(dev);

	if (data->vbus_power_nb_registered)
		power_supply_unreg_notifier(&data->vbus_power_nb);
	if (data->id_det_irq > 0)
		devm_free_irq(dev, data->id_det_irq, data);
	if (data->vbus_det_irq > 0)
		devm_free_irq(dev, data->vbus_det_irq, data);

	cancel_delayed_work_sync(&data->detect);
}

static const unsigned int sun4i_usb_phy0_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static int sun4i_usb_phy_probe(struct platform_device *pdev)
{
	struct sun4i_usb_phy_data *data;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct phy_provider *phy_provider;
	int i, ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	spin_lock_init(&data->reg_lock);
	INIT_DELAYED_WORK(&data->detect, sun4i_usb_phy0_id_vbus_det_scan);
	dev_set_drvdata(dev, data);
	data->cfg = of_device_get_match_data(dev);
	if (!data->cfg)
		return -EINVAL;

	data->base = devm_platform_ioremap_resource_byname(pdev, "phy_ctrl");
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	data->id_det_gpio = devm_gpiod_get_optional(dev, "usb0_id_det",
						    GPIOD_IN);
	if (IS_ERR(data->id_det_gpio)) {
		dev_err(dev, "Couldn't request ID GPIO\n");
		return PTR_ERR(data->id_det_gpio);
	}

	data->vbus_det_gpio = devm_gpiod_get_optional(dev, "usb0_vbus_det",
						      GPIOD_IN);
	if (IS_ERR(data->vbus_det_gpio)) {
		dev_err(dev, "Couldn't request VBUS detect GPIO\n");
		return PTR_ERR(data->vbus_det_gpio);
	}

	if (of_property_present(np, "usb0_vbus_power-supply")) {
		data->vbus_power_supply = devm_power_supply_get_by_reference(dev,
						     "usb0_vbus_power-supply");
		if (IS_ERR(data->vbus_power_supply)) {
			dev_err(dev, "Couldn't get the VBUS power supply\n");
			return PTR_ERR(data->vbus_power_supply);
		}

		if (!data->vbus_power_supply)
			return -EPROBE_DEFER;
	}

	data->role_switch = of_property_read_bool(np, "usb-role-switch");
	if (data->role_switch && !data->cfg->needs_phy1_clock)
		return -EINVAL;
	data->dr_mode = of_usb_get_dr_mode_by_phy(np, 0);
	if (data->role_switch)
		data->dr_mode = USB_DR_MODE_UNKNOWN;
	if (data->cfg->needs_phy1_clock && !data->role_switch &&
	    ((data->dr_mode != USB_DR_MODE_PERIPHERAL &&
	      data->dr_mode != USB_DR_MODE_HOST) ||
	     data->vbus_det_gpio || data->vbus_power_supply))
		return dev_err_probe(dev, -EINVAL,
				     "Brick PHY requires a fixed role without VBUS detection\n");
	data->extcon = devm_extcon_dev_allocate(dev, sun4i_usb_phy0_cable);
	if (IS_ERR(data->extcon)) {
		dev_err(dev, "Couldn't allocate our extcon device\n");
		return PTR_ERR(data->extcon);
	}

	ret = devm_extcon_dev_register(dev, data->extcon);
	if (ret) {
		dev_err(dev, "failed to register extcon: %d\n", ret);
		return ret;
	}

	for (i = 0; i < MAX_PHYS; i++) {
		struct sun4i_usb_phy *phy = data->phys + i;
		char name[32];

		if (data->cfg->missing_phys & BIT(i))
			continue;

		snprintf(name, sizeof(name), "usb%d_reset", i);
		if (data->cfg->needs_phy1_clock && i == 1)
			phy->reset = devm_reset_control_get_shared(dev, name);
		else
			phy->reset = devm_reset_control_get(dev, name);
		if (IS_ERR(phy->reset)) {
			if (PTR_ERR(phy->reset) == -ENOENT)
				break;
			dev_err(dev, "failed to get reset %s\n", name);
			return PTR_ERR(phy->reset);
		}

		snprintf(name, sizeof(name), "usb%d_vbus", i);
		phy->vbus = devm_regulator_get_optional(dev, name);
		if (IS_ERR(phy->vbus)) {
			if (PTR_ERR(phy->vbus) == -EPROBE_DEFER) {
				dev_err(dev,
					"Couldn't get regulator %s... Deferring probe\n",
					name);
				return -EPROBE_DEFER;
			}

			phy->vbus = NULL;
		}

		if (data->cfg->dedicated_clocks)
			snprintf(name, sizeof(name), "usb%d_phy", i);
		else
			strscpy(name, "usb_phy", sizeof(name));

		phy->clk = devm_clk_get(dev, name);
		if (IS_ERR(phy->clk)) {
			dev_err(dev, "failed to get clock %s\n", name);
			return PTR_ERR(phy->clk);
		}

		/* The first PHY is always tied to OTG, and never HSIC */
		if (data->cfg->hsic_index && i == data->cfg->hsic_index) {
			/* HSIC needs secondary clock */
			snprintf(name, sizeof(name), "usb%d_hsic_12M", i);
			phy->clk2 = devm_clk_get(dev, name);
			if (IS_ERR(phy->clk2)) {
				dev_err(dev, "failed to get clock %s\n", name);
				return PTR_ERR(phy->clk2);
			}
		} else {
			snprintf(name, sizeof(name), "pmu%d_clk", i);
			phy->clk2 = devm_clk_get_optional(dev, name);
			if (IS_ERR(phy->clk2)) {
				dev_err(dev, "failed to get clock %s\n", name);
				return PTR_ERR(phy->clk2);
			}
		}

		if (i || data->cfg->phy0_dual_route) { /* No pmu for musb */
			snprintf(name, sizeof(name), "pmu%d", i);
			phy->pmu = devm_platform_ioremap_resource_byname(pdev, name);
			if (IS_ERR(phy->pmu))
				return PTR_ERR(phy->pmu);
		}

		phy->phy = devm_phy_create(dev, NULL, &sun4i_usb_phy_ops);
		if (IS_ERR(phy->phy)) {
			dev_err(dev, "failed to create PHY %d\n", i);
			return PTR_ERR(phy->phy);
		}

		phy->index = i;
		phy_set_drvdata(phy->phy, &data->phys[i]);
	}
	data->num_phys = i;
	if (data->role_switch && (!data->phys[0].vbus || data->id_det_gpio ||
				 data->vbus_det_gpio || data->vbus_power_supply))
		return dev_err_probe(dev, -EINVAL,
			"Coordinated PHY requires VBUS and external detection ownership\n");

	if (data->cfg->needs_phy1_clock && data->dr_mode == USB_DR_MODE_HOST &&
	    (!data->phys[0].vbus || !data->id_det_gpio))
		return dev_err_probe(dev, -EINVAL,
				     "Brick host requires VBUS supply and ID input\n");
	if (data->cfg->needs_phy1_clock) {
		if (data->num_phys < 2 || !data->phys[1].clk2)
			return dev_err_probe(dev, -EINVAL,
					     "Brick PHY requires PHY1 and pmu1_clk\n");
		data->phy1_bus_reset = devm_reset_control_get_shared(dev,
								"usb1_bus_reset");
		if (IS_ERR(data->phy1_bus_reset))
			return dev_err_probe(dev, PTR_ERR(data->phy1_bus_reset),
					     "Missing Brick PHY1 bus reset\n");
	}

	data->id_det_irq = gpiod_to_irq(data->id_det_gpio);
	if (data->id_det_irq > 0) {
		ret = devm_request_irq(dev, data->id_det_irq,
				sun4i_usb_phy0_id_vbus_det_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				"usb0-id-det", data);
		if (ret) {
			dev_err(dev, "Err requesting id-det-irq: %d\n", ret);
			return ret;
		}
	}

	data->vbus_det_irq = gpiod_to_irq(data->vbus_det_gpio);
	if (data->vbus_det_irq > 0) {
		ret = devm_request_irq(dev, data->vbus_det_irq,
				sun4i_usb_phy0_id_vbus_det_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				"usb0-vbus-det", data);
		if (ret) {
			dev_err(dev, "Err requesting vbus-det-irq: %d\n", ret);
			data->vbus_det_irq = -1;
			sun4i_usb_phy_remove(pdev); /* Stop detect work */
			return ret;
		}
	}

	if (data->vbus_power_supply) {
		data->vbus_power_nb.notifier_call = sun4i_usb_phy0_vbus_notify;
		data->vbus_power_nb.priority = 0;
		ret = power_supply_reg_notifier(&data->vbus_power_nb);
		if (ret) {
			sun4i_usb_phy_remove(pdev); /* Stop detect work */
			return ret;
		}
		data->vbus_power_nb_registered = true;
	}

	phy_provider = devm_of_phy_provider_register(dev, sun4i_usb_phy_xlate);
	if (IS_ERR(phy_provider)) {
		sun4i_usb_phy_remove(pdev); /* Stop detect work */
		return PTR_ERR(phy_provider);
	}

	dev_dbg(dev, "successfully loaded\n");

	return 0;
}

static const struct sun4i_usb_phy_cfg suniv_f1c100s_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A10,
	.dedicated_clocks = true,
};

static const struct sun4i_usb_phy_cfg sun4i_a10_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A10,
	.dedicated_clocks = false,
};

static const struct sun4i_usb_phy_cfg sun5i_a13_cfg = {
	.disc_thresh = 2,
	.phyctl_offset = REG_PHYCTL_A10,
	.dedicated_clocks = false,
};

static const struct sun4i_usb_phy_cfg sun6i_a31_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A10,
	.dedicated_clocks = true,
	.poll_vbusen = true,
};

static const struct sun4i_usb_phy_cfg sun7i_a20_cfg = {
	.disc_thresh = 2,
	.phyctl_offset = REG_PHYCTL_A10,
	.dedicated_clocks = false,
};

static const struct sun4i_usb_phy_cfg sun8i_a23_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A10,
	.dedicated_clocks = true,
	.poll_vbusen = true,
};

static const struct sun4i_usb_phy_cfg sun8i_a33_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.poll_vbusen = true,
};

static const struct sun4i_usb_phy_cfg sun8i_a83t_cfg = {
	.hsic_index = 2,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.siddq_in_base = true,
};

static const struct sun4i_usb_phy_cfg sun8i_h3_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.hci_phy_ctl_clear = PHY_CTL_H3_SIDDQ,
	.phy0_dual_route = true,
};

static const struct sun4i_usb_phy_cfg sun8i_r40_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.hci_phy_ctl_clear = PHY_CTL_H3_SIDDQ,
	.phy0_dual_route = true,
};

static const struct sun4i_usb_phy_cfg sun8i_v3s_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.hci_phy_ctl_clear = PHY_CTL_H3_SIDDQ,
	.phy0_dual_route = true,
};

static const struct sun4i_usb_phy_cfg sun20i_d1_cfg = {
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.hci_phy_ctl_clear = PHY_CTL_SIDDQ,
	.phy0_dual_route = true,
	.siddq_in_base = true,
};

/* Experimental Brick-only variant: bit 1 meaning is not yet documented.
 * Stock sunxi_udc_bsp_init calls USBC_PHY_Clear_Ctl(base, 1), clearing
 * USB base + 0x410 bit 1. Do not apply this to other D1-compatible boards.
 */
static const struct sun4i_usb_phy_cfg trimui_brick_cfg = {
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.hci_phy_ctl_clear = PHY_CTL_SIDDQ,
	.phy0_dual_route = true,
	.siddq_in_base = true,
	.phy0_ctl_clear = BIT(1),
	.phy0_ctl_init_clear = BIT(7), /* Stock open_usb_clock operation. */
	.needs_phy1_clock = true,
};

static const struct sun4i_usb_phy_cfg sun50i_a64_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.hci_phy_ctl_clear = PHY_CTL_H3_SIDDQ,
	.phy0_dual_route = true,
};

static const struct sun4i_usb_phy_cfg sun50i_h6_cfg = {
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.phy0_dual_route = true,
	.missing_phys = BIT(1) | BIT(2),
	.siddq_in_base = true,
};

static const struct sun4i_usb_phy_cfg sun50i_h616_cfg = {
	.disc_thresh = 3,
	.phyctl_offset = REG_PHYCTL_A33,
	.dedicated_clocks = true,
	.phy0_dual_route = true,
	.hci_phy_ctl_clear = PHY_CTL_SIDDQ,
	.needs_phy2_siddq = true,
	.siddq_in_base = true,
};

static const struct of_device_id sun4i_usb_phy_of_match[] = {
	{ .compatible = "trimui,brick-usb-phy", .data = &trimui_brick_cfg },
	{ .compatible = "allwinner,sun4i-a10-usb-phy", .data = &sun4i_a10_cfg },
	{ .compatible = "allwinner,sun5i-a13-usb-phy", .data = &sun5i_a13_cfg },
	{ .compatible = "allwinner,sun6i-a31-usb-phy", .data = &sun6i_a31_cfg },
	{ .compatible = "allwinner,sun7i-a20-usb-phy", .data = &sun7i_a20_cfg },
	{ .compatible = "allwinner,sun8i-a23-usb-phy", .data = &sun8i_a23_cfg },
	{ .compatible = "allwinner,sun8i-a33-usb-phy", .data = &sun8i_a33_cfg },
	{ .compatible = "allwinner,sun8i-a83t-usb-phy", .data = &sun8i_a83t_cfg },
	{ .compatible = "allwinner,sun8i-h3-usb-phy", .data = &sun8i_h3_cfg },
	{ .compatible = "allwinner,sun8i-r40-usb-phy", .data = &sun8i_r40_cfg },
	{ .compatible = "allwinner,sun8i-v3s-usb-phy", .data = &sun8i_v3s_cfg },
	{ .compatible = "allwinner,sun20i-d1-usb-phy", .data = &sun20i_d1_cfg },
	{ .compatible = "allwinner,sun50i-a64-usb-phy",
	  .data = &sun50i_a64_cfg},
	{ .compatible = "allwinner,sun50i-h6-usb-phy", .data = &sun50i_h6_cfg },
	{ .compatible = "allwinner,sun50i-h616-usb-phy", .data = &sun50i_h616_cfg },
	{ .compatible = "allwinner,suniv-f1c100s-usb-phy",
	  .data = &suniv_f1c100s_cfg },
	{ },
};
MODULE_DEVICE_TABLE(of, sun4i_usb_phy_of_match);

static struct platform_driver sun4i_usb_phy_driver = {
	.probe = sun4i_usb_phy_probe,
	.remove = sun4i_usb_phy_remove,
	.shutdown = sun4i_usb_phy_shutdown,
	.driver = {
		.of_match_table= sun4i_usb_phy_of_match,
		.name = "sun4i-usb-phy",
	}
};
module_platform_driver(sun4i_usb_phy_driver);

MODULE_DESCRIPTION("Allwinner sun4i USB phy driver");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL v2");

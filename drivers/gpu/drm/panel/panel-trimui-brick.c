// SPDX-License-Identifier: GPL-2.0
/* TrimUI Brick JD9168 panel. See Documentation/arch/arm64/trimui-brick.rst for wiring. */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <video/of_display_timing.h>

struct brick_cmd { u8 command; u8 count; u8 data[64]; };
struct brick_desc { const struct brick_cmd *commands; size_t count; };
#include "panel-trimui-brick-tables.h"
struct brick_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct brick_desc *desc;
	struct drm_display_mode mode;
	struct regulator *power0, *power1;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	bool backlight_attempted;
	bool commands_allowed;
	struct pinctrl *pins;
	struct pinctrl_state *active, *sleep;
	bool power0_on, power1_on, pins_active, gpio_output;
};
static struct brick_panel *to_brick(struct drm_panel *panel)
{
	return container_of(panel, struct brick_panel, panel);
}
static int brick_power_off(struct brick_panel *p)
{
	int ret = 0, err;

	/* After any cleanup attempt, retries are GPIO/regulator-only. Shared
	 * rails may stay physically powered after our references are dropped.
	 */
	p->commands_allowed = false;
	/* A failed backlight enable may still have changed PWM/GPIO state. */
	if (p->backlight_attempted) {
		err = backlight_disable(p->backlight);
		if (err) ret = err;
		else p->backlight_attempted = false;
	}
	/* Track successful transitions: cleanup is safe to retry after errors. */
	if (p->pins_active) {
		err = pinctrl_select_state(p->pins, p->sleep);
		if (err) ret = err;
		else p->pins_active = false;
		msleep(20);
	}
	if (!p->gpio_output) {
		err = gpiod_direction_output_raw(p->reset, 0);
		if (err) { if (!ret) ret = err; }
		else p->gpio_output = true;
	} else {
		gpiod_set_raw_value_cansleep(p->reset, 0);
	}
	msleep(5);
	if (p->power1_on) {
		err = regulator_disable(p->power1);
		if (err) { if (!ret) ret = err; }
		else p->power1_on = false;
		msleep(5);
	}
	if (p->power0_on) {
		err = regulator_disable(p->power0);
		if (err) { if (!ret) ret = err; }
		else p->power0_on = false;
	}
	return ret;
}
static int brick_prepare(struct drm_panel *panel)
{
	struct brick_panel *p = to_brick(panel);
	int ret;
	size_t i;

	if (p->backlight_attempted || p->power0_on || p->power1_on || p->pins_active) {
		ret = brick_power_off(p);
		if (ret) return ret;
	}
	ret = regulator_enable(p->power0);
	if (ret) { dev_err(&p->dsi->dev, "panel power0 enable failed: %d\n", ret); return ret; }
	p->power0_on = true;
	msleep(10);
	ret = regulator_enable(p->power1);
	if (ret) goto power_off;
	p->power1_on = true;
	msleep(10);
	ret = pinctrl_select_state(p->pins, p->active);
	if (ret) goto power_off;
	p->pins_active = true;
	msleep(50);
	ret = gpiod_direction_output_raw(p->reset, 1);
	if (ret) goto power_off;
	p->gpio_output = true;
	msleep(20);
	gpiod_set_raw_value_cansleep(p->reset, 0);
	msleep(120);
	gpiod_set_raw_value_cansleep(p->reset, 1);
	msleep(20);
	/* Vendor open-flow adds 10ms after power-on. */
	msleep(10);
	for (i = 0; i < p->desc->count; i++) {
		const struct brick_cmd *c = &p->desc->commands[i];
		if (c->count == 254) { msleep(c->data[0]); continue; }
		if (c->count > sizeof(c->data)) { ret = -EINVAL; goto power_off; }
		ret = mipi_dsi_dcs_write(p->dsi, c->command, c->data, c->count);
		if (ret < 0) { dev_err(&p->dsi->dev, "panel command %zu (0x%02x) failed: %d\n", i, c->command, ret); goto power_off; }
		if (ret != c->count + 1) { dev_err(&p->dsi->dev, "panel command %zu short transfer: %d/%u\n", i, ret, c->count + 1); ret = -EIO; goto power_off; }
	}
	msleep(60);
	p->commands_allowed = true;
	return 0;
power_off:
	dev_err(&p->dsi->dev, "panel prepare failed: %d\n", ret);
	{
		int cleanup = brick_power_off(p);
		if (cleanup) dev_err(&p->dsi->dev, "cleanup failed: %d (original %d)\n", cleanup, ret);
	}
	return ret;
}
static int brick_enable(struct drm_panel *panel)
{
	struct brick_panel *p = to_brick(panel);

	/* Host starts HS transmission before this callback. No DCS here. */
	if (!p->commands_allowed || !p->power0_on || !p->power1_on || !p->pins_active)
		return -EIO;
	msleep(50);
	p->backlight_attempted = true;
	{
		int ret = backlight_enable(p->backlight);
		if (ret) dev_err(&p->dsi->dev, "panel backlight enable failed: %d\n", ret);
		return ret;
	}
}
static int brick_disable(struct drm_panel *panel)
{
	struct brick_panel *p = to_brick(panel);
	int ret;

	if (!p->backlight_attempted) return 0;
	ret = backlight_disable(p->backlight);
	if (!ret) p->backlight_attempted = false;
	return ret;
}
static int brick_unprepare(struct drm_panel *panel)
{
	struct brick_panel *p = to_brick(panel);
	int ret, err;

	ret = brick_disable(panel);
	if (ret) dev_warn(&p->dsi->dev, "backlight disable failed: %d; continuing reset/cleanup\n", ret);
	/* A cleanup retry must not transmit commands to already unpowered rails. */
	if (p->commands_allowed) {
		p->commands_allowed = false;
		ret = mipi_dsi_dcs_set_display_off(p->dsi);
		msleep(25);
		err = mipi_dsi_dcs_enter_sleep_mode(p->dsi);
		if (ret < 0 || err < 0)
			dev_warn(&p->dsi->dev, "panel sleep commands failed: %d/%d; powering off\n", ret, err);
		msleep(150);
		msleep(200);
	}
	ret = brick_power_off(p);
	msleep(500);
	/* Successful physical poweroff means unprepared, even if DCS failed. */
	return ret;
}
static int brick_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct brick_panel *p = to_brick(panel);
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &p->mode);
	if (!mode) return -ENOMEM;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	return 1;
}
static const struct drm_panel_funcs brick_funcs = {
	.prepare = brick_prepare, .enable = brick_enable, .disable = brick_disable,
	.unprepare = brick_unprepare, .get_modes = brick_get_modes,
};
static int brick_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct brick_panel *p;
	u32 lanes, bus_flags;
	int ret;

	p = devm_drm_panel_alloc(dev, struct brick_panel, panel, &brick_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(p)) return PTR_ERR(p);
	p->dsi = dsi;
	p->desc = device_get_match_data(dev);
	if (!p->desc) return -EINVAL;
	ret = of_get_drm_display_mode(dev->of_node, &p->mode, &bus_flags, OF_USE_NATIVE_MODE);
	if (ret) return dev_err_probe(dev, ret, "explicit verified display timing required\n");
	if (p->mode.clock <= 0 || p->mode.hdisplay <= 0 || p->mode.vdisplay <= 0)
		return -EINVAL;
	ret = device_property_read_u32(dev, "dsi-lanes", &lanes);
	if (ret || lanes != 4) return -EINVAL;
	if (!device_property_present(dev, "power0-supply") ||
	    !device_property_present(dev, "power1-supply") ||
	    !device_property_present(dev, "backlight"))
		return -EINVAL;
	p->power0 = devm_regulator_get(dev, "power0");
	if (IS_ERR(p->power0)) return dev_err_probe(dev, PTR_ERR(p->power0), "power0\n");
	p->power1 = devm_regulator_get(dev, "power1");
	if (IS_ERR(p->power1)) return dev_err_probe(dev, PTR_ERR(p->power1), "power1\n");
	p->reset = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(p->reset)) return dev_err_probe(dev, PTR_ERR(p->reset), "reset\n");
	p->pins = devm_pinctrl_get(dev);
	if (IS_ERR(p->pins)) return PTR_ERR(p->pins);
	p->active = pinctrl_lookup_state(p->pins, "active");
	if (IS_ERR(p->active)) return PTR_ERR(p->active);
	p->sleep = pinctrl_lookup_state(p->pins, "sleep");
	if (IS_ERR(p->sleep)) return PTR_ERR(p->sleep);
	/* Own backlight errors in our callbacks: the core helper logs errors
	 * after marking a panel enabled. Keep panel.backlight NULL here.
	 */
	p->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(p->backlight)) return PTR_ERR(p->backlight);
	if (!p->backlight) return -ENODEV;
	p->panel.prepare_prev_first = true;
	dsi->lanes = lanes;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO;
	mipi_dsi_set_drvdata(dsi, p);
	drm_panel_add(&p->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret) drm_panel_remove(&p->panel);
	return ret;
}
static void brick_shutdown(struct mipi_dsi_device *dsi)
{
	struct brick_panel *p = mipi_dsi_get_drvdata(dsi);
	int ret;

	/* Host shutdown order is not guaranteed; no DCS during removal. */
	p->commands_allowed = false;

	if (p->panel.enabled) drm_panel_disable(&p->panel);
	if (p->panel.prepared) drm_panel_unprepare(&p->panel);
	ret = 0;
	/* Failed prepare may have left resources on while core prepared=false. */
	if (p->backlight_attempted || p->power0_on || p->power1_on || p->pins_active)
		ret = brick_power_off(p);
	if (ret) dev_err(&dsi->dev, "panel shutdown failed: %d\n", ret);
}
static void brick_remove(struct mipi_dsi_device *dsi)
{
	struct brick_panel *p = mipi_dsi_get_drvdata(dsi);
	brick_shutdown(dsi);
	mipi_dsi_detach(dsi);
	drm_panel_remove(&p->panel);
}
/* Failed prepare can retain private resources without panel.prepared being
 * set. Validate these after the DRM master's normal suspend phase as well.
 */
static int brick_suspend_late(struct device *dev)
{
	struct brick_panel *p = dev_get_drvdata(dev);

	if (p->panel.enabled || p->panel.prepared || p->backlight_attempted ||
	    p->commands_allowed || p->power0_on || p->power1_on || p->pins_active) {
		dev_err(dev, "panel resources did not quiesce for suspend\n");
		return -EBUSY;
	}
	return 0;
}

static const struct dev_pm_ops brick_pm_ops = {
	.suspend_late = brick_suspend_late,
};

static const struct of_device_id brick_of_match[] = {
	{ .compatible = "trimui,brick-jd9168", .data = &jd9168_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, brick_of_match);
static struct mipi_dsi_driver brick_driver = {
	.probe = brick_probe, .remove = brick_remove, .shutdown = brick_shutdown,
	.driver = { .name = "panel-trimui-brick", .of_match_table = brick_of_match,
		.pm = &brick_pm_ops },
};
module_mipi_dsi_driver(brick_driver);
MODULE_DESCRIPTION("TrimUI Brick JD9168 panel");
MODULE_LICENSE("GPL");

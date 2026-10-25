/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 Hans de Goede <hdegoede@redhat.com>
 */

#ifndef PHY_SUN4I_USB_H_
#define PHY_SUN4I_USB_H_

#include "phy.h"
#include <linux/usb/role.h>

/**
 * sun4i_usb_phy_set_squelch_detect() - Enable/disable squelch detect
 * @phy: reference to a sun4i usb phy
 * @enabled: whether to enable or disable squelch detect
 */
void sun4i_usb_phy_set_squelch_detect(struct phy *phy, bool enabled);

/* Checked bottom-port role commit after all controller PHY votes are gone. */
int sun4i_usb_phy_set_role(struct phy *phy, enum usb_role role);

#endif

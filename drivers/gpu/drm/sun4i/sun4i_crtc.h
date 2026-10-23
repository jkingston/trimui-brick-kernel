/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Free Electrons
 * Copyright (C) 2015 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#ifndef _SUN4I_CRTC_H_
#define _SUN4I_CRTC_H_

struct sun4i_crtc {
	struct drm_crtc			crtc;
	struct drm_pending_vblank_event	*event;

	struct sunxi_engine		*engine;
	struct sun4i_tcon		*tcon;

	/* Installed at component bind, before drm_dev_register. The provider
	 * outlives this CRTC through master shutdown/mode_config_cleanup.
	 * Vblank timer callbacks must not traverse mutable encoder lists.
	 */
	int (*external_vblank)(void *data, bool enable);
	void *external_vblank_data;
};

static inline struct sun4i_crtc *drm_crtc_to_sun4i_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct sun4i_crtc, crtc);
}

void sun4i_tcon_external_vblank(struct sun4i_crtc *scrtc);

struct sun4i_crtc *sun4i_crtc_init(struct drm_device *drm,
				   struct sunxi_engine *engine,
				   struct sun4i_tcon *tcon);

#endif /* _SUN4I_CRTC_H_ */

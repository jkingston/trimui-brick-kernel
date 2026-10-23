# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_DRM_IMG_ROGUE) += img-rogue/
obj-$(CONFIG_DRM_IMG_BRICK_DC_METADATA) += brick_dc_metadata.o
CFLAGS_brick_dc_metadata.o += -include $(src)/img-rogue/config_kernel.h -I$(src) -I$(src)/img-rogue/include -I$(src)/img-rogue/services/include -I$(src)/img-rogue/services/server/include -I$(src)/img-rogue/include/public -I$(src)/img-rogue/hwdefs/rogue/km

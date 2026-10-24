/*
 * Firmware I/O implementation for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/firmware.h>
#include <linux/unaligned.h>

#include "xradio.h"
#include "fwio.h"
#include "hwio.h"
#include "bh.h"
#include "sdio.h"

/* Macroses are local. */
#define APB_WRITE(reg, val) \
	do { \
		ret = xradio_apb_write_32(hw_priv, APB_ADDR(reg), (val)); \
		if (ret < 0) { \
			dev_err(hw_priv->pdev, \
				"%s: can't write %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define APB_READ(reg, val) \
	do { \
		ret = xradio_apb_read_32(hw_priv, APB_ADDR(reg), &(val)); \
		if (ret < 0) { \
			dev_err(hw_priv->pdev, \
				"%s: can't read %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define REG_WRITE(reg, val) \
	do { \
		ret = xradio_reg_write_32(hw_priv, (reg), (val)); \
		if (ret < 0) { \
			dev_err(hw_priv->pdev, \
				"%s: can't write %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define REG_READ(reg, val) \
	do { \
		ret = xradio_reg_read_32(hw_priv, (reg), &(val)); \
		if (ret < 0) { \
			dev_err(hw_priv->pdev, \
				"%s: can't read %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)


static int xradio_get_hw_type(u32 config_reg_val, int *major_revision)
{
	int hw_type  = -1;
	u32 hif_type = (config_reg_val >> 24) & 0x4;
	//u32 hif_vers = (config_reg_val >> 31) & 0x1;

	/* Check if we have XRADIO*/
	if (hif_type == 0x4) {
		*major_revision = 0x4;
		hw_type = HIF_HW_TYPE_XRADIO;
	} else {
		//hw type unknown.
		*major_revision = 0x0;
	}
	return hw_type;
}

/*
 * This function is called to Parse the SDD file
 * to extract some informations
 */
/* XR829 stock SDD is TLV. Reject truncated/unknown clock configuration. */
static int xradio_parse_sdd(struct xradio_common *hw, u32 *dpll)
{
 size_t off = 0;
 bool ref = false;
 int ret = request_firmware(&hw->sdd, XR829_SDD_FILE, hw->pdev);
 if (ret) return ret;
 hw->is_BT_Present = false;
 while (off < hw->sdd->size) {
  const u8 *e = hw->sdd->data + off;
  if (hw->sdd->size - off < 2 || e[1] > hw->sdd->size - off - 2)
   return -EINVAL;
  if (e[0] == SDD_REFERENCE_FREQUENCY_ELT_ID) {
   if (e[1] < 2 || get_unaligned_le16(e + 2) != 24000)
    return -EINVAL;
   *dpll = 0x141; /* vendor table: 24MHz crystal */
   ref = true;
  } else if (e[0] == SDD_PTA_CFG_ELT_ID) {
   if (e[1] < 4) return -EINVAL;
   hw->conf_listen_interval = (get_unaligned_le16(e + 4) >> 7) & 0x1f;
   hw->is_BT_Present = true;
  }
  off += 2 + e[1];
 }
 if (!ref) return -EINVAL;
 dev_info(hw->pdev, "XR829 SDD validated: 24MHz, PTA=%u\n", hw->is_BT_Present);
 return 0;
}

/* Stock XR829 NEW_VERSION power-control arbitration, with checked I/O. */
static int xr829_sync_dpll(struct xradio_common *hw, u32 dpll)
{
 u32 want = (((dpll >> 31) & 1) | (((dpll >> 29) & 3) << 1) |
             (((dpll >> 12) & 1) << 3) | (((dpll >> 13) & 0xffff) << 4) |
             (((dpll >> 4) & 0xff) << 20));
 u32 common, actual;
 int ret, i;
 u32 start;
 ret = xradio_ahb_read_32(hw, 0x0ac80018, &start);
 if (ret) return ret;
 dev_info(hw->pdev, "DPLL preflight: want=%#x start=%#x\n", want, start);
 for (i = 0; i <= 100; i++) {
  ret = xradio_ahb_read_32(hw, 0x0ac80178, &common);
  if (ret) return ret;
  if (i == 0 || i == 100) {
   ret = xradio_ahb_read_32(hw, 0x0ac80154, &actual);
   if (ret) return ret;
   dev_info(hw->pdev, "DPLL arbitration step=%d common=%#x actual=%#x\n", i, common, actual);
  }
  if (common & 1) {
   ret = xradio_ahb_read_32(hw, 0x0ac80154, &actual);
   if (ret) return ret;
   if (actual == want) break;
  }
  if ((common & 8) && !(common & 4)) {
   ret = xradio_ahb_read_32(hw, 0x0ac80154, &actual);
   if (ret) return ret;
   if (actual != want) {
    ret = xradio_ahb_write_32(hw, 0x0ac80154, want | 1);
    if (ret) return ret;
    msleep(5);
   }
   ret = xradio_ahb_write_32(hw, 0x0ac80178, 1);
   if (ret) return ret;
   break;
  }
  if (i == 100) return -ETIMEDOUT;
  msleep(i);
 }
 return xradio_ahb_write_32(hw, 0x0ac80018, 0);
}

static int xradio_firmware(struct xradio_common *hw_priv)
{
	int ret, block, num_blocks;
	unsigned i;
	u32 val32;
	u32 put = 0, get = 0;
	u8 *buf = NULL;
	const char *fw_path;
	const struct firmware *firmware = NULL;

	switch (hw_priv->hw_revision) {
	case XR829_HW_REV0:
		fw_path = XR829_FIRMWARE;
		break;
	default:
		dev_dbg(hw_priv->pdev, "invalid silicon revision %d.\n",
				hw_priv->hw_revision);
		return -EINVAL;
	}
	/* Load a firmware file */
	ret = request_firmware(&firmware, fw_path, hw_priv->pdev);
	if (ret) {
		dev_dbg(hw_priv->pdev, "can't load firmware file %s.\n",
				fw_path);
		goto error;
	}
	if (firmware->size != 175384) {
  ret = -EINVAL;
  goto error;
 }

	/* Initialize common registers */
	APB_WRITE(DOWNLOAD_IMAGE_SIZE_REG, DOWNLOAD_ARE_YOU_HERE);
	APB_WRITE(DOWNLOAD_PUT_REG, 0);
	APB_WRITE(DOWNLOAD_GET_REG, 0);
	APB_WRITE(DOWNLOAD_STATUS_REG, DOWNLOAD_PENDING);
	APB_WRITE(DOWNLOAD_FLAGS_REG, 0);

	/* Release CPU from RESET */
	REG_READ(HIF_CONFIG_REG_ID, val32);
	val32 &= ~HIF_CONFIG_CPU_RESET_BIT;
	REG_WRITE(HIF_CONFIG_REG_ID, val32);

	/* Enable Clock */
	val32 &= ~HIF_CONFIG_CPU_CLK_DIS_BIT;
	REG_WRITE(HIF_CONFIG_REG_ID, val32);


	buf = kmalloc(DOWNLOAD_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		dev_dbg(hw_priv->pdev, "can't allocate firmware buffer.\n");
		ret = -ENOMEM;
		goto error;
	}

	/* Check if the bootloader is ready */
	for (i = 0; i < 100; i++/*= 1 + i / 2*/) {
		APB_READ(DOWNLOAD_IMAGE_SIZE_REG, val32);
		if (val32 == DOWNLOAD_I_AM_HERE)
			break;
		mdelay(10);
	} /* End of for loop */
	if (val32 != DOWNLOAD_I_AM_HERE) {
		dev_dbg(hw_priv->pdev, "bootloader is not ready.\n");
		ret = -ETIMEDOUT;
		goto error;
	}

	/* Calculcate number of download blocks */
	num_blocks = (firmware->size - 1) / DOWNLOAD_BLOCK_SIZE + 1;

	/* Updating the length in Download Ctrl Area */
	val32 = firmware->size; /* Explicit cast from size_t to u32 */
	APB_WRITE(DOWNLOAD_IMAGE_SIZE_REG, val32);

	/* Firmware downloading loop */
	for (block = 0; block < num_blocks ; block++) {
		size_t tx_size;
		size_t block_size;

		/* check the download status */
		APB_READ(DOWNLOAD_STATUS_REG, val32);
		if (val32 != DOWNLOAD_PENDING) {
			dev_dbg(hw_priv->pdev, "bootloader reported error %d.\n",
					val32);
			ret = -EIO;
			goto error;
		}

		/* loop until put - get <= 24K */
		for (i = 0; i < 100; i++) {
			APB_READ(DOWNLOAD_GET_REG, get);
			if ((put - get) <= (DOWNLOAD_FIFO_SIZE - DOWNLOAD_BLOCK_SIZE))
				break;
			mdelay(i);
		}

		if ((put - get) > (DOWNLOAD_FIFO_SIZE - DOWNLOAD_BLOCK_SIZE)) {
			dev_dbg(hw_priv->pdev, "Timeout waiting for FIFO.\n");
			ret = -ETIMEDOUT;
			goto error;
		}

		/* calculate the block size */
		tx_size = block_size = min((size_t)(firmware->size - put), (size_t)DOWNLOAD_BLOCK_SIZE);
		memcpy(buf, &firmware->data[put], block_size);

		if (block_size < DOWNLOAD_BLOCK_SIZE) {
			memset(&buf[block_size], 0, DOWNLOAD_BLOCK_SIZE - block_size);
			tx_size = DOWNLOAD_BLOCK_SIZE;
		}

		/* send the block to sram */
		ret = xradio_apb_write(hw_priv, APB_ADDR(DOWNLOAD_FIFO_OFFSET + (put & (DOWNLOAD_FIFO_SIZE - 1))),
		                       buf, tx_size);
		if (ret < 0) {
			dev_err(hw_priv->pdev, "%s: can't write block at line %d.\n", __func__, __LINE__);
			goto error;
		}

		/* update the put register */
		put += block_size;
		APB_WRITE(DOWNLOAD_PUT_REG, put);
	} /* End of firmware download loop */

	/* Wait for the download completion */
	for (i = 0; i < 300; i += 1 + i / 2) {
		APB_READ(DOWNLOAD_STATUS_REG, val32);
		if (val32 != DOWNLOAD_PENDING)
			break;
		mdelay(i);
	}
	if (val32 != DOWNLOAD_SUCCESS) {
		dev_dbg(hw_priv->pdev, "wait for download completion failed. " \
		           "Read: 0x%.8X\n", val32);
		ret = -ETIMEDOUT;
		goto error;
	} else {
		dev_dbg(hw_priv->pdev, "Firmware completed.\n");
		ret = 0;
	}

error:
	if(buf)
		kfree(buf);
	if (firmware) {
		release_firmware(firmware);
	}
	return ret;
}

static int xradio_bootloader(struct xradio_common *hw_priv)
{
	const char *bl_path = XR829_BOOTLOADER;
	u32  addr = AHB_MEMORY_ADDRESS;
	int ret;
	u32 i;
	u32 *data;
	const struct firmware *bootloader = NULL;

	/* Load a bootloader file */
	ret = request_firmware(&bootloader, bl_path, hw_priv->pdev);
	if (ret) {
		dev_dbg(hw_priv->pdev, "can't load bootloader file %s.\n",
				bl_path);
		goto error;
	}

	/* Down bootloader. */
 if (bootloader->size != 2184 || bootloader->size % 4 || bootloader->size > 0x4000) {
  ret = -EINVAL;
  goto error;
 }
 data = (u32 *)bootloader->data;
	for(i = 0; i < (bootloader->size)/4; i++, addr+=4) {
		REG_WRITE(HIF_SRAM_BASE_ADDR_REG_ID, addr);
		REG_WRITE(HIF_AHB_DPORT_REG_ID,data[i]);
	}
	dev_dbg(hw_priv->pdev, "Bootloader complete\n");

error:
	if(bootloader) {
		release_firmware(bootloader);
	}
	return ret;
}

bool test_retry = false;
int xradio_load_firmware(struct xradio_common *hw_priv)
{
	int ret;
	int i;
	u32 val32;
	u16 val16;
	u32 dpll = 0;
	int major_revision;

	/* Read CONFIG Register Value - We will read 32 bits */
	ret = xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "can't read config register, err=%d.\n",
				ret);
		return ret;
	}

	//check hardware type and revision.
	hw_priv->hw_type = xradio_get_hw_type(val32, &major_revision);
	switch (hw_priv->hw_type) {
	case HIF_HW_TYPE_XRADIO:
		dev_dbg(hw_priv->pdev, "HW_TYPE_XRADIO detected.\n");
		break;
	default:
		dev_dbg(hw_priv->pdev, "Unknown hardware: %d.\n",
				hw_priv->hw_type);
		return -ENOTSUPP;
	}
	if (major_revision == 4) {
		hw_priv->hw_revision = XR829_HW_REV0;
		dev_dbg(hw_priv->pdev, "XRADIO_HW_REV 1.0 detected.\n");
	} else {
		dev_dbg(hw_priv->pdev, "Unsupported major revision %d.\n",
				major_revision);
		return -ENOTSUPP;
	}

	//load sdd file, and get config from it.
	ret = xradio_parse_sdd(hw_priv, &dpll);
	if (ret < 0)
		goto out;

	//set dpll initial value and check.
	ret = xradio_reg_write_32(hw_priv, HIF_TSET_GEN_R_W_REG_ID, dpll);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "can't write DPLL register.\n");
		goto out;
	}
	msleep(5);
	ret = xradio_reg_read_32(hw_priv, HIF_TSET_GEN_R_W_REG_ID, &val32);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "can't read DPLL register.\n");
		goto out;
	}
	if (val32 != dpll) {
		dev_dbg(hw_priv->pdev, "unable to initialise " \
		           "DPLL register. Wrote 0x%.8X, read 0x%.8X.\n",
				   dpll, val32);
		ret = -EIO;
		goto out;
	}

	/* Set wakeup bit in device */
	ret = xradio_reg_read_16(hw_priv, HIF_CONTROL_REG_ID, &val16);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "set_wakeup: can't read control register.\n");
		goto out;
	}
	ret = xradio_reg_write_16(hw_priv, HIF_CONTROL_REG_ID, val16 | HIF_CTRL_WUP_BIT);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "set_wakeup: can't write control register.\n");
		goto out;
	}

	/* Wait for wakeup */
	for (i = 0 ; i < 300 ; i += 1 + i / 2) {
		ret = xradio_reg_read_16(hw_priv, HIF_CONTROL_REG_ID, &val16);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "Wait_for_wakeup: "
			           "can't read control register.\n");
			goto out;
		}
		if (val16 & HIF_CTRL_RDY_BIT) {
			break;
		}
		msleep(i);
	}
	if ((val16 & HIF_CTRL_RDY_BIT) == 0) {
		dev_dbg(hw_priv->pdev, "Wait for wakeup:"
		           "device is not responding.\n");
		ret = -ETIMEDOUT;
		goto out;
	} else {
		dev_dbg(hw_priv->pdev, "WLAN device is ready.\n");
	}

	/* Checking for access mode and download firmware. */
	ret = xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "check_access_mode: "
		           "can't read config register.\n");
		goto out;
	}
	ret = xradio_reg_write_32(hw_priv, HIF_CONFIG_REG_ID,
                                 val32 | HIF_CONFIG_ACCESS_MODE_BIT);
 if (ret) goto out;
 ret = xr829_sync_dpll(hw_priv, dpll);
 if (ret) { dev_err(hw_priv->pdev, "DPLL sync failed: %d\n", ret); goto out; }
 if (true) {
		/* Down bootloader. */
		ret = xradio_bootloader(hw_priv);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "can't download bootloader.\n");
			goto out;
		}
		/* Down firmware. */
		ret = xradio_firmware(hw_priv);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "can't download firmware.\n");
			goto out;
		}
	} else {
		dev_dbg(hw_priv->pdev, "check_access_mode: "
		           "device is already in QUEUE mode.\n");
		/* TODO: verify this branch. Do we need something to do? */
	}

	if (HIF_HW_TYPE_XRADIO  == hw_priv->hw_type) {
		/* If device is XRADIO the IRQ enable/disable bits
		 * are in CONFIG register */
		ret = xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "enable_irq: can't read " \
			           "config register.\n");
			goto unsubscribe;
		}
		ret = xradio_reg_write_32(hw_priv, HIF_CONFIG_REG_ID,
			val32 | HIF_CONF_IRQ_RDY_ENABLE);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "enable_irq: can't write " \
			           "config register.\n");
			goto unsubscribe;
		}
	} else {
		/* If device is XRADIO the IRQ enable/disable bits
		 * are in CONTROL register */
		/* Enable device interrupts - Both DATA_RDY and WLAN_RDY */
		ret = xradio_reg_read_16(hw_priv, HIF_CONFIG_REG_ID, &val16);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "enable_irq: can't read " \
			           "control register.\n");
			goto unsubscribe;
		}
		ret = xradio_reg_write_16(hw_priv, HIF_CONFIG_REG_ID,
		                          val16 | HIF_CTRL_IRQ_RDY_ENABLE);
		if (ret < 0) {
			dev_dbg(hw_priv->pdev, "enable_irq: can't write " \
			           "control register.\n");
			goto unsubscribe;
		}

	}

	/* Configure device for MESSSAGE MODE */
	ret = xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "set_mode: can't read config register.\n");
		goto unsubscribe;
	}
	ret = xradio_reg_write_32(hw_priv, HIF_CONFIG_REG_ID,
	                          val32 & ~HIF_CONFIG_ACCESS_MODE_BIT);
	if (ret < 0) {
		dev_dbg(hw_priv->pdev, "set_mode: can't write config register.\n");
		goto unsubscribe;
	}

	/* Unless we read the CONFIG Register we are
	 * not able to get an interrupt */
	mdelay(10);
	xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
	return 0;

unsubscribe:
out:
	if (hw_priv->sdd) {
		release_firmware(hw_priv->sdd);
		hw_priv->sdd = NULL;
	}
	return ret;
}

int xradio_dev_deinit(struct xradio_common *hw_priv)
{
	if (hw_priv->sdd) {
		release_firmware(hw_priv->sdd);
		hw_priv->sdd = NULL;
	}
	return 0;
}

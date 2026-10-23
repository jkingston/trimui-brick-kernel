/*************************************************************************/ /*!
@File
@Title          System Configuration
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    System Configuration functions
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

/* Linux 7.x adaptation for A133. See PROVENANCE.txt for evidence and limits. */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "pvrsrv_device.h"
#include "syscommon.h"
#include "allocmem.h"
#include "physheap.h"
#include "rgxdevice.h"
#include "interrupt_support.h"

/* From stock GPU power-domain DT and vendor sunxi_platform.c. */
#define SUNXI_PPU_BASE 0x07001000
#define SUNXI_PPU_SIZE 0x34
#define SUNXI_PPU_STATUS 0x20
/* A100 CCU/R-CCU reset maps: bit16=reset released, bit0=bus gate. */
#define SUNXI_GPU_BGR 0x0300167c
#define SUNXI_PPU_BGR 0x0701017c
#define SUNXI_BGR_RESET_RELEASED BIT(16)
/* Vendor pm_domains.c and sunxi_platform.c PPU register definitions. */
#define SUNXI_PPU_RSTN 0x00
#define SUNXI_PPU_GATE 0x04
#define SUNXI_PPU_ISOLATION 0x08
#define SUNXI_PPU_CONTROL 0x24
#define SUNXI_PPU_IRQ_MASK 0x2c
#define SUNXI_PPU_CONTROL_MASK 0x03
#define SUNXI_PPU_CONTROL_HW 0x02
#define SUNXI_DFS_REG 0x01880020
#define SUNXI_GPU_BASE 0x01800000
#define SUNXI_GPU_SIZE 0x80000

struct sunxi_gpu {
 struct device *dev;
 struct clk *core, *bus, *ppu_clock;
 void __iomem *ppu, *dfs;
 bool core_enabled, bus_enabled, ppu_enabled;
 RGX_TIMING_INFORMATION timing;
 RGX_DATA data;
 PHYS_HEAP_CONFIG heaps[2];
 PVRSRV_DEVICE_CONFIG config;
};

/* Read only always-on CCU registers before enabling or accessing PPU/GPU.
 * A reset-default PPU status of zero does not prove the domain is powered.
 * Do not deassert resets here: vendor silicon erratum requires early-boot
 * domain/reset sequencing, before the JTAG reset on affected revisions. */
static bool inherited_resets_released(struct device *dev)
{
 void __iomem *gpu_bgr, *ppu_bgr;
 u32 gpu_value, ppu_value;
 gpu_bgr = ioremap(SUNXI_GPU_BGR, sizeof(u32));
 ppu_bgr = ioremap(SUNXI_PPU_BGR, sizeof(u32));
 if (!gpu_bgr || !ppu_bgr) {
  if (gpu_bgr) iounmap(gpu_bgr);
  if (ppu_bgr) iounmap(ppu_bgr);
  dev_err(dev, "Cannot inspect inherited GPU/PPU reset state\n");
  return false;
 }
 gpu_value = readl(gpu_bgr);
 ppu_value = readl(ppu_bgr);
 iounmap(gpu_bgr);
 iounmap(ppu_bgr);
 if (!(gpu_value & SUNXI_BGR_RESET_RELEASED) ||
     !(ppu_value & SUNXI_BGR_RESET_RELEASED)) {
  dev_err(dev, "Inherited resets not released: GPU_BGR=%#x PPU_BGR=%#x; early-boot initialization required\n",
          gpu_value, ppu_value);
  return false;
 }
 return true;
}

/* Called only after acquiring/enabling the PPU clock. Hardware mode with
 * power-request IRQ masked matches vendor ppu_mode_switch(HW, false).
 * Software mode needs the vendor PPU domain-B IRQ handler, absent here. */
static bool inherited_ppu_ready(struct sunxi_gpu *gpu)
{
 u32 rstn = readl(gpu->ppu + SUNXI_PPU_RSTN);
 u32 gate = readl(gpu->ppu + SUNXI_PPU_GATE);
 u32 isolation = readl(gpu->ppu + SUNXI_PPU_ISOLATION);
 u32 status = readl(gpu->ppu + SUNXI_PPU_STATUS);
 u32 control = readl(gpu->ppu + SUNXI_PPU_CONTROL);
 u32 irq_mask = readl(gpu->ppu + SUNXI_PPU_IRQ_MASK);
 if (!(rstn & BIT(0)) || !(gate & BIT(0)) || (isolation & 3) ||
     (status & 3) ||
     (control & SUNXI_PPU_CONTROL_MASK) != SUNXI_PPU_CONTROL_HW ||
     (irq_mask & BIT(0))) {
  dev_err(gpu->dev, "Unsupported inherited PPU state: reset=%#x gate=%#x isolation=%#x status=%#x control=%#x irqmask=%#x\n",
          rstn, gate, isolation, status, control, irq_mask);
  return false;
 }
 return true;
}

static void cpu_to_dev(IMG_HANDLE priv, IMG_UINT32 count,
 IMG_DEV_PHYADDR *dst, IMG_CPU_PHYADDR *src)
{
 IMG_UINT32 i;
 for (i = 0; i < count; i++) dst[i].uiAddr = src[i].uiAddr;
}
static void dev_to_cpu(IMG_HANDLE priv, IMG_UINT32 count,
 IMG_CPU_PHYADDR *dst, IMG_DEV_PHYADDR *src)
{
 IMG_UINT32 i;
 for (i = 0; i < count; i++) dst[i].uiAddr = src[i].uiAddr;
}
static PHYS_HEAP_FUNCTIONS heap_funcs = {
 .pfnCpuPAddrToDevPAddr = cpu_to_dev,
 .pfnDevPAddrToCpuPAddr = dev_to_cpu,
};

/* Keep the boot-established domain on. Vendor source documents an A133
 * JTAG/GPU-reset ordering erratum; no speculative resets are performed here.
 * Firmware manages its internal power islands. System suspend is refused
 * until a separately validated domain/clock restore sequence exists. */
static PVRSRV_ERROR sunxi_pre_power(IMG_HANDLE handle,
 PVRSRV_SYS_POWER_STATE next, PVRSRV_SYS_POWER_STATE old_state,
 PVRSRV_POWER_FLAGS flags)
{
 struct sunxi_gpu *gpu = handle;
 if (flags & PVRSRV_POWER_FLAGS_SUSPEND_REQ) {
  dev_err(gpu->dev, "A133 GPU suspend has not been validated\n");
  return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
 }
 if (next == PVRSRV_SYS_POWER_STATE_ON) {
  if ((readl(gpu->ppu + SUNXI_PPU_STATUS) & 3) != 0)
   return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
  /* Preserve boot-selected DFS instead of changing GPU frequency. */
  if (!clk_get_rate(gpu->core))
   return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
 }
 return PVRSRV_OK;
}
static PVRSRV_ERROR sunxi_post_power(IMG_HANDLE handle,
 PVRSRV_SYS_POWER_STATE next, PVRSRV_SYS_POWER_STATE old_state,
 PVRSRV_POWER_FLAGS flags)
{
 /* No domain gate: see erratum above. Driver-level firmware power is real. */
 return PVRSRV_OK;
}
static void feature_init(PVRSRV_DEVICE_CONFIG *config, IMG_UINT64 features)
{
 config->eCacheSnoopingMode = PVRSRV_DEVICE_SNOOP_NONE;
}

static void release_platform(struct sunxi_gpu *gpu)
{
 if (gpu->core_enabled) clk_disable_unprepare(gpu->core);
 if (gpu->bus_enabled) clk_disable_unprepare(gpu->bus);
 if (gpu->ppu_enabled) clk_disable_unprepare(gpu->ppu_clock);
 if (!IS_ERR_OR_NULL(gpu->core)) clk_put(gpu->core);
 if (!IS_ERR_OR_NULL(gpu->bus)) clk_put(gpu->bus);
 if (!IS_ERR_OR_NULL(gpu->ppu_clock)) clk_put(gpu->ppu_clock);
 if (gpu->ppu) iounmap(gpu->ppu);
 if (gpu->dfs) iounmap(gpu->dfs);
 kfree(gpu);
}

PVRSRV_ERROR SysDevInit(void *osdev, PVRSRV_DEVICE_CONFIG **result)
{
 struct device *dev = osdev;
 struct platform_device *pdev = to_platform_device(dev);
 struct sunxi_gpu *gpu;
 struct resource *res;
 int irq, ret;
 unsigned long core_rate;
 u32 dfs;

 *result = NULL;
 if (!of_machine_is_compatible("allwinner,sun50i-a133") &&
     !of_machine_is_compatible("allwinner,a133"))
  return PVRSRV_ERROR_INVALID_DEVICE;
 res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
 if (!res || res->start != SUNXI_GPU_BASE || resource_size(res) != SUNXI_GPU_SIZE)
  return PVRSRV_ERROR_INVALID_DEVICE;
 if (!inherited_resets_released(dev))
  return PVRSRV_ERROR_INIT_FAILURE;
 irq = platform_get_irq_byname(pdev, "IRQGPU");
 if (irq < 0) return PVRSRV_ERROR_INIT_FAILURE;
 /* Stock SysDevInit+0x44 loads 0x0000ffffffffffff, not the1.11 mask32. */
 if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48)))
  return PVRSRV_ERROR_INIT_FAILURE;
 gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
 if (!gpu) return PVRSRV_ERROR_OUT_OF_MEMORY;
 gpu->dev = dev;
 gpu->core = of_clk_get_by_name(dev->of_node, "core");
 if (IS_ERR(gpu->core)) goto fail;
 gpu->bus = of_clk_get_by_name(dev->of_node, "bus");
 if (IS_ERR(gpu->bus)) goto fail;
 gpu->ppu_clock = of_clk_get_by_name(dev->of_node, "ppu");
 if (IS_ERR(gpu->ppu_clock)) goto fail;
 ret = clk_prepare_enable(gpu->ppu_clock);
 if (ret) goto fail;
 gpu->ppu_enabled = true;
 gpu->ppu = ioremap(SUNXI_PPU_BASE, SUNXI_PPU_SIZE);
 if (!gpu->ppu) goto fail;
 if (!inherited_ppu_ready(gpu)) goto fail;
 ret = clk_prepare_enable(gpu->bus);
 if (ret) goto fail;
 gpu->bus_enabled = true;
 ret = clk_prepare_enable(gpu->core);
 if (ret) goto fail;
 gpu->core_enabled = true;
 gpu->dfs = ioremap(SUNXI_DFS_REG, sizeof(u32));
 if (!gpu->dfs) goto fail;
 core_rate = clk_get_rate(gpu->core);
 dfs = readl(gpu->dfs);
 /* DFS is one-hot: vendor formula1<<(16-freq/(core/16)). */
 if (!core_rate || !dfs || (dfs & (dfs-1)) || dfs > BIT(15)) {
  dev_err(dev, "Cannot establish inherited GPU timing: clock%lu DFS%#x\n", core_rate, dfs);
  goto fail;
 }
 gpu->timing.ui32CoreClockSpeed = (core_rate / 16) * (16 - __ffs(dfs));
 gpu->timing.bEnableActivePM = IMG_FALSE;
 gpu->timing.bEnableRDPowIsland = IMG_TRUE;
 gpu->timing.ui32ActivePMLatencyms = 100;
 gpu->data.psRGXTimingInfo = &gpu->timing;
 gpu->heaps[0].eType = PHYS_HEAP_TYPE_UMA;
 gpu->heaps[0].pszPDumpMemspaceName = "SYSMEM";
 gpu->heaps[0].psMemFuncs = &heap_funcs;
 gpu->heaps[0].ui32UsageFlags = PHYS_HEAP_USAGE_GPU_LOCAL;
 gpu->heaps[1].eType = PHYS_HEAP_TYPE_UMA;
 gpu->heaps[1].pszPDumpMemspaceName = "SYSMEM_FW";
 gpu->heaps[1].psMemFuncs = &heap_funcs;
 gpu->heaps[1].ui32UsageFlags = PHYS_HEAP_USAGE_FW_SHARED;
 gpu->config.pvOSDevice = dev;
 gpu->config.pszName = "sunxi";
 gpu->config.sRegsCpuPBase.uiAddr = res->start;
 gpu->config.ui32RegsSize = resource_size(res);
 gpu->config.ui32IRQ = irq;
 gpu->config.hDevData = &gpu->data;
 gpu->config.hSysData = gpu;
 gpu->config.eDefaultHeap = PVRSRV_PHYS_HEAP_GPU_LOCAL;
 gpu->config.pasPhysHeaps = gpu->heaps;
 gpu->config.ui32PhysHeapCount = 2;
 gpu->config.pfnPrePowerState = sunxi_pre_power;
 gpu->config.pfnPostPowerState = sunxi_post_power;
 gpu->config.pfnSysDevFeatureDepInit = feature_init;
 *result = &gpu->config;
 dev_info(dev, "A133 GPU fixed inherited clock%uHz DFS%#x; suspend unsupported\n",
  gpu->timing.ui32CoreClockSpeed, dfs);
 return PVRSRV_OK;
fail:
 release_platform(gpu);
 return PVRSRV_ERROR_INIT_FAILURE;
}
void SysDevDeInit(PVRSRV_DEVICE_CONFIG *config)
{
 release_platform(config->hSysData);
}
PVRSRV_ERROR SysInstallDeviceLISR(IMG_HANDLE sysdata, IMG_UINT32 irq,
 const IMG_CHAR *name, PFN_LISR isr, void *data, IMG_HANDLE *handle)
{
 return OSInstallSystemLISR(handle, irq, name, isr, data, SYS_IRQ_FLAG_TRIGGER_DEFAULT);
}
PVRSRV_ERROR SysUninstallDeviceLISR(IMG_HANDLE handle)
{
 return OSUninstallSystemLISR(handle);
}
PVRSRV_ERROR SysDebugInfo(PVRSRV_DEVICE_CONFIG *config,
 DUMPDEBUG_PRINTF_FUNC *printfn, void *file)
{
 return PVRSRV_OK;
}

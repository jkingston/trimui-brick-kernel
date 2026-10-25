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
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/sun50i-a133-gpu.h>
#include "pvrsrv_device.h"
#include "syscommon.h"
#include "allocmem.h"
#include "physheap.h"
#include "rgxdevice.h"
#include "interrupt_support.h"
#include "sunxi_irq_lifecycle.h"

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

/* Validate successful stop completion before any internal power removal.
 * All calls and consume are serialized by the existing DDK power mutex.
 */
enum stop_invalid {
	STOP_VALID, STOP_NO_BEGIN, STOP_MISMATCH, STOP_ORDER, STOP_POST_MISMATCH
};
struct stop_evidence {
	unsigned long long sequence;
	unsigned int next, old, flags, polls, invalid;
	int first_irq_error, stop_result;
	bool active, stop_seen;
};
static void stop_record(struct stop_evidence *e, unsigned int event,
	unsigned int next, unsigned int old, unsigned int flags, int result)
{
	if (event == 0) {
		e->sequence++;
		e->next = next; e->old = old;
		e->flags = flags;
		e->polls = 0; e->first_irq_error = 0;
		e->stop_result = 0;
		e->invalid = STOP_VALID;
		e->active = true;
		e->stop_seen = false;
		return;
	}
	if (e->invalid)
		return;
	if (!e->active) {
		e->invalid = STOP_NO_BEGIN;
		return;
	}
	if (e->next != next || e->old != old || e->flags != flags) {
		e->invalid = STOP_MISMATCH;
		return;
	}
	if (e->stop_seen || (event != 1 && event != 2)) {
		e->invalid = STOP_ORDER;
		return;
	}
	if (event == 1) {
		e->polls++;
		if (result && !e->first_irq_error)
			e->first_irq_error = result;
	} else {
		e->stop_seen = true;
		e->stop_result = result;
		if (!e->polls)
			e->invalid = STOP_ORDER;
	}
}
/* Match power.c's explicit DEV ON -> SYS ON, everything else -> SYS OFF. */
static unsigned int stop_system_state(unsigned int state)
{
	return state == PVRSRV_DEV_POWER_STATE_ON ?
		PVRSRV_SYS_POWER_STATE_ON : PVRSRV_SYS_POWER_STATE_OFF;
}
static bool stop_consume(struct stop_evidence *e, unsigned int next,
	unsigned int old, unsigned int flags)
{
	bool eligible;

	if (!e->invalid && !e->active)
		e->invalid = STOP_NO_BEGIN;
	if (!e->invalid && (stop_system_state(e->next) != next ||
	    stop_system_state(e->old) != old || e->flags != flags))
		e->invalid = STOP_POST_MISMATCH;
	eligible = e->active && !e->invalid &&
		e->next == PVRSRV_DEV_POWER_STATE_OFF && e->old != e->next &&
		e->polls && !e->first_irq_error && e->stop_seen && !e->stop_result;
	e->active = false;
	return eligible;
}

struct sunxi_gpu;
struct sunxi_irq_owner {
	struct sunxi_gpu *gpu;
	struct irq_lifecycle state;
};
enum sunxi_sleep_phase { SUNXI_AWAKE, SUNXI_SUSPENDING, SUNXI_ASLEEP,
	SUNXI_RESUMING, SUNXI_SLEEP_FAULT };
struct sunxi_gpu {
	struct mutex irq_lock;
	struct sunxi_irq_owner irq_owner;
	struct stop_evidence stop;
	bool transition_fault;
	enum sunxi_sleep_phase sleep_phase;
	bool sleep_verified;
	struct device *dev;
	struct clk *core, *bus, *ppu_clock;
	void __iomem *ppu, *dfs;
	bool core_enabled, bus_enabled, ppu_enabled;
	RGX_TIMING_INFORMATION timing;
	RGX_DATA data;
	PHYS_HEAP_CONFIG heaps[2];
	PVRSRV_DEVICE_CONFIG config;
};

/* DDK mutex -> irq_lock; LISR takes neither. Never flush MISRs here. */
static void sunxi_irq_disable(unsigned int irq) { disable_irq(irq); }
static void sunxi_irq_enable(unsigned int irq) { enable_irq(irq); }
static int sunxi_irq_remove(void *handle) { return OSUninstallSystemLISR(handle); }
static const struct irq_lifecycle_ops sunxi_irq_ops = {
	.disable_sync = sunxi_irq_disable,
	.enable = sunxi_irq_enable,
	.uninstall = sunxi_irq_remove,
};
static int sunxi_irq_quiesce(struct sunxi_gpu *gpu, bool eligible, bool forced)
{
	int ret;

	mutex_lock(&gpu->irq_lock);
	ret = irq_quiesce(&gpu->irq_owner.state, &sunxi_irq_ops,
		eligible, forced, gpu->stop.sequence);
	mutex_unlock(&gpu->irq_lock);
	return ret;
}

/* Read only always-on CCU registers before enabling or accessing PPU/GPU.
 * A reset-default PPU status of zero does not prove the domain is powered.
 * Do not deassert resets here: vendor silicon erratum requires early-boot
 * domain/reset sequencing, before the JTAG reset on affected revisions.
 */
static bool inherited_resets_released(struct device *dev)
{
	void __iomem *gpu_bgr, *ppu_bgr;
	u32 gpu_value, ppu_value;

	gpu_bgr = ioremap(SUNXI_GPU_BGR, sizeof(u32));
	ppu_bgr = ioremap(SUNXI_PPU_BGR, sizeof(u32));
	if (!gpu_bgr || !ppu_bgr) {
		if (gpu_bgr)
			iounmap(gpu_bgr);
		if (ppu_bgr)
			iounmap(ppu_bgr);
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
 * Software mode needs the vendor PPU domain-B IRQ handler, absent here.
 */
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

	for (i = 0; i < count; i++)
		dst[i].uiAddr = src[i].uiAddr;
}
static void dev_to_cpu(IMG_HANDLE priv, IMG_UINT32 count,
	IMG_CPU_PHYADDR *dst, IMG_DEV_PHYADDR *src)
{
	IMG_UINT32 i;

	for (i = 0; i < count; i++)
		dst[i].uiAddr = src[i].uiAddr;
}
static PHYS_HEAP_FUNCTIONS heap_funcs = {
	.pfnCpuPAddrToDevPAddr = cpu_to_dev,
	.pfnDevPAddrToCpuPAddr = dev_to_cpu,
};

/* External clocks and reset-controller resources remain enabled. The provider
 * owns internal PPU power removal and restores it before this wake validation.
 * s2idle retains these resources; deep sleep remains unsupported.
 * In hardware mode, firmware idle leaves
 * the PPU in HOST (status 1), with 3D isolation and the host interface clocked.
 * Accept HOST only when waking from OFF; RGXDoStart restarts the firmware.
 * ALLOFF is valid only while owned by the provider, never at this boundary.
 * Reject inconsistent reset, isolation or clock state without speculative reset.
 */
static bool sunxi_apm_wake_valid(bool was_off, unsigned int reset,
	unsigned int gate, unsigned int isolation, unsigned int status,
	unsigned int control, unsigned int irqmask, unsigned long rate,
	unsigned int dfs)
{
	if (!(reset & 1) || !(gate & 1) || (control & 3) != 2 ||
	    (irqmask & 1) || rate != A133_GPU_FIXED_RATE || dfs != 1)
		return false;
	return ((status & 3) == 0 && (isolation & 3) == 0) ||
	       (was_off && (status & 3) == 1 && (isolation & 3) == 2);
}
static void sunxi_stop_evidence(IMG_HANDLE handle, IMG_UINT32 event,
	PVRSRV_DEV_POWER_STATE next, PVRSRV_DEV_POWER_STATE old,
	PVRSRV_POWER_FLAGS flags, PVRSRV_ERROR result)
{
	struct sunxi_gpu *gpu = handle;

	stop_record(&gpu->stop, event, next, old, flags, result);
}
/* Keep the local fail gate even if provider identity has become invalid. */
static void sunxi_latch_fault(struct sunxi_gpu *gpu, int error)
{
	WRITE_ONCE(gpu->transition_fault, true);
	a133_gpu_latch_fault(gpu->dev, error);
}

static IMG_BOOL sunxi_power_fault(IMG_HANDLE handle)
{
	struct sunxi_gpu *gpu = handle;

	return (READ_ONCE(gpu->transition_fault) || a133_gpu_power_fault(gpu->dev)) ?
		IMG_TRUE : IMG_FALSE;
}
/* Commit only after the complete DDK transition, including firmware restart.
 * Failed stop/start is not reversible by replaying a platform callback: retain
 * blocked clients and the existing sticky GPU-access gate until reboot.
 */
static PVRSRV_ERROR sunxi_system_sleep(IMG_HANDLE handle, IMG_BOOL suspend,
	IMG_BOOL complete, PVRSRV_ERROR result)
{
	struct sunxi_gpu *gpu = handle;
	bool fault = false;

	mutex_lock(&gpu->irq_lock);
	if (!complete) {
		if (sunxi_power_fault(gpu) ||
		    (suspend && pm_suspend_target_state != PM_SUSPEND_TO_IDLE) ||
		    gpu->sleep_phase != (suspend ? SUNXI_AWAKE : SUNXI_ASLEEP)) {
			result = PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
		} else {
			gpu->sleep_phase = suspend ? SUNXI_SUSPENDING : SUNXI_RESUMING;
			gpu->sleep_verified = false;
		}
	} else if (result != PVRSRV_OK || !gpu->sleep_verified ||
		   gpu->sleep_phase != (suspend ? SUNXI_SUSPENDING : SUNXI_RESUMING)) {
		fault = true;
		gpu->sleep_phase = SUNXI_SLEEP_FAULT;
		if (result == PVRSRV_OK)
			result = PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
	} else {
		gpu->sleep_phase = suspend ? SUNXI_ASLEEP : SUNXI_AWAKE;
	}
	mutex_unlock(&gpu->irq_lock);
	if (fault) {
		sunxi_latch_fault(gpu, -EIO);
		mutex_lock(&gpu->irq_lock);
		if (gpu->irq_owner.state.phase == IRQ_ACTIVE) {
			disable_irq(gpu->irq_owner.state.irq);
			gpu->irq_owner.state.phase = IRQ_QUIESCED;
			gpu->irq_owner.state.stop_sequence = 0;
		}
		mutex_unlock(&gpu->irq_lock);
	}
	return result;
}

static bool sunxi_sleep_retained(struct sunxi_gpu *gpu)
{
	bool quiesced;

	if (a133_gpu_s2idle_retained(gpu->dev))
		return false;
	mutex_lock(&gpu->irq_lock);
	quiesced = gpu->irq_owner.state.phase == IRQ_QUIESCED;
	mutex_unlock(&gpu->irq_lock);
	return quiesced;
}

static PVRSRV_ERROR sunxi_pre_power(IMG_HANDLE handle,
	PVRSRV_SYS_POWER_STATE next, PVRSRV_SYS_POWER_STATE old_state,
	PVRSRV_POWER_FLAGS flags)
{
	struct sunxi_gpu *gpu = handle;

	if ((flags & PVRSRV_POWER_FLAGS_SUSPEND_REQ) &&
	    (READ_ONCE(gpu->sleep_phase) != SUNXI_SUSPENDING ||
	     next != PVRSRV_SYS_POWER_STATE_OFF))
		goto sleep_fault;
	if ((flags & PVRSRV_POWER_FLAGS_RESUME_REQ) &&
	    READ_ONCE(gpu->sleep_phase) != SUNXI_RESUMING)
		goto sleep_fault;
	if (!(flags & PVRSRV_POWER_FLAGS_RESUME_REQ) &&
	    READ_ONCE(gpu->sleep_phase) == SUNXI_ASLEEP)
		return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
	if (next == PVRSRV_SYS_POWER_STATE_ON) {
		if (a133_gpu_apm_handoff(gpu->dev, true,
	     !!(flags & PVRSRV_POWER_FLAGS_FORCED)))
			return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
		if (!sunxi_apm_wake_valid(old_state == PVRSRV_SYS_POWER_STATE_OFF,
	     readl(gpu->ppu + SUNXI_PPU_RSTN), readl(gpu->ppu + SUNXI_PPU_GATE),
	     readl(gpu->ppu + SUNXI_PPU_ISOLATION), readl(gpu->ppu + SUNXI_PPU_STATUS),
	     readl(gpu->ppu + SUNXI_PPU_CONTROL), readl(gpu->ppu + SUNXI_PPU_IRQ_MASK),
	     clk_get_rate(gpu->core), readl(gpu->dfs))) {
			sunxi_latch_fault(gpu, -EIO);
			dev_err(gpu->dev, "Unsupported GPU wake PPU/clock state\n");
			return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
		}
		/* Provider handoff and full tuple validation above establish accessibility
		 * before resuming the IRQ; a false fault query alone never does.
		 */
		mutex_lock(&gpu->irq_lock);
		{
			int ret = irq_resume(&gpu->irq_owner.state, &sunxi_irq_ops, true,
				sunxi_power_fault(gpu));
			mutex_unlock(&gpu->irq_lock);
			if (ret) {
				sunxi_latch_fault(gpu, ret);
				return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
			}
		}
	}
	return PVRSRV_OK;
sleep_fault:
	sunxi_latch_fault(gpu, -EIO);
	return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
}
static PVRSRV_ERROR sunxi_post_power(IMG_HANDLE handle,
	PVRSRV_SYS_POWER_STATE next, PVRSRV_SYS_POWER_STATE old_state,
	PVRSRV_POWER_FLAGS flags)
{
	struct sunxi_gpu *gpu = handle;
	bool eligible = false;

	/* OS requests may repeat device OFF (APM's default). Do not hand an
	 * already internally-OFF domain to the ordinary OFF transition again.
	 */
	if (flags & (PVRSRV_POWER_FLAGS_SUSPEND_REQ | PVRSRV_POWER_FLAGS_RESUME_REQ)) {
		if (next == PVRSRV_SYS_POWER_STATE_OFF) {
			eligible = stop_consume(&gpu->stop, next, old_state, flags);
			if (old_state != PVRSRV_SYS_POWER_STATE_OFF) {
				if (!eligible || a133_gpu_apm_handoff(gpu->dev, false, true) ||
				    sunxi_irq_quiesce(gpu, true, false) != 1)
					goto sleep_fault;
			}
			/* Fresh forced OFF may retain 3D/HOST. No unproven forced
			 * internal cut: only existing APM-owned OFF is preserved.
			 */
			if (!sunxi_sleep_retained(gpu))
				goto sleep_fault;
		}
		WRITE_ONCE(gpu->sleep_verified, true);
		return PVRSRV_OK;
	}

	if (next == PVRSRV_SYS_POWER_STATE_OFF)
		eligible = stop_consume(&gpu->stop, next, old_state, flags);
	if (next == PVRSRV_SYS_POWER_STATE_OFF &&
	    a133_gpu_apm_handoff(gpu->dev, false,
	     !!(flags & PVRSRV_POWER_FLAGS_FORCED)))
		return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;

	if (next == PVRSRV_SYS_POWER_STATE_OFF) {
		/* Forced initialization also stops firmware. Drain its IRQ while
		 * that stop evidence is current, before a later OFF-to-OFF request
		 * replaces it. Forced transitions still cannot cut internal power.
		 */
		int barrier = sunxi_irq_quiesce(gpu, eligible, false);
		int cut;

		if (barrier < 0) {
			sunxi_latch_fault(gpu, barrier);
			return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
		}
		if (barrier == 1 && eligible &&
	     !(flags & PVRSRV_POWER_FLAGS_FORCED)) {
			cut = a133_gpu_internal_off(gpu->dev);
			if (cut < 0) {
				sunxi_latch_fault(gpu, cut);
				return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
			}
		}
	}
	return PVRSRV_OK;
sleep_fault:
	sunxi_latch_fault(gpu, -EIO);
	return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
}
static void feature_init(PVRSRV_DEVICE_CONFIG *config, IMG_UINT64 features)
{
	config->eCacheSnoopingMode = PVRSRV_DEVICE_SNOOP_NONE;
}

static void release_platform(struct sunxi_gpu *gpu)
{
	/* Actual DDK teardown removes LISR and destroys MISR workqueues before
	 * SysDevDeInit. Init failures here occur before any LISR installation.
	 * Do not invent a teardown fallback if that lifetime invariant is broken.
	 */

	mutex_lock(&gpu->irq_lock);
	if (!irq_can_install(&gpu->irq_owner.state)) {
		dev_err(gpu->dev, "IRQ lifetime invariant failed; retaining platform resources\n");
		mutex_unlock(&gpu->irq_lock);
		return;
	}
	mutex_unlock(&gpu->irq_lock);

	if (gpu->core_enabled)
		clk_disable_unprepare(gpu->core);
	if (gpu->bus_enabled)
		clk_disable_unprepare(gpu->bus);
	if (gpu->ppu_enabled)
		clk_disable_unprepare(gpu->ppu_clock);
	if (!IS_ERR_OR_NULL(gpu->core))
		clk_put(gpu->core);
	if (!IS_ERR_OR_NULL(gpu->bus))
		clk_put(gpu->bus);
	if (!IS_ERR_OR_NULL(gpu->ppu_clock))
		clk_put(gpu->ppu_clock);
	if (gpu->ppu)
		iounmap(gpu->ppu);
	if (gpu->dfs)
		iounmap(gpu->dfs);
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
	if (irq < 0)
		return PVRSRV_ERROR_INIT_FAILURE;
	/* Stock SysDevInit+0x44 loads 0x0000ffffffffffff, not the1.11 mask32. */
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48)))
		return PVRSRV_ERROR_INIT_FAILURE;
	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	gpu->dev = dev;
	mutex_init(&gpu->irq_lock);
	gpu->irq_owner.gpu = gpu;
	gpu->core = of_clk_get_by_name(dev->of_node, "core");
	if (IS_ERR(gpu->core))
		goto fail;
	gpu->bus = of_clk_get_by_name(dev->of_node, "bus");
	if (IS_ERR(gpu->bus))
		goto fail;
	gpu->ppu_clock = of_clk_get_by_name(dev->of_node, "ppu");
	if (IS_ERR(gpu->ppu_clock))
		goto fail;
	ret = clk_prepare_enable(gpu->ppu_clock);
	if (ret)
		goto fail;
	gpu->ppu_enabled = true;
	gpu->ppu = ioremap(SUNXI_PPU_BASE, SUNXI_PPU_SIZE);
	if (!gpu->ppu)
		goto fail;
	if (!inherited_ppu_ready(gpu))
		goto fail;
	ret = clk_prepare_enable(gpu->bus);
	if (ret)
		goto fail;
	gpu->bus_enabled = true;
	ret = clk_prepare_enable(gpu->core);
	if (ret)
		goto fail;
	gpu->core_enabled = true;
	gpu->dfs = ioremap(SUNXI_DFS_REG, sizeof(u32));
	if (!gpu->dfs)
		goto fail;
	core_rate = clk_get_rate(gpu->core);
	dfs = readl(gpu->dfs);
	/* DFS is one-hot: vendor formula1<<(16-freq/(core/16)). */
	if (!core_rate || !dfs || (dfs & (dfs-1)) || dfs > BIT(15)) {
		dev_err(dev, "Cannot establish inherited GPU timing: clock%lu DFS%#x\n",
			core_rate, dfs);
		goto fail;
	}
	gpu->timing.ui32CoreClockSpeed = (core_rate / 16) * (16 - __ffs(dfs));
	gpu->timing.bEnableActivePM = IMG_TRUE;
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
	gpu->config.pfnStopEvidence = sunxi_stop_evidence;
	gpu->config.pfnGpuPowerFault = sunxi_power_fault;
	gpu->config.pfnSystemSleep = sunxi_system_sleep;
	gpu->config.pfnPrePowerState = sunxi_pre_power;
	gpu->config.pfnPostPowerState = sunxi_post_power;
	gpu->config.pfnSysDevFeatureDepInit = feature_init;
 *result = &gpu->config;
	dev_info(dev, "A133 GPU fixed inherited clock%uHz DFS%#x; s2idle retention\n",
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
	struct sunxi_gpu *gpu = sysdata;
	void *os_handle = NULL;
	PVRSRV_ERROR ret;

	if (!gpu || !handle || irq != gpu->config.ui32IRQ)
		return PVRSRV_ERROR_INVALID_PARAMS;
	*handle = NULL;

	mutex_lock(&gpu->irq_lock);
	if (!irq_can_install(&gpu->irq_owner.state)) {
		ret = PVRSRV_ERROR_INVALID_PARAMS;
		goto out;
	}
	/* IRQGPU must remain exclusive: do not add SYS_IRQ_FLAG_SHARED. */
	ret = OSInstallSystemLISR(&os_handle, irq, name, isr, data,
		SYS_IRQ_FLAG_TRIGGER_DEFAULT);
	if (ret != PVRSRV_OK)
		goto out;
	/* Under irq_lock, can_install was checked and successful OS install returns
	 * a non-NULL handle. Record ownership before publishing the wrapper.
	 */
	if (irq_installed(&gpu->irq_owner.state, irq, os_handle)) {
		OSUninstallSystemLISR(os_handle);
		ret = PVRSRV_ERROR_INVALID_PARAMS;
		goto out;
	}
	*handle = &gpu->irq_owner;
out:
	mutex_unlock(&gpu->irq_lock);
	return ret;
}
PVRSRV_ERROR SysUninstallDeviceLISR(IMG_HANDLE handle)
{
	struct sunxi_irq_owner *owner = handle;
	struct sunxi_gpu *gpu;
	int ret;

	if (!owner || !owner->gpu)
		return PVRSRV_ERROR_INVALID_PARAMS;
	gpu = owner->gpu;
	if (owner != &gpu->irq_owner)
		return PVRSRV_ERROR_INVALID_PARAMS;

	mutex_lock(&gpu->irq_lock);
	ret = irq_uninstall(&owner->state, &sunxi_irq_ops);
	mutex_unlock(&gpu->irq_lock);
	return ret < 0 ? PVRSRV_ERROR_INVALID_PARAMS : (PVRSRV_ERROR)ret;
}
PVRSRV_ERROR SysDebugInfo(PVRSRV_DEVICE_CONFIG *config,
	DUMPDEBUG_PRINTF_FUNC *printfn, void *file)
{
	return PVRSRV_OK;
}

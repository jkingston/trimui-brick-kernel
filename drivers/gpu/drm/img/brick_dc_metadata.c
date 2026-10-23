// SPDX-License-Identifier: GPL-2.0
/* Metadata-only virtual display for stock NULL_WSEGL pbuffer experiments.
 * Implements the current DDK callback contract; no display memory or hardware.
 * No vendor implementation copied. Unsupported operations return real errors.
 */
#include <linux/module.h>
#include <linux/string.h>
#include "kerneldisplay.h"
#define FORMAT_BGRA8 90 /* IMG_PIXFMT_B8G8R8A8_UNORM, stock DWARF */
static IMG_HANDLE registration;
static_assert(sizeof(DC_DEVICE_FUNCTIONS)==176);
static_assert(offsetof(DC_DEVICE_FUNCTIONS,pfnResetDevice)==168);
static void info(IMG_HANDLE h, DC_DISPLAY_INFO *o)
{
 memset(o,0,sizeof(*o));
 strscpy(o->szDisplayName,"Brick metadata-only virtual DC",sizeof(o->szDisplayName));
 o->ui32MaxDisplayPeriod=1;o->ui32MaxPipes=1;
}
static PVRSRV_ERROR count(IMG_HANDLE h, IMG_UINT32 *n)
{*n=1;return PVRSRV_OK;}
static PVRSRV_ERROR panels(IMG_HANDLE h,IMG_UINT32 size,IMG_UINT32 *n,PVRSRV_PANEL_INFO *o)
{
 *n=0;if(!size||!o)return PVRSRV_ERROR_INVALID_PARAMS;
 memset(o,0,sizeof(*o));o->sSurfaceInfo.sDims.ui32Width=64;o->sSurfaceInfo.sDims.ui32Height=64;
 o->sSurfaceInfo.sFormat.ePixFormat=FORMAT_BGRA8;
 o->sSurfaceInfo.sFormat.eMemLayout=PVRSRV_SURFACE_MEMLAYOUT_STRIDED;
 o->ui32RefreshRate=60;*n=1;return PVRSRV_OK;
}
static PVRSRV_ERROR formats(IMG_HANDLE h,IMG_UINT32 n,PVRSRV_SURFACE_FORMAT *f,IMG_UINT32 *s)
{u32 i;for(i=0;i<n;i++)s[i]=f[i].ePixFormat==FORMAT_BGRA8&&f[i].eMemLayout==PVRSRV_SURFACE_MEMLAYOUT_STRIDED;return PVRSRV_OK;}
static PVRSRV_ERROR dimensions(IMG_HANDLE h,IMG_UINT32 n,PVRSRV_SURFACE_DIMS *d,IMG_UINT32 *s)
{u32 i;for(i=0;i<n;i++)s[i]=d[i].ui32Width==64&&d[i].ui32Height==64;return PVRSRV_OK;}
static PVRSRV_ERROR context(IMG_HANDLE h,IMG_HANDLE *o)
{*o=NULL;pr_info("brick_dc_metadata: display context unsupported\n");return PVRSRV_ERROR_NOT_SUPPORTED;}
static void destroy(IMG_HANDLE h){WARN_ON_ONCE(h);}
static PVRSRV_ERROR check(IMG_HANDLE h,IMG_UINT32 n,PVRSRV_SURFACE_CONFIG_INFO *s,IMG_HANDLE *b)
{return PVRSRV_ERROR_NOT_SUPPORTED;}
static void configure(IMG_HANDLE h,IMG_UINT32 n,PVRSRV_SURFACE_CONFIG_INFO *s,IMG_HANDLE *b,IMG_UINT32 period,IMG_HANDLE config)
{WARN_ON_ONCE(1); /* No context can be created, so this must be unreachable. */}
static PVRSRV_ERROR allocate(IMG_HANDLE h,DC_BUFFER_CREATE_INFO *i,IMG_DEVMEM_LOG2ALIGN_T *p,IMG_UINT32 *n,IMG_UINT32 *stride,IMG_HANDLE *b)
{*p=0;*n=0;*stride=0;*b=NULL;pr_info("brick_dc_metadata: buffer allocation unsupported\n");return PVRSRV_ERROR_NOT_SUPPORTED;}
static PVRSRV_ERROR acquire(IMG_HANDLE h,IMG_DEV_PHYADDR *a,void **v)
{*v=NULL;return PVRSRV_ERROR_NOT_SUPPORTED;}
static void release(IMG_HANDLE h){WARN_ON_ONCE(h);}
static DC_DEVICE_FUNCTIONS functions={
 .pfnGetInfo=info,.pfnPanelQueryCount=count,.pfnPanelQuery=panels,
 .pfnFormatQuery=formats,.pfnDimQuery=dimensions,.pfnContextCreate=context,
 .pfnContextDestroy=destroy,.pfnContextConfigureCheck=check,.pfnContextConfigure=configure,
 .pfnBufferAlloc=allocate,.pfnBufferAcquire=acquire,.pfnBufferRelease=release,.pfnBufferFree=release,
};
static int __init metadata_init(void)
{
 PVRSRV_ERROR e=DCRegisterDevice(&functions,1,&registration,&registration);
 if(e!=PVRSRV_OK){pr_err("brick_dc_metadata: registration error%d\n",e);return -ENODEV;}
 __module_get(THIS_MODULE); /* Pinned: registered callbacks remain until reboot. */
 pr_info("brick_dc_metadata: registered virtual64x64 BGRA8 metadata only; no scanout/buffers\n");
 return 0;
}
module_init(metadata_init);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Metadata-only virtual DC for pbuffer bring-up");

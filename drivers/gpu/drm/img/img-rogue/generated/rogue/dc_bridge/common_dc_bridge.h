/*******************************************************************************
@File
@Title          Common bridge header for dc
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Declares common defines and structures used by both the client
                and server side of the bridge for dc
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
*******************************************************************************/

#ifndef COMMON_DC_BRIDGE_H
#define COMMON_DC_BRIDGE_H

#include <powervr/mem_types.h>

#include "img_types.h"
#include "pvrsrv_error.h"

#include "pvrsrv_surface.h"
#include "dc_external.h"
#include "pvrsrv_sync_km.h"

#define PVRSRV_BRIDGE_DC_CMD_FIRST			0
#define PVRSRV_BRIDGE_DC_DCDEVICESQUERYCOUNT			PVRSRV_BRIDGE_DC_CMD_FIRST+0
#define PVRSRV_BRIDGE_DC_DCDEVICESENUMERATE			PVRSRV_BRIDGE_DC_CMD_FIRST+1
#define PVRSRV_BRIDGE_DC_DCDEVICEACQUIRE			PVRSRV_BRIDGE_DC_CMD_FIRST+2
#define PVRSRV_BRIDGE_DC_DCDEVICERELEASE			PVRSRV_BRIDGE_DC_CMD_FIRST+3
#define PVRSRV_BRIDGE_DC_DCGETINFO			PVRSRV_BRIDGE_DC_CMD_FIRST+4
#define PVRSRV_BRIDGE_DC_DCPANELQUERYCOUNT			PVRSRV_BRIDGE_DC_CMD_FIRST+5
#define PVRSRV_BRIDGE_DC_DCPANELQUERY			PVRSRV_BRIDGE_DC_CMD_FIRST+6
#define PVRSRV_BRIDGE_DC_DCFORMATQUERY			PVRSRV_BRIDGE_DC_CMD_FIRST+7
#define PVRSRV_BRIDGE_DC_DCDIMQUERY			PVRSRV_BRIDGE_DC_CMD_FIRST+8
#define PVRSRV_BRIDGE_DC_DCSETBLANK			PVRSRV_BRIDGE_DC_CMD_FIRST+9
#define PVRSRV_BRIDGE_DC_DCSETVSYNCREPORTING			PVRSRV_BRIDGE_DC_CMD_FIRST+10
#define PVRSRV_BRIDGE_DC_DCLASTVSYNCQUERY			PVRSRV_BRIDGE_DC_CMD_FIRST+11
#define PVRSRV_BRIDGE_DC_DCSYSTEMBUFFERACQUIRE			PVRSRV_BRIDGE_DC_CMD_FIRST+12
#define PVRSRV_BRIDGE_DC_DCSYSTEMBUFFERRELEASE			PVRSRV_BRIDGE_DC_CMD_FIRST+13
#define PVRSRV_BRIDGE_DC_DCDISPLAYCONTEXTCREATE			PVRSRV_BRIDGE_DC_CMD_FIRST+14
#define PVRSRV_BRIDGE_DC_DCDISPLAYCONTEXTCONFIGURECHECK			PVRSRV_BRIDGE_DC_CMD_FIRST+15
#define PVRSRV_BRIDGE_DC_DCDISPLAYCONTEXTCONFIGURE2			PVRSRV_BRIDGE_DC_CMD_FIRST+16
#define PVRSRV_BRIDGE_DC_DCDISPLAYCONTEXTDESTROY			PVRSRV_BRIDGE_DC_CMD_FIRST+17
#define PVRSRV_BRIDGE_DC_DCBUFFERALLOC			PVRSRV_BRIDGE_DC_CMD_FIRST+18
#define PVRSRV_BRIDGE_DC_DCBUFFERIMPORT			PVRSRV_BRIDGE_DC_CMD_FIRST+19
#define PVRSRV_BRIDGE_DC_DCBUFFERFREE			PVRSRV_BRIDGE_DC_CMD_FIRST+20
#define PVRSRV_BRIDGE_DC_DCBUFFERUNIMPORT			PVRSRV_BRIDGE_DC_CMD_FIRST+21
#define PVRSRV_BRIDGE_DC_DCBUFFERPIN			PVRSRV_BRIDGE_DC_CMD_FIRST+22
#define PVRSRV_BRIDGE_DC_DCBUFFERUNPIN			PVRSRV_BRIDGE_DC_CMD_FIRST+23
#define PVRSRV_BRIDGE_DC_DCBUFFERACQUIRE			PVRSRV_BRIDGE_DC_CMD_FIRST+24
#define PVRSRV_BRIDGE_DC_DCBUFFERRELEASE			PVRSRV_BRIDGE_DC_CMD_FIRST+25
#define PVRSRV_BRIDGE_DC_CMD_LAST			(PVRSRV_BRIDGE_DC_CMD_FIRST+25)
/* Structure layouts recovered from stock1.19 DWARF; behaviour audited separately. */
typedef struct PVRSRV_BRIDGE_IN_DCDEVICESQUERYCOUNT_TAG {
    IMG_UINT32 ui32EmptyStructPlaceholder;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDEVICESQUERYCOUNT;

typedef struct PVRSRV_BRIDGE_OUT_DCDEVICESQUERYCOUNT_TAG {
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32DeviceCount;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDEVICESQUERYCOUNT;

typedef struct PVRSRV_BRIDGE_IN_DCDEVICESENUMERATE_TAG {
    IMG_UINT32 *pui32DeviceIndex;
    IMG_UINT32 ui32DeviceArraySize;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDEVICESENUMERATE;

typedef struct PVRSRV_BRIDGE_OUT_DCDEVICESENUMERATE_TAG {
    IMG_UINT32 *pui32DeviceIndex;
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32DeviceCount;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDEVICESENUMERATE;

typedef struct PVRSRV_BRIDGE_IN_DCDEVICEACQUIRE_TAG {
    IMG_UINT32 ui32DeviceIndex;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDEVICEACQUIRE;

typedef struct PVRSRV_BRIDGE_OUT_DCDEVICEACQUIRE_TAG {
    IMG_HANDLE hDevice;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDEVICEACQUIRE;

typedef struct PVRSRV_BRIDGE_IN_DCDEVICERELEASE_TAG {
    IMG_HANDLE hDevice;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDEVICERELEASE;

typedef struct PVRSRV_BRIDGE_OUT_DCDEVICERELEASE_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDEVICERELEASE;

typedef struct PVRSRV_BRIDGE_IN_DCGETINFO_TAG {
    IMG_HANDLE hDevice;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCGETINFO;

typedef struct PVRSRV_BRIDGE_OUT_DCGETINFO_TAG {
    DC_DISPLAY_INFO sDisplayInfo;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCGETINFO;

typedef struct PVRSRV_BRIDGE_IN_DCPANELQUERYCOUNT_TAG {
    IMG_HANDLE hDevice;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCPANELQUERYCOUNT;

typedef struct PVRSRV_BRIDGE_OUT_DCPANELQUERYCOUNT_TAG {
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32NumPanels;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCPANELQUERYCOUNT;

typedef struct PVRSRV_BRIDGE_IN_DCPANELQUERY_TAG {
    IMG_HANDLE hDevice;
    PVRSRV_PANEL_INFO *psPanelInfo;
    IMG_UINT32 ui32PanelsArraySize;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCPANELQUERY;

typedef struct PVRSRV_BRIDGE_OUT_DCPANELQUERY_TAG {
    PVRSRV_PANEL_INFO *psPanelInfo;
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32NumPanels;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCPANELQUERY;

typedef struct PVRSRV_BRIDGE_IN_DCFORMATQUERY_TAG {
    IMG_HANDLE hDevice;
    PVRSRV_SURFACE_FORMAT *psFormat;
    IMG_UINT32 *pui32Supported;
    IMG_UINT32 ui32NumFormats;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCFORMATQUERY;

typedef struct PVRSRV_BRIDGE_OUT_DCFORMATQUERY_TAG {
    IMG_UINT32 *pui32Supported;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCFORMATQUERY;

typedef struct PVRSRV_BRIDGE_IN_DCDIMQUERY_TAG {
    IMG_HANDLE hDevice;
    PVRSRV_SURFACE_DIMS *psDim;
    IMG_UINT32 *pui32Supported;
    IMG_UINT32 ui32NumDims;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDIMQUERY;

typedef struct PVRSRV_BRIDGE_OUT_DCDIMQUERY_TAG {
    IMG_UINT32 *pui32Supported;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDIMQUERY;

typedef struct PVRSRV_BRIDGE_IN_DCSETBLANK_TAG {
    IMG_HANDLE hDevice;
    IMG_BOOL bEnabled;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCSETBLANK;

typedef struct PVRSRV_BRIDGE_OUT_DCSETBLANK_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCSETBLANK;

typedef struct PVRSRV_BRIDGE_IN_DCSETVSYNCREPORTING_TAG {
    IMG_HANDLE hDevice;
    IMG_BOOL bEnabled;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCSETVSYNCREPORTING;

typedef struct PVRSRV_BRIDGE_OUT_DCSETVSYNCREPORTING_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCSETVSYNCREPORTING;

typedef struct PVRSRV_BRIDGE_IN_DCLASTVSYNCQUERY_TAG {
    IMG_HANDLE hDevice;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCLASTVSYNCQUERY;

typedef struct PVRSRV_BRIDGE_OUT_DCLASTVSYNCQUERY_TAG {
    IMG_INT64 i64Timestamp;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCLASTVSYNCQUERY;

typedef struct PVRSRV_BRIDGE_IN_DCSYSTEMBUFFERACQUIRE_TAG {
    IMG_HANDLE hDevice;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCSYSTEMBUFFERACQUIRE;

typedef struct PVRSRV_BRIDGE_OUT_DCSYSTEMBUFFERACQUIRE_TAG {
    IMG_HANDLE hBuffer;
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32Stride;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCSYSTEMBUFFERACQUIRE;

typedef struct PVRSRV_BRIDGE_IN_DCSYSTEMBUFFERRELEASE_TAG {
    IMG_HANDLE hBuffer;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCSYSTEMBUFFERRELEASE;

typedef struct PVRSRV_BRIDGE_OUT_DCSYSTEMBUFFERRELEASE_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCSYSTEMBUFFERRELEASE;

typedef struct PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTCREATE_TAG {
    IMG_HANDLE hDevice;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTCREATE;

typedef struct PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTCREATE_TAG {
    IMG_HANDLE hDisplayContext;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTCREATE;

typedef struct PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTCONFIGURECHECK_TAG {
    IMG_HANDLE hDisplayContext;
    PVRSRV_SURFACE_CONFIG_INFO *psSurfInfo;
    IMG_HANDLE *phBuffers;
    IMG_UINT32 ui32PipeCount;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTCONFIGURECHECK;

typedef struct PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTCONFIGURECHECK_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTCONFIGURECHECK;

typedef struct PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTDESTROY_TAG {
    IMG_HANDLE hDisplayContext;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTDESTROY;

typedef struct PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTDESTROY_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTDESTROY;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERALLOC_TAG {
    IMG_HANDLE hDisplayContext;
    DC_BUFFER_CREATE_INFO sSurfInfo;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERALLOC;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERALLOC_TAG {
    IMG_HANDLE hBuffer;
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32Stride;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERALLOC;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERIMPORT_TAG {
    IMG_HANDLE hDisplayContext;
    IMG_HANDLE *phImport;
    DC_BUFFER_IMPORT_INFO sSurfAttrib;
    IMG_UINT32 ui32NumPlanes;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERIMPORT;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERIMPORT_TAG {
    IMG_HANDLE hBuffer;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERIMPORT;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERFREE_TAG {
    IMG_HANDLE hBuffer;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERFREE;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERFREE_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERFREE;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERUNIMPORT_TAG {
    IMG_HANDLE hBuffer;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERUNIMPORT;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERUNIMPORT_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERUNIMPORT;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERPIN_TAG {
    IMG_HANDLE hBuffer;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERPIN;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERPIN_TAG {
    IMG_HANDLE hPinHandle;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERPIN;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERUNPIN_TAG {
    IMG_HANDLE hPinHandle;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERUNPIN;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERUNPIN_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERUNPIN;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERACQUIRE_TAG {
    IMG_HANDLE hBuffer;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERACQUIRE;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERACQUIRE_TAG {
    IMG_HANDLE hExtMem;
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERACQUIRE;

typedef struct PVRSRV_BRIDGE_IN_DCBUFFERRELEASE_TAG {
    IMG_HANDLE hExtMem;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCBUFFERRELEASE;

typedef struct PVRSRV_BRIDGE_OUT_DCBUFFERRELEASE_TAG {
    PVRSRV_ERROR eError;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCBUFFERRELEASE;

typedef struct PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTCONFIGURE2_TAG {
    IMG_HANDLE hDisplayContext;
    PVRSRV_SURFACE_CONFIG_INFO *psSurfInfo;
    IMG_HANDLE *phBuffers;
    PVRSRV_FENCE hAcquireFence;
    PVRSRV_TIMELINE hReleaseFenceTimeline;
    IMG_UINT32 ui32DisplayPeriod;
    IMG_UINT32 ui32MaxDepth;
    IMG_UINT32 ui32PipeCount;
} __attribute__((packed)) PVRSRV_BRIDGE_IN_DCDISPLAYCONTEXTCONFIGURE2;

typedef struct PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTCONFIGURE2_TAG {
    PVRSRV_ERROR eError;
    PVRSRV_FENCE hReleaseFence;
} __attribute__((packed)) PVRSRV_BRIDGE_OUT_DCDISPLAYCONTEXTCONFIGURE2;

#endif /* COMMON_DC_BRIDGE_H */

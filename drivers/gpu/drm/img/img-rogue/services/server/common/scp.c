/*************************************************************************/ /*!
@File           scp.c
@Title          Software Command Processor
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    The software command processor allows commands queued and
                deferred until their synchronisation requirements have been meet.
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

#include "scp.h"
#include "allocmem.h"
#include "pvr_notifier.h"
#include "pvrsrv.h"
#include "pvr_debug.h"
#include "osfunc.h"
#include "lock.h"
#include "sync_server.h"
#include "sync_internal.h"
#include "rgxhwperf.h"

#if defined(PVR_USE_FENCE_SYNC_MODEL)
#include "pvrsrv_sync_server.h"
#endif

struct _SCP_CONTEXT_
{
	PVRSRV_DEVICE_NODE  *psDevNode;         /*<! Device node reference for SCP layer */
	void				*pvCCB;	            /*!< Pointer to the command circler buffer*/
	volatile IMG_UINT32	ui32DepOffset;      /*!< Dependency offset  */
	volatile IMG_UINT32	ui32ReadOffset;     /*!< Read offset */
	volatile IMG_UINT32	ui32WriteOffset;    /*!< Write offset */
	IMG_UINT32			ui32CCBSize;        /*!< CCB size */
	IMG_UINT32 psSyncRequesterID; /* Retained in stock1.19 context layout; no server-sync operations. */
	POS_LOCK			hLock;				/*!< Lock for this structure */
};


#define SCP_COMMAND_INVALID     0   /*!< Invalid command */
#define SCP_COMMAND_CALLBACK    1   /*!< Command with callbacks */
#define SCP_COMMAND_PADDING     2   /*!< Padding */
typedef struct _SCP_COMMAND_
{
	IMG_UINT32				ui32CmdType;        /*!< Command type */
	IMG_UINT32				ui32CmdSize;        /*!< Total size of the command (i.e. includes header) */
#if defined(PVR_USE_FENCE_SYNC_MODEL)
	SYNC_FENCE_OBJ			sAcquireFenceObj;   /*!< Acquire fence (if applicable) */
	SYNC_TIMELINE_OBJ		sSWTimelineObj;     /*!< SW Timeline to be advanced on command completion */
	SYNC_FENCE_OBJ			sReleaseFenceObj;   /*!< Release fence (if applicable) */
#endif
	SCPReady				pfnReady;           /*!< Pointer to the function to check if the command is ready */
	SCPDo					pfnDo;              /*!< Pointer to the function to call when the command is ready to go */
	void					*pvReadyData;       /*!< Data to pass into pfnReady */
	void					*pvCompleteData;    /*!< Data to pass into pfnComplete */
} SCP_COMMAND;

#if defined(CONFIG_ARM64)
/* Independent oracle: stock Brick1.19 DWARF, _SCP_CONTEXT_/_SCP_COMMAND_.
 * These assertions catch accidental loss of fence fields or callback data. */
static_assert(sizeof(struct _SCP_CONTEXT_) == 48);
static_assert(offsetof(struct _SCP_CONTEXT_, hLock) == 40);
static_assert(sizeof(SCP_COMMAND) == 88);
static_assert(offsetof(SCP_COMMAND, sAcquireFenceObj) == 8);
static_assert(offsetof(SCP_COMMAND, sSWTimelineObj) == 24);
static_assert(offsetof(SCP_COMMAND, sReleaseFenceObj) == 40);
static_assert(offsetof(SCP_COMMAND, pfnReady) == 56);
static_assert(offsetof(SCP_COMMAND, pfnDo) == 64);
static_assert(offsetof(SCP_COMMAND, pvReadyData) == 72);
static_assert(offsetof(SCP_COMMAND, pvCompleteData) == 80);
#endif

#define GET_CCB_SPACE(WOff, ROff, CCBSize) \
	((((ROff) - (WOff)) + ((CCBSize) - 1)) & ((CCBSize) - 1))

#define UPDATE_CCB_OFFSET(Off, PacketSize, CCBSize) \
	(Off) = (((Off) + (PacketSize)) & ((CCBSize) - 1))

#define PADDING_COMMAND_SIZE	(sizeof(SCP_COMMAND))

#if defined(SCP_DEBUG)
#define SCP_DEBUG_PRINT(fmt, ...) \
	PVRSRVDebugPrintf(PVR_DBG_WARNING, \
					  __FILE__, __LINE__, \
					  fmt, \
					  __VA_ARGS__)
#else
#define SCP_DEBUG_PRINT(fmt, ...)
#endif

/*****************************************************************************
 *                          Internal functions                               *
 *****************************************************************************/

/*************************************************************************/ /*!
@Function       __SCPAlloc

@Description    Allocate space in the software command processor.

@Input          psContext            Context to allocate from

@Input          ui32Size                Size to allocate

@Output         ppvBufferSpace          Pointer to space allocated

@Return         PVRSRV_OK if the allocation was successful
*/
/*****************************************************************************/
static
PVRSRV_ERROR __SCPAlloc(SCP_CONTEXT *psContext,
						IMG_UINT32 ui32Size,
						void **ppvBufferSpace)
{
	IMG_UINT32 ui32FreeSpace;

	ui32FreeSpace = GET_CCB_SPACE(psContext->ui32WriteOffset,
								  psContext->ui32ReadOffset,
								  psContext->ui32CCBSize);
	if (ui32FreeSpace >= ui32Size)
	{
		*ppvBufferSpace = (void *)((IMG_UINT8 *)psContext->pvCCB +
		                  psContext->ui32WriteOffset);
		return PVRSRV_OK;
	}
	else
	{
		return PVRSRV_ERROR_RETRY;
	}
}

/*************************************************************************/ /*!
@Function       _SCPAlloc

@Description    Allocate space in the software command processor, handling the
                case where we wrap around the CCB.

@Input          psContext            Context to allocate from

@Input          ui32Size                Size to allocate

@Output         ppvBufferSpace          Pointer to space allocated

@Return         PVRSRV_OK if the allocation was successful
*/
/*****************************************************************************/
static
PVRSRV_ERROR _SCPAlloc(SCP_CONTEXT *psContext,
					   IMG_UINT32 ui32Size,
					   void **ppvBufferSpace)
{
	if ((ui32Size + PADDING_COMMAND_SIZE) > psContext->ui32CCBSize)
	{
		PVR_DPF((PVR_DBG_WARNING, "Command size (%d) too big for CCB\n", ui32Size));
		return PVRSRV_ERROR_CMD_TOO_BIG;
	}

	/*
		Check we don't overflow the end of the buffer and make sure we have
		enough for the padding command
	*/
	if ((psContext->ui32WriteOffset + ui32Size + PADDING_COMMAND_SIZE) > psContext->ui32CCBSize)
	{
		SCP_COMMAND *psCommand;
		void *pvCommand;
		PVRSRV_ERROR eError;
		IMG_UINT32 ui32Remain = psContext->ui32CCBSize - psContext->ui32WriteOffset;

		/* We're at the end of the buffer without enough contiguous space */
		eError = __SCPAlloc(psContext, ui32Remain, &pvCommand);
		if (eError != PVRSRV_OK)
		{
			PVR_ASSERT(eError == PVRSRV_ERROR_RETRY);
			return eError;
		}
		psCommand = pvCommand;
		psCommand->ui32CmdType = SCP_COMMAND_PADDING;
		psCommand->ui32CmdSize = ui32Remain;

		UPDATE_CCB_OFFSET(psContext->ui32WriteOffset, ui32Remain, psContext->ui32CCBSize);
	}

	return __SCPAlloc(psContext, ui32Size, ppvBufferSpace);
}

/*************************************************************************/ /*!
@Function       _SCPInsert

@Description    Insert the a finished command that was written into the CCB
                space allocated in a previous call to _SCPAlloc.
                This makes the command ready to be processed.

@Input          psContext               Context to allocate from

@Input          ui32Size                Size to allocate

@Return         None
*/
/*****************************************************************************/
static
void _SCPInsert(SCP_CONTEXT *psContext,
				IMG_UINT32 ui32Size)
{
	/*
	 * Update the write offset.
	 */
	UPDATE_CCB_OFFSET(psContext->ui32WriteOffset,
					  ui32Size,
					  psContext->ui32CCBSize);
}

/*************************************************************************/ /*!
@Function       _SCPCommandReady

@Description    Check if a command is ready. Checks to see if the command
                has had its fences met and is ready to go.

@Input          psCommand               Command to check

@Return         PVRSRV_OK if the command is ready
*/
/*****************************************************************************/
static
PVRSRV_ERROR _SCPCommandReady(PVRSRV_DEVICE_NODE *psDevNode, SCP_COMMAND *psCommand)
{

	if (psCommand->ui32CmdType == SCP_COMMAND_PADDING)
	{
		return PVRSRV_OK;
	}


#if defined(PVR_USE_FENCE_SYNC_MODEL)
	/* Check for the provided acquire fence */
	if (SyncIsFenceObjValid(&psCommand->sAcquireFenceObj))
	{
		PVRSRV_ERROR eErr;

		eErr = SyncFenceWaitKM(psDevNode, &psCommand->sAcquireFenceObj, 0);
		/* PVRSRV_ERROR_TIMEOUT means active. In this case we will retry later again. If the
		 * return value is an error we will release this fence and proceed.
		 * This makes sure that we are not getting stuck here when a fence transitions into
		 * an error state for whatever reason. */
		if (eErr == PVRSRV_ERROR_TIMEOUT)
		{
			return PVRSRV_ERROR_FAILED_DEPENDENCIES;
		}
		else
		{
			if (eErr != PVRSRV_OK)
			{
				PVR_LOG(("%s: Fence wait failed and returned %s", __func__, PVRSRVGetErrorString(eErr)));
			}
			/* Release the fence. */
			SyncFenceReleaseKM(&psCommand->sAcquireFenceObj);
			SyncClearFenceObj(&psCommand->sAcquireFenceObj);
		}
	}
#endif /* defined(PVR_USE_FENCE_SYNC_MODEL) */
	/* Command is ready */
	if (psCommand->pfnReady(psCommand->pvReadyData))
	{
		return PVRSRV_OK;
	}

	/*
		If we got here it means the command is ready to go, but the SCP client
		isn't ready for the command
	*/
	return PVRSRV_ERROR_NOT_READY;
}

/*************************************************************************/ /*!
@Function       _SCPCommandDo

@Description    Run a command

@Input          psCommand               Command to run

@Return         PVRSRV_OK if the command is ready
*/
/*****************************************************************************/
static
void _SCPCommandDo(SCP_COMMAND *psCommand)
{
	if (psCommand->ui32CmdType == SCP_COMMAND_CALLBACK)
	{
		psCommand->pfnDo(psCommand->pvReadyData, psCommand->pvCompleteData);
	}
}

/*************************************************************************/ /*!
@Function       _SCPDumpCommand

@Description    Dump a SCP command

@Input          psCommand               Command to dump

@Return         None
*/
/*****************************************************************************/
static void _SCPDumpCommand(SCP_COMMAND *psCommand,
				DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf,
				void *pvDumpDebugFile)
{

	if (psCommand->ui32CmdType == SCP_COMMAND_CALLBACK)
	{
	}
}

/*****************************************************************************
 *                    Public interface functions                             *
 *****************************************************************************/

/*
	SCPCreate
*/
PVRSRV_ERROR IMG_CALLCONV SCPCreate(PVRSRV_DEVICE_NODE *psDevNode,
									IMG_UINT32 ui32CCBSizeLog2,
									SCP_CONTEXT **ppsContext)
{
	SCP_CONTEXT	*psContext;
	IMG_UINT32 ui32Power2QueueSize = 1 << ui32CCBSizeLog2;
	PVRSRV_ERROR eError;

	/* allocate an internal queue info structure */
	psContext = OSAllocZMem(sizeof(SCP_CONTEXT));
	if (psContext == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,"SCPCreate: Failed to alloc queue struct"));
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorExit;
	}

	psContext->psDevNode = psDevNode;

	/* allocate the command queue buffer - allow for overrun */
	psContext->pvCCB = OSAllocMem(ui32Power2QueueSize);
	if (psContext->pvCCB == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,"SCPCreate: Failed to alloc queue buffer"));
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorExit;
	}

	psContext->ui32CCBSize = ui32Power2QueueSize;

	eError = OSLockCreate(&psContext->hLock);
	if (eError != PVRSRV_OK)
	{
		goto ErrorExit;
	}

	SCP_DEBUG_PRINT("%s: New SCP %p of size %d",
			__func__, psContext, ui32Power2QueueSize);

	*ppsContext = psContext;

	return PVRSRV_OK;

ErrorExit:
	if (psContext)
	{
		if (psContext->pvCCB)
		{
			OSFreeMem(psContext->pvCCB);
			psContext->pvCCB = NULL;
		}

		OSFreeMem(psContext);
	}

	return eError;
}

/*
	SCPAllocCommand
*/
PVRSRV_ERROR IMG_CALLCONV SCPAllocCommand(SCP_CONTEXT *psContext,
										  PVRSRV_FENCE iAcquireFence,
										  SCPReady pfnCommandReady,
										  SCPDo pfnCommandDo,
										  size_t ui32ReadyDataByteSize,
										  size_t ui32CompleteDataByteSize,
										  void **ppvReadyData,
										  void **ppvCompleteData,
										  PVRSRV_TIMELINE iReleaseFenceTimeline,
										  PVRSRV_FENCE *piReleaseFence)
{
	PVRSRV_ERROR eError;
	SCP_COMMAND *psCommand;
	IMG_UINT32 ui32CommandSize;

	SCP_DEBUG_PRINT("%s: iAcquireFence=%d, iReleaseFenceTimeline=%d, piReleaseFence=<%p>",
			__func__, iAcquireFence, iReleaseFenceTimeline, piReleaseFence);


	/* Round up the incoming data sizes to be pointer granular */
	ui32ReadyDataByteSize = (ui32ReadyDataByteSize & (~(sizeof(void *)-1))) + sizeof(void *);
	ui32CompleteDataByteSize = (ui32CompleteDataByteSize & (~(sizeof(void *)-1))) + sizeof(void *);


	/* Total command size */
	ui32CommandSize = sizeof(SCP_COMMAND) +
					  ui32ReadyDataByteSize +
					  ui32CompleteDataByteSize;

	eError = _SCPAlloc(psContext, ui32CommandSize, (void **) &psCommand);
	if (eError != PVRSRV_OK)
	{
		SCP_DEBUG_PRINT("%s: Failed to allocate command of size %d for ctx %p (%s)", __func__, ui32CommandSize, psContext, PVRSRVGetErrorString(eError));
		return eError;
	}

#if defined(PVR_USE_FENCE_SYNC_MODEL)
	if (piReleaseFence && iReleaseFenceTimeline != PVRSRV_NO_TIMELINE)
	{
		/* Create a release fence for the caller. */
		eError = SyncSWTimelineFenceCreateKM(psContext->psDevNode,
		                                     iReleaseFenceTimeline,
		                                     "pvr_scp_retire",
		                                     piReleaseFence);
		if (eError != PVRSRV_OK)
		{
			SCP_DEBUG_PRINT("%s: SyncSWTimelineFenceCreateKM() returned %s", __func__, PVRSRVGetErrorString(eError));
			return eError;
		}
	}
#endif /* defined(PVR_USE_FENCE_SYNC_MODEL) */
	SCP_DEBUG_PRINT("%s: New Command %p for ctx %p of size %d",
			__func__, psCommand, psContext, ui32CommandSize);

	/* setup the command */
	psCommand->ui32CmdSize = ui32CommandSize;
	psCommand->ui32CmdType = SCP_COMMAND_CALLBACK;


	psCommand->pfnReady = pfnCommandReady;
	psCommand->pfnDo = pfnCommandDo;

	psCommand->pvReadyData = ((IMG_CHAR *) psCommand) + sizeof(SCP_COMMAND);
	psCommand->pvCompleteData = ((IMG_CHAR *) psCommand) + sizeof(SCP_COMMAND) +
	                           ui32ReadyDataByteSize;


#if defined(PVR_USE_FENCE_SYNC_MODEL)
	/* Copy over the fences */
	if (iAcquireFence != PVRSRV_NO_FENCE)
	{
		SyncGetFenceObj(iAcquireFence, &psCommand->sAcquireFenceObj);
	}
	else
	{
		SyncClearFenceObj(&psCommand->sAcquireFenceObj);
	}

	if (piReleaseFence &&
		*piReleaseFence != PVRSRV_NO_FENCE &&
		iReleaseFenceTimeline != PVRSRV_NO_TIMELINE)
	{
		eError = SyncSWGetTimelineObj(iReleaseFenceTimeline, &psCommand->sSWTimelineObj);
		PVR_LOG_RETURN_IF_ERROR(eError, "SyncSWGetTimelineObj");
		eError = SyncGetFenceObj(*piReleaseFence, &psCommand->sReleaseFenceObj);
		PVR_LOG_RETURN_IF_ERROR(eError, "SyncGetFenceObj");
	}
	else
	{
		SyncClearTimelineObj(&psCommand->sSWTimelineObj);
		SyncClearFenceObj(&psCommand->sReleaseFenceObj);
	}
#endif /* defined(PVR_USE_FENCE_SYNC_MODEL) */

	*ppvReadyData = psCommand->pvReadyData;
	*ppvCompleteData = psCommand->pvCompleteData;

	return PVRSRV_OK;
}

/*
	SCPSubmitCommand
*/
PVRSRV_ERROR SCPSubmitCommand(SCP_CONTEXT *psContext)
{
	SCP_COMMAND *psCommand;

	if (psContext == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psCommand = (SCP_COMMAND *) ((IMG_UINT8 *) psContext->pvCCB
				+ psContext->ui32WriteOffset);

	SCP_DEBUG_PRINT("%s: Submit command %p for ctx %p",
			__func__, psCommand, psContext);

	_SCPInsert(psContext, psCommand->ui32CmdSize);

	return PVRSRV_OK;
}

/*
	SCPRun
*/
PVRSRV_ERROR SCPRun(SCP_CONTEXT *psContext)
{
	SCP_COMMAND *psCommand;
	PVRSRV_ERROR eError = PVRSRV_OK;


	if (psContext == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	OSLockAcquire(psContext->hLock);
	while (psContext->ui32DepOffset != psContext->ui32WriteOffset)
	{
		psCommand = (SCP_COMMAND *)((IMG_UINT8 *)psContext->pvCCB +
		            psContext->ui32DepOffset);

		/* See if the command is ready to go */
		eError = _SCPCommandReady(psContext->psDevNode, psCommand);

		SCP_DEBUG_PRINT("%s: Processes command %p for ctx %p (%d)",
				__func__, psCommand, psContext, eError);

		if (eError == PVRSRV_OK)
		{
			/* processed cmd so update queue */
			UPDATE_CCB_OFFSET(psContext->ui32DepOffset,
							  psCommand->ui32CmdSize,
							  psContext->ui32CCBSize);
		}
		else
		{
			/* As soon as we hit a command that can't run break out */
			break;
		}

		/* Run the command */
		_SCPCommandDo(psCommand);
	}
	OSLockRelease(psContext->hLock);

	return eError;
}

PVRSRV_ERROR SCPFlush(SCP_CONTEXT *psContext)
{
	if (psContext->ui32ReadOffset != psContext->ui32WriteOffset)
	{
		return PVRSRV_ERROR_RETRY;
	}

	return PVRSRV_OK;
}

/* This looks like a reasonable value. Number of traced syncs should
 * not exceed 20. */
#define MAX_TRACED_UFOS 20

/*
	SCPCommandComplete
*/
void SCPCommandComplete(SCP_CONTEXT *psContext,
                        IMG_BOOL bIgnoreFences)
{
	SCP_COMMAND *psCommand;
	IMG_BOOL bContinue = IMG_TRUE;
	PVR_UNREFERENCED_PARAMETER(bIgnoreFences);

	if (psContext == NULL)
	{
		return;
	}

	if (psContext->ui32ReadOffset == psContext->ui32DepOffset)
	{
		PVR_DPF((PVR_DBG_ERROR, "SCPCommandComplete: Called with nothing to do!"));
		return;
	}

	while (bContinue)
	{
		psCommand = (SCP_COMMAND *) ((IMG_UINT8 *) psContext->pvCCB +
					psContext->ui32ReadOffset);

		if (psCommand->ui32CmdType == SCP_COMMAND_CALLBACK)
		{


#if defined(PVR_USE_FENCE_SYNC_MODEL)
			if (SyncIsFenceObjValid(&psCommand->sReleaseFenceObj))
			{
				SyncSWTimelineAdvanceKM(psContext->psDevNode, &psCommand->sSWTimelineObj);
				SyncSWTimelineReleaseKM(&psCommand->sSWTimelineObj);
				SyncClearTimelineObj(&psCommand->sSWTimelineObj);

				/* Destroy the release fence */
				SyncFenceReleaseKM(&psCommand->sReleaseFenceObj);
				SyncClearFenceObj(&psCommand->sReleaseFenceObj);
			}
#endif /* defined(PVR_USE_FENCE_SYNC_MODEL) */
			bContinue = IMG_FALSE;
		}

		/* processed cmd so update queue */
		UPDATE_CCB_OFFSET(psContext->ui32ReadOffset,
						  psCommand->ui32CmdSize,
						  psContext->ui32CCBSize);

		SCP_DEBUG_PRINT("%s: Complete command %p for ctx %p (continue: %d)",
				__func__, psCommand, psContext, bContinue);

	}
}

IMG_BOOL SCPHasPendingCommand(SCP_CONTEXT *psContext)
{
	return psContext->ui32DepOffset != psContext->ui32WriteOffset;
}

void IMG_CALLCONV SCPDumpStatus(SCP_CONTEXT *psContext,
					DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf,
					void *pvDumpDebugFile)
{
	PVR_ASSERT(psContext != NULL);

	/*
		Acquire the lock to ensure that the SCP isn't run while
		while we're dumping info
	*/
	OSLockAcquire(psContext->hLock);

	PVR_DUMPDEBUG_LOG("Pending command:");
	if (psContext->ui32DepOffset == psContext->ui32WriteOffset)
	{
		PVR_DUMPDEBUG_LOG("\tNone");
	}
	else
	{
		SCP_COMMAND *psCommand;
		IMG_UINT32 ui32DepOffset = psContext->ui32DepOffset;

		while (ui32DepOffset != psContext->ui32WriteOffset)
		{
		        /* Dump the command we're pending on */
		        psCommand = (SCP_COMMAND *)((IMG_UINT8 *)psContext->pvCCB +
		                ui32DepOffset);

		        _SCPDumpCommand(psCommand, pfnDumpDebugPrintf, pvDumpDebugFile);

		        /* processed cmd so update queue */
		        UPDATE_CCB_OFFSET(ui32DepOffset,
		                                          psCommand->ui32CmdSize,
		                                          psContext->ui32CCBSize);

		}
	}

	PVR_DUMPDEBUG_LOG("Active command(s):");
	if (psContext->ui32DepOffset == psContext->ui32ReadOffset)
	{
		PVR_DUMPDEBUG_LOG("\tNone");
	}
	else
	{
		SCP_COMMAND *psCommand;
		IMG_UINT32 ui32ReadOffset = psContext->ui32ReadOffset;

		while (ui32ReadOffset != psContext->ui32DepOffset)
		{
			psCommand = (SCP_COMMAND *)((IMG_UINT8 *)psContext->pvCCB +
			            ui32ReadOffset);

			_SCPDumpCommand(psCommand, pfnDumpDebugPrintf, pvDumpDebugFile);

			/* processed cmd so update queue */
			UPDATE_CCB_OFFSET(ui32ReadOffset,
							  psCommand->ui32CmdSize,
							  psContext->ui32CCBSize);
		}
	}

	OSLockRelease(psContext->hLock);
}


/*
	SCPDestroy
*/
void IMG_CALLCONV SCPDestroy(SCP_CONTEXT *psContext)
{
	/*
		The caller must ensure that they completed all queued operations
		before calling this function
	*/

	PVR_ASSERT(psContext->ui32ReadOffset == psContext->ui32WriteOffset);

	OSLockDestroy(psContext->hLock);
	psContext->hLock = NULL;
	OSFreeMem(psContext->pvCCB);
	psContext->pvCCB = NULL;
	OSFreeMem(psContext);
}

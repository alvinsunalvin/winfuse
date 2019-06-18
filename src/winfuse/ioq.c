/**
 * @file winfuse/ioq.c
 *
 * @copyright 2019 Bill Zissimopoulos
 */
/*
 * This file is part of WinFuse.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * Affero General Public License version 3 as published by the Free
 * Software Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the AGPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#include <winfuse/driver.h>

NTSTATUS FuseIoqCreate(FUSE_IOQ **PIoq);
VOID FuseIoqDelete(FUSE_IOQ *Ioq);
VOID FuseIoqStartProcessing(FUSE_IOQ *Ioq, FUSE_CONTEXT *Context);
FUSE_CONTEXT *FuseIoqEndProcessing(FUSE_IOQ *Ioq, UINT64 Unique);
VOID FuseIoqPostPending(FUSE_IOQ *Ioq, FUSE_CONTEXT *Context);
FUSE_CONTEXT *FuseIoqNextPending(FUSE_IOQ *Ioq);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FuseIoqCreate)
#pragma alloc_text(PAGE, FuseIoqDelete)
#pragma alloc_text(PAGE, FuseIoqStartProcessing)
#pragma alloc_text(PAGE, FuseIoqEndProcessing)
#pragma alloc_text(PAGE, FuseIoqPostPending)
#pragma alloc_text(PAGE, FuseIoqNextPending)
#endif

#define FUSE_IOQ_SIZE                   1024

struct _FUSE_IOQ
{
    FAST_MUTEX Mutex;
    LIST_ENTRY PendingList, ProcessList;
    ULONG ProcessBucketCount;
    FUSE_CONTEXT *ProcessBuckets[];
};

NTSTATUS FuseIoqCreate(FUSE_IOQ **PIoq)
{
    PAGED_CODE();

    *PIoq = 0;

    FUSE_IOQ *Ioq;
    ULONG BucketCount = (FUSE_IOQ_SIZE - sizeof *Ioq) / sizeof Ioq->ProcessBuckets[0];
    Ioq = FuseAllocNonPaged(FUSE_IOQ_SIZE);
    if (0 == Ioq)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Ioq, FUSE_IOQ_SIZE);

    ExInitializeFastMutex(&Ioq->Mutex);
    InitializeListHead(&Ioq->PendingList);
    InitializeListHead(&Ioq->ProcessList);
    Ioq->ProcessBucketCount = BucketCount;

    *PIoq = Ioq;

    return STATUS_SUCCESS;
}

VOID FuseIoqDelete(FUSE_IOQ *Ioq)
{
    PAGED_CODE();

    for (PLIST_ENTRY ListEntry = Ioq->PendingList.Flink;
        &Ioq->PendingList != ListEntry;
        ListEntry = ListEntry->Flink)
    {
        FUSE_CONTEXT *Context = CONTAINING_RECORD(ListEntry, FUSE_CONTEXT, ListEntry);
        FuseContextDelete(Context);
    }
    for (PLIST_ENTRY ListEntry = Ioq->ProcessList.Flink;
        &Ioq->ProcessList != ListEntry;
        ListEntry = ListEntry->Flink)
    {
        FUSE_CONTEXT *Context = CONTAINING_RECORD(ListEntry, FUSE_CONTEXT, ListEntry);
        FuseContextDelete(Context);
    }
    FuseFree(Ioq);
}

VOID FuseIoqStartProcessing(FUSE_IOQ *Ioq, FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    ExAcquireFastMutex(&Ioq->Mutex);

    InsertTailList(&Ioq->ProcessList, &Context->ListEntry);

    ULONG Index = FuseHashMixPointer(Context) % Ioq->ProcessBucketCount;
#if DBG
    for (FUSE_CONTEXT *ContextX = Ioq->ProcessBuckets[Index]; ContextX; ContextX = ContextX->DictNext)
        ASSERT(ContextX != Context);
#endif
    ASSERT(0 == Context->DictNext);
    Context->DictNext = Ioq->ProcessBuckets[Index];
    Ioq->ProcessBuckets[Index] = Context;

    ExReleaseFastMutex(&Ioq->Mutex);
}

FUSE_CONTEXT *FuseIoqEndProcessing(FUSE_IOQ *Ioq, UINT64 Unique)
{
    PAGED_CODE();

    FUSE_CONTEXT *ContextHint = (PVOID)(UINT_PTR)Unique;
    FUSE_CONTEXT *Context = 0;

    ExAcquireFastMutex(&Ioq->Mutex);

    ULONG Index = FuseHashMixPointer(ContextHint) % Ioq->ProcessBucketCount;
    for (FUSE_CONTEXT **PContext = &Ioq->ProcessBuckets[Index];; PContext = &(*PContext)->DictNext)
    {
        ASSERT(0 != *PContext);
        if (*PContext == ContextHint)
        {
            *PContext = (*PContext)->DictNext;
            (*PContext)->DictNext = 0;

            Context = ContextHint;
            RemoveEntryList(&Context->ListEntry);

            break;
        }
    }

    ExReleaseFastMutex(&Ioq->Mutex);

    return Context;
}

VOID FuseIoqPostPending(FUSE_IOQ *Ioq, FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    ExAcquireFastMutex(&Ioq->Mutex);

    InsertTailList(&Ioq->PendingList, &Context->ListEntry);

    ExReleaseFastMutex(&Ioq->Mutex);
}

FUSE_CONTEXT *FuseIoqNextPending(FUSE_IOQ *Ioq)
{
    PAGED_CODE();

    ExAcquireFastMutex(&Ioq->Mutex);

    PLIST_ENTRY ListEntry = Ioq->PendingList.Flink;
    FUSE_CONTEXT *Context = &Ioq->PendingList != ListEntry ?
        CONTAINING_RECORD(ListEntry, FUSE_CONTEXT, ListEntry) : 0;

    if (0 != Context)
        RemoveEntryList(&Context->ListEntry);

    ExReleaseFastMutex(&Ioq->Mutex);

    return Context;
}
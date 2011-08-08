/* $Id$ */
/** @file
 * VBox USB Monitor
 */
/*
 * Copyright (C) 2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include "VBoxUsbMon.h"
#include "../cmn/VBoxUsbIdc.h"
#include <vbox/err.h>
#include <VBox/usblib.h>
#include <excpt.h>
#include <stdio.h>

/*
 * Note: Must match the VID & PID in the USB driver .inf file!!
 */
/*
  BusQueryDeviceID USB\Vid_80EE&Pid_CAFE
  BusQueryInstanceID 2
  BusQueryHardwareIDs USB\Vid_80EE&Pid_CAFE&Rev_0100
  BusQueryHardwareIDs USB\Vid_80EE&Pid_CAFE
  BusQueryCompatibleIDs USB\Class_ff&SubClass_00&Prot_00
  BusQueryCompatibleIDs USB\Class_ff&SubClass_00
  BusQueryCompatibleIDs USB\Class_ff
*/

#define szBusQueryDeviceId       L"USB\\Vid_80EE&Pid_CAFE"
#define szBusQueryHardwareIDs    L"USB\\Vid_80EE&Pid_CAFE&Rev_0100\0USB\\Vid_80EE&Pid_CAFE\0\0"
#define szBusQueryCompatibleIDs  L"USB\\Class_ff&SubClass_00&Prot_00\0USB\\Class_ff&SubClass_00\0USB\\Class_ff\0\0"

#define szDeviceTextDescription          L"VirtualBox USB"

typedef struct VBOXUSBMONINS
{
    void * pvDummy;
} VBOXUSBMONINS, *PVBOXUSBMONINS;

typedef struct VBOXUSBMONCTX
{
    VBOXUSBFLTCTX FltCtx;
} VBOXUSBMONCTX, *PVBOXUSBMONCTX;

typedef struct VBOXUSBHUB_PNPHOOK
{
    VBOXUSBHOOK_ENTRY Hook;
    bool fUninitFailed;
} VBOXUSBHUB_PNPHOOK, *PVBOXUSBHUB_PNPHOOK;

typedef struct VBOXUSBHUB_PNPHOOK_COMPLETION
{
    VBOXUSBHOOK_REQUEST Rq;
} VBOXUSBHUB_PNPHOOK_COMPLETION, *PVBOXUSBHUB_PNPHOOK_COMPLETION;

typedef struct VBOXUSBMONGLOBALS
{
    PDEVICE_OBJECT pDevObj;
    VBOXUSBHUB_PNPHOOK UsbHubPnPHook;
    KEVENT OpenSynchEvent;
    IO_REMOVE_LOCK RmLock;
    uint32_t cOpens;
    volatile LONG ulPreventUnloadOn;
    PFILE_OBJECT pPreventUnloadFileObj;
} VBOXUSBMONGLOBALS, *PVBOXUSBMONGLOBALS;

static VBOXUSBMONGLOBALS g_VBoxUsbMonGlobals;

#define VBOXUSBMON_MEMTAG 'MUBV'

PVOID VBoxUsbMonMemAlloc(SIZE_T cbBytes)
{
    PVOID pvMem = ExAllocatePoolWithTag(NonPagedPool, cbBytes, VBOXUSBMON_MEMTAG);
    Assert(pvMem);
    return pvMem;
}

PVOID VBoxUsbMonMemAllocZ(SIZE_T cbBytes)
{
    PVOID pvMem = VBoxUsbMonMemAlloc(cbBytes);
    if (pvMem)
    {
        RtlZeroMemory(pvMem, cbBytes);
    }
    return pvMem;
}

VOID VBoxUsbMonMemFree(PVOID pvMem)
{
    ExFreePoolWithTag(pvMem, VBOXUSBMON_MEMTAG);
}

#define VBOXUSBDBG_STRCASE(_t) \
        case _t: return #_t
#define VBOXUSBDBG_STRCASE_UNKNOWN(_v) \
        default: LOG((__FUNCTION__": Unknown Value (0n%d), (0x%x)\n", _v, _v)); return "Unknown"

static const char* vboxUsbDbgStrPnPMn(UCHAR uMn)
{
    switch (uMn)
    {
        VBOXUSBDBG_STRCASE(IRP_MN_START_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_REMOVE_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_REMOVE_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_CANCEL_REMOVE_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_STOP_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_STOP_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_CANCEL_STOP_DEVICE);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_DEVICE_RELATIONS);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_INTERFACE);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_CAPABILITIES);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_RESOURCES);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_RESOURCE_REQUIREMENTS);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_DEVICE_TEXT);
        VBOXUSBDBG_STRCASE(IRP_MN_FILTER_RESOURCE_REQUIREMENTS);
        VBOXUSBDBG_STRCASE(IRP_MN_READ_CONFIG);
        VBOXUSBDBG_STRCASE(IRP_MN_WRITE_CONFIG);
        VBOXUSBDBG_STRCASE(IRP_MN_EJECT);
        VBOXUSBDBG_STRCASE(IRP_MN_SET_LOCK);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_ID);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_PNP_DEVICE_STATE);
        VBOXUSBDBG_STRCASE(IRP_MN_QUERY_BUS_INFORMATION);
        VBOXUSBDBG_STRCASE(IRP_MN_DEVICE_USAGE_NOTIFICATION);
        VBOXUSBDBG_STRCASE(IRP_MN_SURPRISE_REMOVAL);
        VBOXUSBDBG_STRCASE_UNKNOWN(uMn);
    }
}

void vboxUsbDbgPrintUnicodeString(PUNICODE_STRING pUnicodeString)
{
    PWSTR pStr = pUnicodeString->Buffer;
    for (int i = 0; i < pUnicodeString->Length/2; ++i)
    {
        LOG(("%c", *pStr++));
    }
}

/**
 * Send IRP_MN_QUERY_DEVICE_RELATIONS
 *
 * @returns NT Status
 * @param   pDevObj         USB device pointer
 * @param   pFileObj        Valid file object pointer
 * @param   pDevRelations   Pointer to DEVICE_RELATIONS pointer (out)
 */
NTSTATUS VBoxUsbMonQueryBusRelations(PDEVICE_OBJECT pDevObj, PFILE_OBJECT pFileObj, PDEVICE_RELATIONS *pDevRelations)
{
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    NTSTATUS Status;
    PIRP pIrp;
    PIO_STACK_LOCATION pSl;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Assert(pDevRelations);
    *pDevRelations = NULL;

    pIrp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, pDevObj, NULL, 0, NULL, &Event, &IoStatus);
    if (!pIrp)
    {
        WARN(("IoBuildDeviceIoControlRequest failed!!\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoStatus.Status = STATUS_NOT_SUPPORTED;

    pSl = IoGetNextIrpStackLocation(pIrp);
    pSl->MajorFunction = IRP_MJ_PNP;
    pSl->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
    pSl->Parameters.QueryDeviceRelations.Type = BusRelations;
    pSl->FileObject = pFileObj;

    Status = IoCallDriver(pDevObj, pIrp);
    if (Status == STATUS_PENDING)
    {
        LOG(("IoCallDriver returned STATUS_PENDING!!\n"));
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (Status == STATUS_SUCCESS)
    {
        PDEVICE_RELATIONS pRel = (PDEVICE_RELATIONS)IoStatus.Information;
        LOG(("pRel = %p\n", pRel));
        if (VALID_PTR(pRel))
        {
            *pDevRelations = pRel;
        }
        else
        {
            WARN(("Invalid pointer %p\n", pRel));
        }
    }
    else
    {
        WARN(("IRP_MN_QUERY_DEVICE_RELATIONS failed Status(0x%x)\n", Status));
    }

    LOG(("IoCallDriver returned %x\n", Status));
    return Status;
}

static PDRIVER_OBJECT vboxUsbMonHookFindHubDrvObj()
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    UNICODE_STRING szStandardHubName;
    PDRIVER_OBJECT pDrvObj = NULL;
    szStandardHubName.Length = 0;
    szStandardHubName.MaximumLength = 0;
    szStandardHubName.Buffer = 0;
    RtlInitUnicodeString(&szStandardHubName, L"\\Driver\\usbhub");

    LOG(("Search USB hub\n"));
    for (int i = 0; i < 16; i++)
    {
        WCHAR           szwHubName[32] = {};
        char            szHubName[32] = {};
        ANSI_STRING     AnsiName;
        UNICODE_STRING  UnicodeName;
        PDEVICE_OBJECT  pHubDevObj;
        PFILE_OBJECT    pHubFileObj;

        sprintf(szHubName, "\\Device\\USBPDO-%d", i);

        RtlInitAnsiString(&AnsiName, szHubName);

        UnicodeName.Length = 0;
        UnicodeName.MaximumLength = sizeof (szwHubName);
        UnicodeName.Buffer = szwHubName;

        RtlInitAnsiString(&AnsiName, szHubName);
        Status = RtlAnsiStringToUnicodeString(&UnicodeName, &AnsiName, FALSE);
        if (Status == STATUS_SUCCESS)
        {
            Status = IoGetDeviceObjectPointer(&UnicodeName, FILE_READ_DATA, &pHubFileObj, &pHubDevObj);
            if (Status == STATUS_SUCCESS)
            {
                LOG(("IoGetDeviceObjectPointer for %S returned %p %p\n", szwHubName, pHubDevObj, pHubFileObj));

                if (pHubDevObj->DriverObject
                    && pHubDevObj->DriverObject->DriverName.Buffer
                    && pHubDevObj->DriverObject->DriverName.Length
                    && !RtlCompareUnicodeString(&szStandardHubName, &pHubDevObj->DriverObject->DriverName, TRUE /* case insensitive */))
                {
                    LOG(("Associated driver"));
                    LOG_USTR(&pHubDevObj->DriverObject->DriverName);
                    LOG(("pnp handler %p\n", pHubDevObj->DriverObject->MajorFunction[IRP_MJ_PNP]));

                    pDrvObj = pHubDevObj->DriverObject;
                    /* ensure the driver object is not destroyed */
                    ObReferenceObject(pDrvObj);
                    /* release the file object which will releade the dev objectas well,
                     * as we do not need those anymore */
                    ObDereferenceObject(pHubFileObj);
                    break;
                }
                else
                {
                    LOG(("driver object (0x%p) no match", pHubDevObj->DriverObject));
                    if (pHubDevObj->DriverObject)
                    {
                        if (   pHubDevObj->DriverObject->DriverName.Buffer
                            && pHubDevObj->DriverObject->DriverName.Length)
                        {
                            LOG(("driver name not match, was:"));
                            LOG_USTR(&pHubDevObj->DriverObject->DriverName);
                            LOG(("but expected:"));
                            LOG_USTR(&szStandardHubName);
                        }
                        else
                        {
                            LOG(("driver name is zero, Length(%d), Buffer(0x%p)",
                                    pHubDevObj->DriverObject->DriverName.Length, pHubDevObj->DriverObject->DriverName.Buffer));
                        }
                    }
                    else
                    {
                        LOG(("driver object is NULL"));
                    }
                }
                ObDereferenceObject(pHubFileObj);
            }
            else
            {
                WARN(("IoGetDeviceObjectPointer returned Status (0x%x) for (%S)", Status, szwHubName));
            }
        }
        else
        {
            WARN(("RtlAnsiStringToUnicodeString failed, Status (0x%x) for Ansu name (%s)", Status, szHubName));
        }
    }

    return pDrvObj;
}

/* NOTE: the stack location data is not the "actual" IRP stack location,
 * but a copy being preserved on the IRP way down.
 * See the note in VBoxUsbPnPCompletion for detail */
static NTSTATUS vboxUsbMonHandlePnPIoctl(PDEVICE_OBJECT pDevObj, PIO_STACK_LOCATION pSl, PIO_STATUS_BLOCK pIoStatus)
{
    LOG(("IRQL = %d\n", KeGetCurrentIrql()));
    switch(pSl->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_TEXT:
        {
            LOG(("IRP_MN_QUERY_DEVICE_TEXT: pIoStatus->Status = %x\n", pIoStatus->Status));
            if (pIoStatus->Status == STATUS_SUCCESS)
            {
                WCHAR *pId = (WCHAR *)pIoStatus->Information;
                if (VALID_PTR(pId))
                {
                    KIRQL Iqrl = KeGetCurrentIrql();
                    /* IRQL should be always passive here */
                    ASSERT_WARN(Iqrl == PASSIVE_LEVEL, ("irql is not PASSIVE"));
                    switch(pSl->Parameters.QueryDeviceText.DeviceTextType)
                    {
                        case DeviceTextLocationInformation:
                            LOG(("DeviceTextLocationInformation"));
                            LOG_STRW(pId);
                            break;

                        case DeviceTextDescription:
                            LOG(("DeviceTextDescription"));
                            LOG_STRW(pId);
                            if (VBoxUsbFltPdoIsFiltered(pDevObj))
                            {
                                LOG(("PDO (0x%p) is filtered", pDevObj));
                                WCHAR *pId = (WCHAR *)ExAllocatePool(PagedPool, sizeof(szDeviceTextDescription));
                                if (!pId)
                                {
                                    AssertFailed();
                                    break;
                                }
                                memcpy(pId, szDeviceTextDescription, sizeof(szDeviceTextDescription));
                                LOG(("NEW szDeviceTextDescription"));
                                LOG_STRW(pId);
                                ExFreePool((PVOID)pIoStatus->Information);
                                pIoStatus->Information = (ULONG_PTR)pId;
                            }
                            else
                            {
                                LOG(("PDO (0x%p) is NOT filtered", pDevObj));
                            }
                            break;
                        default:
                            LOG(("DeviceText %d", pSl->Parameters.QueryDeviceText.DeviceTextType));
                            break;
                    }
                }
                else
                    LOG(("Invalid pointer %p\n", pId));
            }
            break;
        }

        case IRP_MN_QUERY_ID:
        {
            LOG(("IRP_MN_QUERY_ID: Irp->pIoStatus->Status = %x\n", pIoStatus->Status));
            if (pIoStatus->Status == STATUS_SUCCESS &&  pDevObj)
            {
                WCHAR *pId = (WCHAR *)pIoStatus->Information;
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
                WCHAR *pTmp;
#endif
                if (VALID_PTR(pId))
                {
                    KIRQL Iqrl = KeGetCurrentIrql();
                    /* IRQL should be always passive here */
                    ASSERT_WARN(Iqrl == PASSIVE_LEVEL, ("irql is not PASSIVE"));

                    switch (pSl->Parameters.QueryDeviceRelations.Type)
                    {
                        case BusQueryInstanceID:
                            LOG(("BusQueryInstanceID"));
                            LOG_STRW(pId);
                            break;

                        case BusQueryDeviceID:
                        {
                            LOG(("BusQueryDeviceID"));
                            pId = (WCHAR *)ExAllocatePool(PagedPool, sizeof(szBusQueryDeviceId));
                            if (!pId)
                            {
                                WARN(("ExAllocatePool failed"));
                                break;
                            }

                            BOOLEAN bFiltered = FALSE;
                            NTSTATUS Status = VBoxUsbFltPdoAdd(pDevObj, &bFiltered);
                            if (Status != STATUS_SUCCESS || !bFiltered)
                            {
                                if(Status == STATUS_SUCCESS)
                                {
                                    LOG(("PDO (0x%p) is NOT filtered", pDevObj));
                                }
                                else
                                {
                                    WARN(("VBoxUsbFltPdoAdd for PDO (0x%p) failed Status 0x%x", pDevObj, Status));
                                }
                                ExFreePool(pId);
                                break;
                            }

                            LOG(("PDO (0x%p) is filtered", pDevObj));
                            ExFreePool((PVOID)pIoStatus->Information);
                            memcpy(pId, szBusQueryDeviceId, sizeof(szBusQueryDeviceId));
                            pIoStatus->Information = (ULONG_PTR)pId;
                            break;
                        }
                    case BusQueryHardwareIDs:
                    {
                        LOG(("BusQueryHardwareIDs"));
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
                        while(*pId) //MULTI_SZ
                        {
                            LOG_STRW(pId);
                            while(*pId) pId++;
                            pId++;
                        }
#endif
                        pId = (WCHAR *)ExAllocatePool(PagedPool, sizeof(szBusQueryHardwareIDs));
                        if (!pId)
                        {
                            WARN(("ExAllocatePool failed"));
                            break;
                        }

                        BOOLEAN bFiltered = FALSE;
                        NTSTATUS Status = VBoxUsbFltPdoAdd(pDevObj, &bFiltered);
                        if (Status != STATUS_SUCCESS || !bFiltered)
                        {
                            if(Status == STATUS_SUCCESS)
                            {
                                LOG(("PDO (0x%p) is NOT filtered", pDevObj));
                            }
                            else
                            {
                                WARN(("VBoxUsbFltPdoAdd for PDO (0x%p) failed Status 0x%x", pDevObj, Status));
                            }
                            ExFreePool(pId);
                            break;
                        }

                        LOG(("PDO (0x%p) is filtered", pDevObj));

                        memcpy(pId, szBusQueryHardwareIDs, sizeof(szBusQueryHardwareIDs));
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
                        LOG(("NEW BusQueryHardwareIDs"));
                        pTmp = pId;
                        while(*pTmp) //MULTI_SZ
                        {

                            LOG_STRW(pTmp);
                            while(*pTmp) pTmp++;
                            pTmp++;
                        }
#endif
                        ExFreePool((PVOID)pIoStatus->Information);
                        pIoStatus->Information = (ULONG_PTR)pId;
                        break;
                    }
                    case BusQueryCompatibleIDs:
                        LOG(("BusQueryCompatibleIDs"));
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
                        while(*pId) //MULTI_SZ
                        {
                            LOG_STRW(pId);
                            while(*pId) pId++;
                            pId++;
                        }
#endif
                        if (VBoxUsbFltPdoIsFiltered(pDevObj))
                        {
                            LOG(("PDO (0x%p) is filtered", pDevObj));
                            pId = (WCHAR *)ExAllocatePool(PagedPool, sizeof(szBusQueryCompatibleIDs));
                            if (!pId)
                            {
                                WARN(("ExAllocatePool failed"));
                                break;
                            }
                            memcpy(pId, szBusQueryCompatibleIDs, sizeof(szBusQueryCompatibleIDs));
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
                            LOG(("NEW BusQueryCompatibleIDs"));
                            pTmp = pId;
                            while(*pTmp) //MULTI_SZ
                            {
                                LOG_STRW(pTmp);
                                while(*pTmp) pTmp++;
                                pTmp++;
                            }
#endif
                            ExFreePool((PVOID)pIoStatus->Information);
                            pIoStatus->Information = (ULONG_PTR)pId;
                        }
                        else
                        {
                            LOG(("PDO (0x%p) is NOT filtered", pDevObj));
                        }
                        break;
                    }
                }
                else
                {
                    LOG(("Invalid pointer %p\n", pId));
                }
            }
            break;
        }

#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            switch(pSl->Parameters.QueryDeviceRelations.Type)
            {
            case BusRelations:
            {
                LOG(("BusRelations\n"));

                if (pIoStatus->Status == STATUS_SUCCESS)
                {
                    PDEVICE_RELATIONS pRel = (PDEVICE_RELATIONS)pIoStatus->Information;
                    LOG(("pRel = %p\n", pRel));
                    if (VALID_PTR(pRel))
                    {
                        for (unsigned i=0;i<pRel->Count;i++)
                        {
                            if (VBoxUsbFltPdoIsFiltered(pDevObj))
                                LOG(("New PDO %p\n", pRel->Objects[i]));
                        }
                    }
                    else
                        LOG(("Invalid pointer %p\n", pRel));
                }
                break;
            }
            case TargetDeviceRelation:
                LOG(("TargetDeviceRelation\n"));
                break;
            case RemovalRelations:
                LOG(("RemovalRelations\n"));
                break;
            case EjectionRelations:
                LOG(("EjectionRelations\n"));
                break;
            }
            break;
        }

        case IRP_MN_QUERY_CAPABILITIES:
        {
            LOG(("IRP_MN_QUERY_CAPABILITIES: pIoStatus->Status = %x\n", pIoStatus->Status));
            if (pIoStatus->Status == STATUS_SUCCESS)
            {
                PDEVICE_CAPABILITIES pCaps = pSl->Parameters.DeviceCapabilities.Capabilities;
                if (VALID_PTR(pCaps))
                {
                    LOG(("Caps.SilentInstall  = %d\n", pCaps->SilentInstall));
                    LOG(("Caps.UniqueID       = %d\n", pCaps->UniqueID ));
                    LOG(("Caps.Address        = %d\n", pCaps->Address ));
                    LOG(("Caps.UINumber       = %d\n", pCaps->UINumber ));
                }
                else
                    LOG(("Invalid pointer %p\n", pCaps));
            }
            break;
        }

        default:
            break;
#endif
    } /*switch */

    LOG(("Done returns %x (IRQL = %d)\n", pIoStatus->Status, KeGetCurrentIrql()));
    return pIoStatus->Status;
}

NTSTATUS _stdcall VBoxUsbPnPCompletion(DEVICE_OBJECT *pDevObj, IRP *pIrp, void *pvContext)
{
    LOG(("Completion PDO(0x%p), IRP(0x%p), Status(0x%x)\n", pDevObj, pIrp, pIrp->IoStatus.Status));
    ASSERT_WARN(pvContext, ("zero context"));

    PVBOXUSBHOOK_REQUEST pRequest = (PVBOXUSBHOOK_REQUEST)pvContext;
    /* NOTE: despite a regular IRP processing the stack location in our completion
     * differs from those of the PnP hook since the hook is invoked in the "context" of the calle,
     * while the completion is in the "coller" context in terms of IRP,
     * so the completion stack location is one level "up" here.
     *
     * Moreover we CAN NOT access irp stack location in the completion because we might not have one at all
     * in case the hooked driver is at the top of the irp call stack
     *
     * This is why we use the stack location we saved on IRP way down.
     * */
    PIO_STACK_LOCATION pSl = &pRequest->OldLocation;
    ASSERT_WARN(pIrp == pRequest->pIrp, ("completed IRP(0x%x) not match request IRP(0x%x)", pIrp, pRequest->pIrp));
    /* NOTE: we can not rely on pDevObj passed in IoCompletion since it may be zero
     * in case IRP was created with extra stack locations and the caller did not initialize
     * the IO_STACK_LOCATION::DeviceObject */
    DEVICE_OBJECT *pRealDevObj = pRequest->pDevObj;
//    Assert(!pDevObj || pDevObj == pRealDevObj);
//    Assert(pSl->DeviceObject == pDevObj);

    switch(pSl->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_TEXT:
        case IRP_MN_QUERY_ID:
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        case IRP_MN_QUERY_CAPABILITIES:
#endif
            if (NT_SUCCESS(pIrp->IoStatus.Status))
            {
                vboxUsbMonHandlePnPIoctl(pRealDevObj, pSl, &pIrp->IoStatus);
            }
            else
            {
                ASSERT_WARN(pIrp->IoStatus.Status == STATUS_NOT_SUPPORTED, ("Irp failed with status(0x%x)", pIrp->IoStatus.Status));
            }
            break;

        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            if (NT_SUCCESS(pIrp->IoStatus.Status))
            {
                VBoxUsbFltPdoRemove(pRealDevObj);
            }
            else
            {
                AssertFailed();
            }
            break;

        /* These two IRPs are received when the PnP subsystem has determined the id of the newly arrived device */
        /* IRP_MN_START_DEVICE only arrives if it's a USB device of a known class or with a present host driver */
        case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        case IRP_MN_QUERY_RESOURCES:
            if (NT_SUCCESS(pIrp->IoStatus.Status) || pIrp->IoStatus.Status == STATUS_NOT_SUPPORTED)
            {
                VBoxUsbFltPdoAddCompleted(pRealDevObj);
            }
            else
            {
                AssertFailed();
            }
            break;

        default:
            break;
    }

    LOG(("<==PnP: Mn(%s), PDO(0x%p), IRP(0x%p), Status(0x%x), Sl PDO(0x%p), Compl PDO(0x%p)\n",
                            vboxUsbDbgStrPnPMn(pSl->MinorFunction),
                            pRealDevObj, pIrp, pIrp->IoStatus.Status,
                            pSl->DeviceObject, pDevObj));
#ifdef DEBUG_misha
    NTSTATUS tmpStatus = pIrp->IoStatus.Status;
#endif
    NTSTATUS Status = VBoxUsbHookRequestComplete(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook, pDevObj, pIrp, pRequest);
    VBoxUsbMonMemFree(pRequest);
#ifdef DEBUG_misha
    if (Status != STATUS_MORE_PROCESSING_REQUIRED)
    {
        Assert(pIrp->IoStatus.Status == tmpStatus);
    }
#endif
    VBoxUsbHookRelease(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook);
    return Status;
}

/**
 * Device PnP hook
 *
 * @param   pDevObj     Device object.
 * @param   pIrp         Request packet.
 */
NTSTATUS _stdcall VBoxUsbMonPnPHook(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    LOG(("==>PnP: Mn(%s), PDO(0x%p), IRP(0x%p), Status(0x%x)\n", vboxUsbDbgStrPnPMn(IoGetCurrentIrpStackLocation(pIrp)->MinorFunction), pDevObj, pIrp, pIrp->IoStatus.Status));

    if(!VBoxUsbHookRetain(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook))
    {
        WARN(("VBoxUsbHookRetain failed"));
        return VBoxUsbHookRequestPassDownHookSkip(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook, pDevObj, pIrp);
    }

    PVBOXUSBHUB_PNPHOOK_COMPLETION pCompletion = (PVBOXUSBHUB_PNPHOOK_COMPLETION)VBoxUsbMonMemAlloc(sizeof (*pCompletion));
    if (!pCompletion)
    {
        WARN(("VBoxUsbMonMemAlloc failed"));
        VBoxUsbHookRelease(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook);
        pIrp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        pIrp->IoStatus.Information = 0;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NTSTATUS Status = VBoxUsbHookRequestPassDownHookCompletion(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook, pDevObj, pIrp, VBoxUsbPnPCompletion, &pCompletion->Rq);
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
    if (Status != STATUS_PENDING)
    {
        LOG(("Request completed, Status(0x%x)", Status));
        VBoxUsbHookVerifyCompletion(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook, &pCompletion->Rq, pIrp);
    }
    else
    {
        LOG(("Request pending"));
    }
#endif
    return Status;
}


static NTSTATUS vboxUsbMonHookCheckInit()
{
    static bool fIsHookInited = false;
    if (fIsHookInited)
    {
        LOG(("hook inited already, success"));
        return STATUS_SUCCESS;
    }
    PDRIVER_OBJECT pDrvObj = vboxUsbMonHookFindHubDrvObj();
    if (pDrvObj)
    {
        VBoxUsbHookInit(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook, pDrvObj, IRP_MJ_PNP, VBoxUsbMonPnPHook);
        fIsHookInited = true;
        LOG(("SUCCESS"));
        return STATUS_SUCCESS;
    }
    WARN(("hub drv obj not found, fail"));
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS vboxUsbMonHookInstall()
{
#ifdef VBOXUSBMON_DBG_NO_PNPHOOK
    return STATUS_SUCCESS;
#else
    if (g_VBoxUsbMonGlobals.UsbHubPnPHook.fUninitFailed)
    {
        WARN(("trying to hook usbhub pnp after the unhook failed, do nothing & pretend success"));
        return STATUS_SUCCESS;
    }
    return VBoxUsbHookInstall(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook);
#endif
}

static NTSTATUS vboxUsbMonHookUninstall()
{
#ifdef VBOXUSBMON_DBG_NO_PNPHOOK
    return STATUS_SUCCESS;
#else
    NTSTATUS Status = VBoxUsbHookUninstall(&g_VBoxUsbMonGlobals.UsbHubPnPHook.Hook);
    if (!NT_SUCCESS(Status))
    {
        AssertMsgFailed(("usbhub pnp unhook failed, setting the fUninitFailed flag, the current value of fUninitFailed (%d)\n", g_VBoxUsbMonGlobals.UsbHubPnPHook.fUninitFailed));
        g_VBoxUsbMonGlobals.UsbHubPnPHook.fUninitFailed = true;
    }
    return Status;
#endif
}


static NTSTATUS vboxUsbMonCheckTermStuff()
{
    NTSTATUS Status = KeWaitForSingleObject(&g_VBoxUsbMonGlobals.OpenSynchEvent,
            Executive, KernelMode,
            FALSE, /* BOOLEAN Alertable */
            NULL /* IN PLARGE_INTEGER Timeout */
            );
    AssertRelease(Status == STATUS_SUCCESS);

    do
    {
        if (--g_VBoxUsbMonGlobals.cOpens)
            break;

        Status = vboxUsbMonHookUninstall();

        NTSTATUS tmpStatus = VBoxUsbFltTerm();
        if (!NT_SUCCESS(tmpStatus))
        {
            /* this means a driver state is screwed up, KeBugCheckEx here ? */
            AssertReleaseFailed();
        }
    } while (0);

    KeSetEvent(&g_VBoxUsbMonGlobals.OpenSynchEvent, 0, FALSE);

    return Status;
}

static NTSTATUS vboxUsbMonCheckInitStuff()
{
    NTSTATUS Status = KeWaitForSingleObject(&g_VBoxUsbMonGlobals.OpenSynchEvent,
            Executive, KernelMode,
            FALSE, /* BOOLEAN Alertable */
            NULL /* IN PLARGE_INTEGER Timeout */
        );
    if (Status == STATUS_SUCCESS)
    {
        do
        {
            if (g_VBoxUsbMonGlobals.cOpens++)
            {
                LOG(("opens: %d, success", g_VBoxUsbMonGlobals.cOpens));
                break;
            }

            Status = VBoxUsbFltInit();
            if (NT_SUCCESS(Status))
            {
                Status = vboxUsbMonHookCheckInit();
                if (NT_SUCCESS(Status))
                {
                    Status = vboxUsbMonHookInstall();
                    if (NT_SUCCESS(Status))
                    {
                        Status = STATUS_SUCCESS;
                        LOG(("succeded!!"));
                        break;
                    }
                    else
                    {
                        WARN(("vboxUsbMonHookInstall failed, Status (0x%x)", Status));
                    }
                }
                else
                {
                    WARN(("vboxUsbMonHookCheckInit failed, Status (0x%x)", Status));
                }
                VBoxUsbFltTerm();
            }
            else
            {
                WARN(("VBoxUsbFltInit failed, Status (0x%x)", Status));
            }

            --g_VBoxUsbMonGlobals.cOpens;
            Assert(!g_VBoxUsbMonGlobals.cOpens);
        } while (0);

        KeSetEvent(&g_VBoxUsbMonGlobals.OpenSynchEvent, 0, FALSE);
    }
    else
    {
        WARN(("KeWaitForSingleObject failed, Status (0x%x)", Status));
    }
    return Status;
}

static NTSTATUS vboxUsbMonContextCreate(PVBOXUSBMONCTX *ppCtx)
{
    NTSTATUS Status;
    *ppCtx = NULL;
    PVBOXUSBMONCTX pFileCtx = (PVBOXUSBMONCTX)VBoxUsbMonMemAllocZ(sizeof (*pFileCtx));
    if (pFileCtx)
    {
        Status = vboxUsbMonCheckInitStuff();
        if (Status == STATUS_SUCCESS)
        {
            Status = VBoxUsbFltCreate(&pFileCtx->FltCtx);
            if (Status == STATUS_SUCCESS)
            {
                *ppCtx = pFileCtx;
                LOG(("succeeded!!"));
                return STATUS_SUCCESS;
            }
            else
            {
                WARN(("VBoxUsbFltCreate failed"));
            }
            vboxUsbMonCheckTermStuff();
        }
        else
        {
            WARN(("vboxUsbMonCheckInitStuff failed"));
        }
        VBoxUsbMonMemFree(pFileCtx);
    }
    else
    {
        WARN(("VBoxUsbMonMemAllocZ failed"));
        Status = STATUS_NO_MEMORY;
    }

    return Status;
}

static NTSTATUS vboxUsbMonContextClose(PVBOXUSBMONCTX pCtx)
{
    NTSTATUS Status = VBoxUsbFltClose(&pCtx->FltCtx);
    if (Status == STATUS_SUCCESS)
    {
        Status = vboxUsbMonCheckTermStuff();
        Assert(Status == STATUS_SUCCESS);
        /* ignore the failure */
        VBoxUsbMonMemFree(pCtx);
    }

    return Status;
}

static NTSTATUS _stdcall VBoxUsbMonClose(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    PFILE_OBJECT pFileObj = pStack->FileObject;
    Assert(pFileObj->FsContext);
    PVBOXUSBMONCTX pCtx = (PVBOXUSBMONCTX)pFileObj->FsContext;

    LOG(("VBoxUsbMonClose"));

    NTSTATUS Status = vboxUsbMonContextClose(pCtx);
    if (Status != STATUS_SUCCESS)
    {
        WARN(("vboxUsbMonContextClose failed, Status (0x%x), prefent unload", Status));
        if (!InterlockedExchange(&g_VBoxUsbMonGlobals.ulPreventUnloadOn, 1))
        {
            LOGREL(("ulPreventUnloadOn not set, preventing unload\n"));
            UNICODE_STRING UniName;
            PDEVICE_OBJECT pTmpDevObj;
            RtlInitUnicodeString(&UniName, USBMON_DEVICE_NAME_NT);
            NTSTATUS tmpStatus = IoGetDeviceObjectPointer(&UniName, FILE_ALL_ACCESS, &g_VBoxUsbMonGlobals.pPreventUnloadFileObj, &pTmpDevObj);
            AssertRelease(NT_SUCCESS(tmpStatus));
            AssertRelease(pTmpDevObj == pDevObj);
        }
        else
        {
            WARN(("ulPreventUnloadOn already set"));
        }
        LOG(("success!!"));
        Status = STATUS_SUCCESS;
    }
    pFileObj->FsContext = NULL;
    pIrp->IoStatus.Status = Status;
    pIrp->IoStatus.Information  = 0;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return Status;
}


static NTSTATUS _stdcall VBoxUsbMonCreate(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    PFILE_OBJECT pFileObj = pStack->FileObject;
    NTSTATUS Status;

    LOG(("VBoxUSBMonCreate"));

    if (pStack->Parameters.Create.Options & FILE_DIRECTORY_FILE)
    {
        WARN(("trying to open as a directory"));
        pIrp->IoStatus.Status = STATUS_NOT_A_DIRECTORY;
        pIrp->IoStatus.Information = 0;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
        return STATUS_NOT_A_DIRECTORY;
    }

    pFileObj->FsContext = NULL;
    PVBOXUSBMONCTX pCtx = NULL;
    Status = vboxUsbMonContextCreate(&pCtx);
    if (Status == STATUS_SUCCESS)
    {
        Assert(pCtx);
        pFileObj->FsContext = pCtx;
    }
    else
    {
        WARN(("vboxUsbMonContextCreate failed Status (0x%x)", Status));
    }

    pIrp->IoStatus.Status = Status;
    pIrp->IoStatus.Information  = 0;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return Status;
}

static int VBoxUsbMonSetNotifyEvent(PVBOXUSBMONCTX pContext, HANDLE hEvent)
{
    int rc = VBoxUsbFltSetNotifyEvent(&pContext->FltCtx, hEvent);
    return rc;
}

static int VBoxUsbMonFltAdd(PVBOXUSBMONCTX pContext, PUSBFILTER pFilter, uintptr_t *pId)
{
#ifdef VBOXUSBMON_DBG_NO_FILTERS
    static uintptr_t idDummy = 1;
    *pId = idDummy;
    ++idDummy;
    return VINF_SUCCESS;
#else
    int rc = VBoxUsbFltAdd(&pContext->FltCtx, pFilter, pId);
    return rc;
#endif
}

static int VBoxUsbMonFltRemove(PVBOXUSBMONCTX pContext, uintptr_t uId)
{
#ifdef VBOXUSBMON_DBG_NO_FILTERS
    return VINF_SUCCESS;
#else
    int rc = VBoxUsbFltRemove(&pContext->FltCtx, uId);
    return rc;
#endif
}

static NTSTATUS VBoxUsbMonRunFilters(PVBOXUSBMONCTX pContext)
{
    NTSTATUS Status = VBoxUsbFltFilterCheck(&pContext->FltCtx);
    return Status;
}

static NTSTATUS VBoxUsbMonGetDevice(PVBOXUSBMONCTX pContext, HVBOXUSBDEVUSR hDevice, PUSBSUP_GETDEV_MON pInfo)
{
    NTSTATUS Status = VBoxUsbFltGetDevice(&pContext->FltCtx, hDevice, pInfo);
    return Status;
}

static NTSTATUS vboxUsbMonIoctlDispatch(PVBOXUSBMONCTX pContext, ULONG Ctl, PVOID pvBuffer, ULONG cbInBuffer, ULONG cbOutBuffer, ULONG_PTR* pInfo)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR Info = 0;
    switch (Ctl)
    {
        case SUPUSBFLT_IOCTL_GET_VERSION:
        {
            PUSBSUP_VERSION pOut = (PUSBSUP_VERSION)pvBuffer;

            LOG(("SUPUSBFLT_IOCTL_GET_VERSION\n"));
            if (!pvBuffer || cbOutBuffer != sizeof(*pOut) || cbInBuffer != 0)
            {
                WARN(("SUPUSBFLT_IOCTL_GET_VERSION: Invalid input/output sizes. cbIn=%d expected %d. cbOut=%d expected %d.\n",
                        cbInBuffer, 0, cbOutBuffer, sizeof (*pOut)));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            pOut->u32Major = USBMON_MAJOR_VERSION;
            pOut->u32Minor = USBMON_MINOR_VERSION;
            Info = sizeof (*pOut);
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        case SUPUSBFLT_IOCTL_ADD_FILTER:
        {
            PUSBFILTER pFilter = (PUSBFILTER)pvBuffer;
            PUSBSUP_FLTADDOUT pOut = (PUSBSUP_FLTADDOUT)pvBuffer;
            uintptr_t uId = 0;
            int rc;
            if (RT_UNLIKELY(!pvBuffer || cbInBuffer != sizeof (*pFilter) || cbOutBuffer != sizeof (*pOut)))
            {
                WARN(("SUPUSBFLT_IOCTL_ADD_FILTER: Invalid input/output sizes. cbIn=%d expected %d. cbOut=%d expected %d.\n",
                        cbInBuffer, sizeof (*pFilter), cbOutBuffer, sizeof (*pOut)));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            rc = VBoxUsbMonFltAdd(pContext, pFilter, &uId);
            pOut->rc  = rc;
            pOut->uId = uId;
            Info = sizeof (*pOut);
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        case SUPUSBFLT_IOCTL_REMOVE_FILTER:
        {
            uintptr_t *pIn = (uintptr_t *)pvBuffer;
            int *pRc = (int *)pvBuffer;

            if (!pvBuffer || cbInBuffer != sizeof (*pIn) || (cbOutBuffer && cbOutBuffer != sizeof (*pRc)))
            {
                WARN(("SUPUSBFLT_IOCTL_REMOVE_FILTER: Invalid input/output sizes. cbIn=%d expected %d. cbOut=%d expected %d.\n",
                        cbInBuffer, sizeof (*pIn), cbOutBuffer, 0));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            LOG(("SUPUSBFLT_IOCTL_REMOVE_FILTER %x\n", *pIn));
            int rc = VBoxUsbMonFltRemove(pContext, *pIn);
            if (cbOutBuffer)
            {
                /* we've validated that already */
                Assert(cbOutBuffer == *pRc);
                *pRc = rc;
                Info = sizeof (*pRc);
            }
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        case SUPUSBFLT_IOCTL_RUN_FILTERS:
        {
            if (pvBuffer || cbInBuffer || cbOutBuffer)
            {
                WARN(("SUPUSBFLT_IOCTL_RUN_FILTERS: Invalid input/output sizes. cbIn=%d expected %d. cbOut=%d expected %d.\n",
                        cbInBuffer, 0, cbOutBuffer, 0));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            LOG(("SUPUSBFLT_IOCTL_RUN_FILTERS \n"));
            Status = VBoxUsbMonRunFilters(pContext);
            ASSERT_WARN(Status != STATUS_PENDING, ("status pending!"));
            break;
        }

        case SUPUSBFLT_IOCTL_GET_DEVICE:
        {
            HVBOXUSBDEVUSR hDevice = *((HVBOXUSBDEVUSR*)pvBuffer);
            PUSBSUP_GETDEV_MON pOut = (PUSBSUP_GETDEV_MON)pvBuffer;
            if (!pvBuffer || cbInBuffer != sizeof (hDevice) || cbOutBuffer < sizeof (*pOut))
            {
                WARN(("SUPUSBFLT_IOCTL_GET_DEVICE: Invalid input/output sizes. cbIn=%d expected %d. cbOut=%d expected >= %d.\n",
                        cbInBuffer, sizeof (hDevice), cbOutBuffer, sizeof (*pOut)));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Status = VBoxUsbMonGetDevice(pContext, hDevice, pOut);

            if (NT_SUCCESS(Status))
            {
                Info = sizeof (*pOut);
            }
            else
            {
                WARN(("VBoxUsbMonGetDevice fail 0x%x", Status));
            }
            break;
        }

        case SUPUSBFLT_IOCTL_SET_NOTIFY_EVENT:
        {
            PUSBSUP_SET_NOTIFY_EVENT pSne = (PUSBSUP_SET_NOTIFY_EVENT)pvBuffer;
            if (!pvBuffer || cbInBuffer != sizeof (*pSne) || cbOutBuffer != sizeof (*pSne))
            {
                WARN(("SUPUSBFLT_IOCTL_SET_NOTIFY_EVENT: Invalid input/output sizes. cbIn=%d expected %d. cbOut=%d expected %d.\n",
                        cbInBuffer, sizeof (*pSne), cbOutBuffer, sizeof (*pSne)));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            pSne->u.rc = VBoxUsbMonSetNotifyEvent(pContext, pSne->u.hEvent);
            Info = sizeof (*pSne);
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        default:
            WARN(("Unknown code 0x%x", Ctl));
            Status = STATUS_INVALID_PARAMETER;
            break;
    }

    ASSERT_WARN(Status != STATUS_PENDING, ("Status pending!"));

    *pInfo = Info;
    return Status;
}

static NTSTATUS _stdcall VBoxUsbMonDeviceControl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    ULONG_PTR Info = 0;
    NTSTATUS Status = IoAcquireRemoveLock(&g_VBoxUsbMonGlobals.RmLock, pDevObj);
    if (NT_SUCCESS(Status))
    {
        PIO_STACK_LOCATION pSl = IoGetCurrentIrpStackLocation(pIrp);
        PFILE_OBJECT pFileObj = pSl->FileObject;
        Assert(pFileObj);
        Assert(pFileObj->FsContext);
        PVBOXUSBMONCTX pCtx = (PVBOXUSBMONCTX)pFileObj->FsContext;
        Assert(pCtx);
        Status = vboxUsbMonIoctlDispatch(pCtx,
                    pSl->Parameters.DeviceIoControl.IoControlCode,
                    pIrp->AssociatedIrp.SystemBuffer,
                    pSl->Parameters.DeviceIoControl.InputBufferLength,
                    pSl->Parameters.DeviceIoControl.OutputBufferLength,
                    &Info);
        ASSERT_WARN(Status != STATUS_PENDING, ("Status pending"));

        IoReleaseRemoveLock(&g_VBoxUsbMonGlobals.RmLock, pDevObj);
    }
    else
    {
        WARN(("IoAcquireRemoveLock failed Status (0x%x)", Status));
    }

    pIrp->IoStatus.Information = Info;
    pIrp->IoStatus.Status = Status;
    IoCompleteRequest (pIrp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS vboxUsbMonInternalIoctlDispatch(ULONG Ctl, PVOID pvBuffer,  ULONG_PTR *pInfo)
{
    NTSTATUS Status = STATUS_SUCCESS;
    *pInfo = 0;
    switch (Ctl)
    {
        case VBOXUSBIDC_INTERNAL_IOCTL_GET_VERSION:
        {
            PVBOXUSBIDC_VERSION pOut = (PVBOXUSBIDC_VERSION)pvBuffer;

            LOG(("VBOXUSBIDC_INTERNAL_IOCTL_GET_VERSION\n"));
            if (!pvBuffer)
            {
                WARN(("VBOXUSBIDC_INTERNAL_IOCTL_GET_VERSION: Buffer is NULL\n"));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            pOut->u32Major = VBOXUSBIDC_VERSION_MAJOR;
            pOut->u32Minor = VBOXUSBIDC_VERSION_MINOR;
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        case VBOXUSBIDC_INTERNAL_IOCTL_PROXY_STARTUP:
        {
            PVBOXUSBIDC_PROXY_STARTUP pOut = (PVBOXUSBIDC_PROXY_STARTUP)pvBuffer;

            LOG(("VBOXUSBIDC_INTERNAL_IOCTL_PROXY_STARTUP\n"));
            if (!pvBuffer)
            {
                WARN(("VBOXUSBIDC_INTERNAL_IOCTL_PROXY_STARTUP: Buffer is NULL\n"));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            pOut->u.hDev = VBoxUsbFltProxyStarted(pOut->u.pPDO);
            ASSERT_WARN(pOut->u.hDev, ("zero hDev"));
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        case VBOXUSBIDC_INTERNAL_IOCTL_PROXY_TEARDOWN:
        {
            PVBOXUSBIDC_PROXY_TEARDOWN pOut = (PVBOXUSBIDC_PROXY_TEARDOWN)pvBuffer;

            LOG(("VBOXUSBIDC_INTERNAL_IOCTL_PROXY_TEARDOWN\n"));
            if (!pvBuffer)
            {
                WARN(("VBOXUSBIDC_INTERNAL_IOCTL_PROXY_TEARDOWN: Buffer is NULL\n"));
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            ASSERT_WARN(pOut->hDev, ("zero hDev"));
            VBoxUsbFltProxyStopped(pOut->hDev);
            ASSERT_WARN(Status == STATUS_SUCCESS, ("unexpected status, 0x%x", Status));
            break;
        }

        default:
        {
            WARN(("Unknown code 0x%x", Ctl));
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    return Status;
}

static NTSTATUS _stdcall VBoxUsbMonInternalDeviceControl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    ULONG_PTR Info = 0;
    NTSTATUS Status = IoAcquireRemoveLock(&g_VBoxUsbMonGlobals.RmLock, pDevObj);
    if (NT_SUCCESS(Status))
    {
        PIO_STACK_LOCATION pSl = IoGetCurrentIrpStackLocation(pIrp);
        Status = vboxUsbMonInternalIoctlDispatch(pSl->Parameters.DeviceIoControl.IoControlCode,
                        pSl->Parameters.Others.Argument1,
                        &Info);
        Assert(Status != STATUS_PENDING);

        IoReleaseRemoveLock(&g_VBoxUsbMonGlobals.RmLock, pDevObj);
    }

    pIrp->IoStatus.Information = Info;
    pIrp->IoStatus.Status = Status;
    IoCompleteRequest (pIrp, IO_NO_INCREMENT);
    return Status;
}

/**
 * Unload the driver.
 *
 * @param   pDrvObj     Driver object.
 */
static void _stdcall VBoxUsbMonUnload(PDRIVER_OBJECT pDrvObj)
{
    LOG(("VBoxUSBMonUnload pDrvObj (0x%p)\n", pDrvObj));

    IoReleaseRemoveLockAndWait(&g_VBoxUsbMonGlobals.RmLock, &g_VBoxUsbMonGlobals);

    Assert(!g_VBoxUsbMonGlobals.cOpens);

    UNICODE_STRING DosName;
    RtlInitUnicodeString(&DosName, USBMON_DEVICE_NAME_DOS);
    NTSTATUS rc = IoDeleteSymbolicLink(&DosName);

    IoDeleteDevice(g_VBoxUsbMonGlobals.pDevObj);

    /* cleanup the logger */
    PRTLOGGER pLogger = RTLogRelSetDefaultInstance(NULL);
    if (pLogger)
    {
        RTLogDestroy(pLogger);
    }
    pLogger = RTLogSetDefaultInstance(NULL);
    if (pLogger)
    {
        RTLogDestroy(pLogger);
    }
}

RT_C_DECLS_BEGIN
NTSTATUS _stdcall DriverEntry(PDRIVER_OBJECT pDrvObj, PUNICODE_STRING pRegPath);
RT_C_DECLS_END

/**
 * Driver entry point.
 *
 * @returns appropriate status code.
 * @param   pDrvObj     Pointer to driver object.
 * @param   pRegPath    Registry base path.
 */
NTSTATUS _stdcall DriverEntry(PDRIVER_OBJECT pDrvObj, PUNICODE_STRING pRegPath)
{
#ifdef VBOX_USB_WITH_VERBOSE_LOGGING
    RTLogGroupSettings(0, "+default.e.l.f.l2.l3");
    RTLogDestinations(0, "debugger");
#endif

    LOG(("VBoxUSBMon::DriverEntry\n"));

    memset (&g_VBoxUsbMonGlobals, 0, sizeof (g_VBoxUsbMonGlobals));
    KeInitializeEvent(&g_VBoxUsbMonGlobals.OpenSynchEvent, SynchronizationEvent, TRUE /* signaled */);
    IoInitializeRemoveLock(&g_VBoxUsbMonGlobals.RmLock, VBOXUSBMON_MEMTAG, 1, 100);
    UNICODE_STRING DevName;
    PDEVICE_OBJECT pDevObj;
    /* create the device */
    RtlInitUnicodeString(&DevName, USBMON_DEVICE_NAME_NT);
    NTSTATUS Status = IoAcquireRemoveLock(&g_VBoxUsbMonGlobals.RmLock, &g_VBoxUsbMonGlobals);
    if (NT_SUCCESS(Status))
    {
        Status = IoCreateDevice(pDrvObj, sizeof (VBOXUSBMONINS), &DevName, FILE_DEVICE_UNKNOWN, 0, FALSE, &pDevObj);
        if (NT_SUCCESS(Status))
        {
            UNICODE_STRING DosName;
            RtlInitUnicodeString(&DosName, USBMON_DEVICE_NAME_DOS);
            Status = IoCreateSymbolicLink(&DosName, &DevName);
            if (NT_SUCCESS(Status))
            {
                PVBOXUSBMONINS pDevExt = (PVBOXUSBMONINS)pDevObj->DeviceExtension;
                memset(pDevExt, 0, sizeof(*pDevExt));

                pDrvObj->DriverUnload = VBoxUsbMonUnload;
                pDrvObj->MajorFunction[IRP_MJ_CREATE] = VBoxUsbMonCreate;
                pDrvObj->MajorFunction[IRP_MJ_CLOSE] = VBoxUsbMonClose;
                pDrvObj->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VBoxUsbMonDeviceControl;
                pDrvObj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = VBoxUsbMonInternalDeviceControl;

                g_VBoxUsbMonGlobals.pDevObj = pDevObj;
                LOG(("VBoxUSBMon::DriverEntry returning STATUS_SUCCESS\n"));
                return STATUS_SUCCESS;
            }
            IoDeleteDevice(pDevObj);
        }
        IoReleaseRemoveLockAndWait(&g_VBoxUsbMonGlobals.RmLock, &g_VBoxUsbMonGlobals);
    }

    return Status;
}

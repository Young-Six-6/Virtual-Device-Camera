#include <ntddk.h>
#include <wdmsec.h>
#include <wdmguid.h>
#include <usbioctl.h>
#include "xusb_driver.h"

IO_COMPLETION_ROUTINE XusbReleaseControlRemoveLock;

#define XUSB_MAX_CHILD_DEVICES 256

static const GUID XusbChildDeviceClassGuid = {
    0x7a39372f,
    0xadd4,
    0x446a,
    { 0xac, 0x4e, 0x56, 0x17, 0x39, 0xfa, 0x4e, 0x2a }
};
static const UNICODE_STRING XusbChildDefaultSddl = RTL_CONSTANT_STRING(
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)");

static VOID XusbCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static NTSTATUS XusbReturnHubName(
    _In_ PXUSB_PARENT_EXTENSION Parent,
    _Inout_ PIRP Irp,
    _Out_ PULONG_PTR Information)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PUSB_HUB_NAME output = (PUSB_HUB_NAME)Irp->AssociatedIrp.SystemBuffer;
    SIZE_T requiredLength;

    *Information = 0;
    if (Parent->Role != XusbRoleRootHub || Parent->InterfaceName.Buffer == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (output == NULL ||
        outputLength < (ULONG)FIELD_OFFSET(USB_HUB_NAME, HubName)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    requiredLength = FIELD_OFFSET(USB_HUB_NAME, HubName) +
        Parent->InterfaceName.Length + sizeof(WCHAR);
    if (requiredLength > MAXULONG) {
        return STATUS_INTEGER_OVERFLOW;
    }
    output->ActualLength = (ULONG)requiredLength;
    if ((SIZE_T)outputLength < requiredLength) {
        *Information = FIELD_OFFSET(USB_HUB_NAME, HubName);
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(
        output->HubName,
        Parent->InterfaceName.Buffer,
        Parent->InterfaceName.Length);
    output->HubName[Parent->InterfaceName.Length / sizeof(WCHAR)] = UNICODE_NULL;
    *Information = requiredLength;
    return STATUS_SUCCESS;
}

NTSTATUS XusbReleaseControlRemoveLock(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    NT_ASSERT(Context != NULL);
    if (Context != NULL) {
        IoReleaseRemoveLock((PIO_REMOVE_LOCK)Context, Irp);
    }
    return STATUS_CONTINUE_COMPLETION;
}

static VOID XusbFreeUnicodeString(_Inout_ PUNICODE_STRING String)
{
    if (String->Buffer != NULL) {
        ExFreePoolWithTag(String->Buffer, XUSB_POOL_TAG);
        RtlZeroMemory(String, sizeof(*String));
    }
}

_Success_(NT_SUCCESS(return)) static NTSTATUS XusbCopyString(
    _In_reads_(MaxChars) PCWSTR Source,
    _In_ ULONG MaxChars,
    _In_ BOOLEAN AllowEmpty,
    _Out_ PUNICODE_STRING Destination)
{
    ULONG length = 0;
    SIZE_T bytes;

    while (length < MaxChars && Source[length] != UNICODE_NULL) {
        ++length;
    }
    if (length == MaxChars || (!AllowEmpty && length == 0) ||
        length > (MAXUSHORT / sizeof(WCHAR)) - 1) {
        return STATUS_INVALID_PARAMETER;
    }

    bytes = ((SIZE_T)length + 1) * sizeof(WCHAR);
    Destination->Buffer = ExAllocatePool2(POOL_FLAG_PAGED, bytes, XUSB_POOL_TAG);
    if (Destination->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Destination->Buffer, Source, bytes);
    Destination->Length = (USHORT)(length * sizeof(WCHAR));
    Destination->MaximumLength = (USHORT)bytes;
    return STATUS_SUCCESS;
}

_Success_(NT_SUCCESS(return)) static NTSTATUS XusbCopyMultiSz(
    _In_reads_(MaxChars) PCWSTR Source,
    _In_ ULONG MaxChars,
    _In_ BOOLEAN AllowEmpty,
    _Out_ PUNICODE_STRING Destination)
{
    ULONG index;
    SIZE_T bytes;

    if (AllowEmpty && Source[0] == UNICODE_NULL &&
        (MaxChars < 2 || Source[1] == UNICODE_NULL)) {
        Destination->Buffer = ExAllocatePool2(
            POOL_FLAG_PAGED,
            2 * sizeof(WCHAR),
            XUSB_POOL_TAG);
        if (Destination->Buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Destination->Buffer[0] = UNICODE_NULL;
        Destination->Buffer[1] = UNICODE_NULL;
        Destination->Length = sizeof(WCHAR);
        Destination->MaximumLength = 2 * sizeof(WCHAR);
        return STATUS_SUCCESS;
    }

    if (Source[0] == UNICODE_NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    for (index = 1; index < MaxChars; ++index) {
        if (Source[index] == UNICODE_NULL &&
            index + 1 < MaxChars && Source[index + 1] == UNICODE_NULL) {
            break;
        }
    }
    if (index + 1 >= MaxChars ||
        index + 2 > (MAXUSHORT / sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }

    bytes = ((SIZE_T)index + 2) * sizeof(WCHAR);
    Destination->Buffer = ExAllocatePool2(POOL_FLAG_PAGED, bytes, XUSB_POOL_TAG);
    if (Destination->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Destination->Buffer, Source, bytes);
    Destination->Length = (USHORT)(bytes - sizeof(WCHAR));
    Destination->MaximumLength = (USHORT)bytes;
    return STATUS_SUCCESS;
}

static VOID XusbFreeChildStrings(_Inout_ PXUSB_CHILD_EXTENSION Child)
{
    XusbFreeUnicodeString(&Child->DeviceId);
    XusbFreeUnicodeString(&Child->HardwareIds);
    XusbFreeUnicodeString(&Child->CompatibleIds);
}

static VOID XusbDisposeUnreportedChild(_In_ PDEVICE_OBJECT ChildPdo)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)ChildPdo->DeviceExtension;
    PDEVICE_OBJECT parentDevice = child->ParentDevice;
    PKEVENT removeEvent = child->RemoveEvent;
    PKSEMAPHORE semaphore = child->Semaphore;
    NTSTATUS status;

    /*
     * Stop new work before draining the queue. The sentinel remove-lock tag
     * closes the dispatch window and the wait covers work that already passed
     * its initial stopping check but has not reached the CSQ yet.
     */
    XusbStopTransactions(child);
    status = IoAcquireRemoveLock(&child->RemoveLock, ChildPdo);
    if (!NT_SUCCESS(status)) {
        NT_ASSERT(FALSE);
        return;
    }
    XusbCancelTransactions(child);
    IoReleaseRemoveLockAndWait(&child->RemoveLock, ChildPdo);

    XusbReleaseChildConfiguration(child);
    child->RemoveEvent = NULL;
    child->Semaphore = NULL;
    if (removeEvent != NULL) {
        KeSetEvent(removeEvent, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(removeEvent);
    }
    if (semaphore != NULL) {
        ObDereferenceObject(semaphore);
    }
    XusbFreeChildStrings(child);
    ObDereferenceObject(child->PhysicalDeviceObject);
    IoDeleteDevice(ChildPdo);
    ObDereferenceObject(parentDevice);
}

static PXUSB_FILE_CONTEXT XusbGetFileContext(_In_ PFILE_OBJECT FileObject)
{
    PXUSB_FILE_CONTEXT context;

    if (FileObject == NULL) {
        return NULL;
    }
    context = (PXUSB_FILE_CONTEXT)FileObject->FsContext;
    if (context == NULL || context->Signature != XUSB_FILE_SIGNATURE) {
        return NULL;
    }
    return context;
}

static BOOLEAN XusbDetachChildFromFile(_In_ PXUSB_PARENT_EXTENSION Parent, _In_ PFILE_OBJECT FileObject)
{
    PXUSB_FILE_CONTEXT fileContext = XusbGetFileContext(FileObject);
    PDEVICE_OBJECT childPdo;
    PDEVICE_OBJECT disposePdo = NULL;
    BOOLEAN invalidate = FALSE;

    if (fileContext == NULL) {
        return FALSE;
    }
    ExAcquireFastMutex(&Parent->ChildMutex);
    childPdo = fileContext->ChildPdo;
    fileContext->ChildPdo = NULL;
    if (childPdo != NULL) {
        PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)childPdo->DeviceExtension;
        if (child->Signature == XUSB_CHILD_SIGNATURE && child->Present && !child->Removed) {
            child->Present = FALSE;
            XusbStopTransactions(child);
            invalidate = TRUE;
            if (!child->Reported) {
                child->Removed = TRUE;
                child->Signature = 0;
                if (child->Linked) {
                    RemoveEntryList(&child->Link);
                    InitializeListHead(&child->Link);
                    child->Linked = FALSE;
                }
                disposePdo = childPdo;
            }
        }
    }
    ExReleaseFastMutex(&Parent->ChildMutex);
    if (childPdo != NULL && disposePdo == NULL) {
        PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)childPdo->DeviceExtension;
        if (child->Signature == XUSB_CHILD_SIGNATURE) {
            XusbCancelTransactions(child);
        }
    }
    if (disposePdo != NULL) {
        XusbDisposeUnreportedChild(disposePdo);
        ObDereferenceObject(childPdo);
    } else if (childPdo != NULL) {
        ObDereferenceObject(childPdo);
    }
    if (invalidate && disposePdo == NULL &&
        InterlockedCompareExchange(&Parent->Removing, 0, 0) == 0) {
        IoInvalidateDeviceRelations(
            Parent->PhysicalDeviceObject,
            BusRelations);
    }
    return childPdo != NULL;
}

NTSTATUS XusbCreateFile(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PXUSB_FILE_CONTEXT context;
    NTSTATUS status;

    if (parent->Signature != XUSB_PARENT_SIGNATURE ||
        (parent->Role != XusbRoleUsbOnlyEnumerator && parent->Role != XusbRoleRootHub)) {
        XusbCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    status = IoAcquireRemoveLock(&parent->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        XusbCompleteIrp(Irp, status, 0);
        return status;
    }
    if (InterlockedCompareExchange(&parent->Removing, 0, 0) != 0) {
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
        return STATUS_DELETE_PENDING;
    }
    if (stack->FileObject->FsContext != NULL) {
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE, 0);
        return STATUS_INVALID_DEVICE_STATE;
    }
    context = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*context), XUSB_POOL_TAG);
    if (context == NULL) {
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(context, sizeof(*context));
    context->Signature = XUSB_FILE_SIGNATURE;
    ObReferenceObject(DeviceObject);
    stack->FileObject->FsContext = context;
    IoReleaseRemoveLock(&parent->RemoveLock, Irp);
    XusbCompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS XusbCleanupFile(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    if (parent->Signature == XUSB_PARENT_SIGNATURE) {
        XusbDetachChildFromFile(parent, stack->FileObject);
    }
    XusbCompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS XusbCloseFile(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PXUSB_FILE_CONTEXT context = XusbGetFileContext(stack->FileObject);

    if (parent->Signature == XUSB_PARENT_SIGNATURE && context != NULL) {
        XusbDetachChildFromFile(parent, stack->FileObject);
        stack->FileObject->FsContext = NULL;
        context->Signature = 0;
        ExFreePoolWithTag(context, XUSB_POOL_TAG);
        ObDereferenceObject(DeviceObject);
    }
    XusbCompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS XusbReferenceUserObjects(
    _In_ const ioctl_pdo_create_t* Request,
    _Outptr_ PKSEMAPHORE* Semaphore,
    _Outptr_result_maybenull_ PKEVENT* RemoveEvent)
{
    NTSTATUS status;

    *Semaphore = NULL;
    *RemoveEvent = NULL;
    if (Request->hSemaphore == 0 || Request->hSemaphore > MAXULONG_PTR) {
        return STATUS_INVALID_HANDLE;
    }
    status = ObReferenceObjectByHandle(
        (HANDLE)(ULONG_PTR)Request->hSemaphore,
        SEMAPHORE_MODIFY_STATE,
        *ExSemaphoreObjectType,
        UserMode,
        (PVOID*)Semaphore,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (Request->hRemoveEvent != 0) {
        if (Request->hRemoveEvent > MAXULONG_PTR) {
            ObDereferenceObject(*Semaphore);
            *Semaphore = NULL;
            return STATUS_INVALID_HANDLE;
        }
        status = ObReferenceObjectByHandle(
            (HANDLE)(ULONG_PTR)Request->hRemoveEvent,
            EVENT_MODIFY_STATE,
            *ExEventObjectType,
            UserMode,
            (PVOID*)RemoveEvent,
            NULL);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(*Semaphore);
            *Semaphore = NULL;
            return status;
        }
    }
    return STATUS_SUCCESS;
}

static LONG XusbAllocateBusAddress(_Inout_ PXUSB_PARENT_EXTENSION Parent)
{
    ULONG candidate;
    PLIST_ENTRY entry;
    BOOLEAN found;

    for (;;) {
        candidate = Parent->NextBusAddress++;
        if (Parent->NextBusAddress == 0 || Parent->NextBusAddress > MAXLONG) {
            Parent->NextBusAddress = 1;
        }
        if (candidate == 0 || candidate > MAXLONG) {
            continue;
        }
        found = TRUE;
        for (entry = Parent->Children.Flink; entry != &Parent->Children; entry = entry->Flink) {
            PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(
                entry,
                XUSB_CHILD_EXTENSION,
                Link);
            if ((ULONG)child->BusAddress == candidate) {
                found = FALSE;
                break;
            }
        }
        if (found) {
            return (LONG)candidate;
        }
    }
}

NTSTATUS XusbCreateRoleChild(_In_ PDEVICE_OBJECT DeviceObject)
{
    static const WCHAR rootHubId[] = L"vFxsUSB\\vRootHUB&Virtual&Fanxiushu";
    static const WCHAR rootHubHardwareIds[] = L"vFxsUSB\\vRootHUB&Virtual&Fanxiushu\0";
    static const WCHAR hostControllerId[] =
        L"PCI\\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88";
    static const WCHAR hostControllerHardwareIds[] =
        L"PCI\\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88\0";
    static const WCHAR emptyMultiSz[] = { UNICODE_NULL, UNICODE_NULL };
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PCWSTR childId;
    ULONG childIdChars;
    PCWSTR hardwareIds;
    ULONG hardwareIdChars;
    PDEVICE_OBJECT childPdo = NULL;
    PXUSB_CHILD_EXTENSION child;
    PLIST_ENTRY entry;
    NTSTATUS status;
    BOOLEAN transactionsInitialized = FALSE;

    if (parent->Signature != XUSB_PARENT_SIGNATURE) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    switch (parent->Role) {
    case XusbRoleHostController:
        childId = rootHubId;
        childIdChars = RTL_NUMBER_OF(rootHubId);
        hardwareIds = rootHubHardwareIds;
        hardwareIdChars = RTL_NUMBER_OF(rootHubHardwareIds);
        break;
    case XusbRoleUsbTreeEnumerator:
        childId = hostControllerId;
        childIdChars = RTL_NUMBER_OF(hostControllerId);
        hardwareIds = hostControllerHardwareIds;
        hardwareIdChars = RTL_NUMBER_OF(hostControllerHardwareIds);
        break;
    default:
        return STATUS_SUCCESS;
    }

    ExAcquireFastMutex(&parent->ChildMutex);
    for (entry = parent->Children.Flink; entry != &parent->Children; entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION existing = CONTAINING_RECORD(
            entry,
            XUSB_CHILD_EXTENSION,
            Link);
        if (existing->Internal && existing->Present && !existing->Removed) {
            ExReleaseFastMutex(&parent->ChildMutex);
            return STATUS_SUCCESS;
        }
    }
    if (InterlockedCompareExchange(&parent->Removing, 0, 0) != 0) {
        ExReleaseFastMutex(&parent->ChildMutex);
        return STATUS_DELETE_PENDING;
    }
    ExReleaseFastMutex(&parent->ChildMutex);

    status = IoCreateDeviceSecure(
        DeviceObject->DriverObject,
        sizeof(XUSB_CHILD_EXTENSION),
        NULL,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &XusbChildDefaultSddl,
        &XusbChildDeviceClassGuid,
        &childPdo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    child = (PXUSB_CHILD_EXTENSION)childPdo->DeviceExtension;
    RtlZeroMemory(child, sizeof(*child));
    child->Signature = XUSB_CHILD_SIGNATURE;
    child->Self = childPdo;
    child->ParentDevice = DeviceObject;
    child->PhysicalDeviceObject = parent->PhysicalDeviceObject;
    InitializeListHead(&child->RemovalLink);
    child->Internal = TRUE;
    child->Present = TRUE;
    child->DevicePowerState = PowerDeviceD0;
    ExInitializeFastMutex(&child->ConfigurationMutex);
    ObReferenceObject(DeviceObject);
    ObReferenceObject(child->PhysicalDeviceObject);

    status = XusbCopyString(childId, childIdChars, FALSE, &child->DeviceId);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = XusbCopyMultiSz(hardwareIds, hardwareIdChars, FALSE, &child->HardwareIds);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = XusbCopyMultiSz(
        emptyMultiSz,
        RTL_NUMBER_OF(emptyMultiSz),
        TRUE,
        &child->CompatibleIds);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    IoInitializeRemoveLock(&child->RemoveLock, 'rUXV', 0, 0);
    status = XusbInitializeTransactions(child);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    transactionsInitialized = TRUE;
    childPdo->Flags |= DO_BUS_ENUMERATED_DEVICE | DO_POWER_PAGABLE;

    ExAcquireFastMutex(&parent->ChildMutex);
    if (InterlockedCompareExchange(&parent->Removing, 0, 0) != 0) {
        ExReleaseFastMutex(&parent->ChildMutex);
        status = STATUS_DELETE_PENDING;
        goto Exit;
    }
    for (entry = parent->Children.Flink; entry != &parent->Children; entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION existing = CONTAINING_RECORD(
            entry,
            XUSB_CHILD_EXTENSION,
            Link);
        if (existing->Internal && existing->Present && !existing->Removed) {
            ExReleaseFastMutex(&parent->ChildMutex);
            status = STATUS_SUCCESS;
            goto Exit;
        }
    }

    child->BusAddress = XusbAllocateBusAddress(parent);
    InsertTailList(&parent->Children, &child->Link);
    child->Linked = TRUE;
    ObReferenceObject(childPdo);
    ExReleaseFastMutex(&parent->ChildMutex);

    childPdo->Flags &= ~DO_DEVICE_INITIALIZING;
    IoInvalidateDeviceRelations(parent->PhysicalDeviceObject, BusRelations);
    return STATUS_SUCCESS;

Exit:
    if (transactionsInitialized) {
        NTSTATUS cleanupStatus;
        XusbStopTransactions(child);
        cleanupStatus = IoAcquireRemoveLock(&child->RemoveLock, childPdo);
        if (NT_SUCCESS(cleanupStatus)) {
            XusbCancelTransactions(child);
            IoReleaseRemoveLockAndWait(&child->RemoveLock, childPdo);
        }
    }
    XusbReleaseChildConfiguration(child);
    XusbFreeChildStrings(child);
    ObDereferenceObject(DeviceObject);
    ObDereferenceObject(child->PhysicalDeviceObject);
    IoDeleteDevice(childPdo);
    return status;
}

static NTSTATUS XusbCreateChild(
    _In_ PDEVICE_OBJECT ParentDevice,
    _In_ PXUSB_FILE_CONTEXT FileContext,
    _In_ const ioctl_pdo_create_t* Request,
    _Out_ PLONG BusAddress)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)ParentDevice->DeviceExtension;
    PDEVICE_OBJECT childPdo = NULL;
    PXUSB_CHILD_EXTENSION child;
    NTSTATUS status;
    BOOLEAN transactionsInitialized = FALSE;
    ULONG childCount = 0;

    if (parent->Role != XusbRoleUsbOnlyEnumerator &&
        parent->Role != XusbRoleRootHub &&
        parent->Role != XusbRoleUsbTreeEnumerator) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = IoCreateDeviceSecure(
        ParentDevice->DriverObject,
        sizeof(XUSB_CHILD_EXTENSION),
        NULL,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &XusbChildDefaultSddl,
        &XusbChildDeviceClassGuid,
        &childPdo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    child = (PXUSB_CHILD_EXTENSION)childPdo->DeviceExtension;
    RtlZeroMemory(child, sizeof(*child));
    child->Signature = XUSB_CHILD_SIGNATURE;
    child->Self = childPdo;
    child->ParentDevice = ParentDevice;
    child->PhysicalDeviceObject = parent->PhysicalDeviceObject;
    InitializeListHead(&child->RemovalLink);
    child->Present = TRUE;
    child->DevicePowerState = PowerDeviceD0;
    ExInitializeFastMutex(&child->ConfigurationMutex);
    ObReferenceObject(ParentDevice);
    ObReferenceObject(child->PhysicalDeviceObject);

    status = XusbCopyString(Request->device_id, HW_IDS_COUNT, FALSE, &child->DeviceId);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = XusbCopyMultiSz(Request->hardware_ids, HW_IDS_COUNT, FALSE, &child->HardwareIds);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = XusbCopyMultiSz(
        Request->compatible_ids,
        HW_IDS_COUNT,
        TRUE,
        &child->CompatibleIds);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = XusbReferenceUserObjects(Request, &child->Semaphore, &child->RemoveEvent);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    IoInitializeRemoveLock(&child->RemoveLock, 'rUXV', 0, 0);
    status = XusbInitializeTransactions(child);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    transactionsInitialized = TRUE;

    childPdo->Flags |= DO_BUS_ENUMERATED_DEVICE | DO_POWER_PAGABLE;
    ExAcquireFastMutex(&parent->ChildMutex);
    for (PLIST_ENTRY entry = parent->Children.Flink;
        entry != &parent->Children;
        entry = entry->Flink) {
        ++childCount;
    }
    if (FileContext->ChildPdo != NULL ||
        InterlockedCompareExchange(&parent->Removing, 0, 0) != 0 ||
        childCount >= XUSB_MAX_CHILD_DEVICES) {
        BOOLEAN fileBusy = FileContext->ChildPdo != NULL;
        BOOLEAN capacityExceeded = childCount >= XUSB_MAX_CHILD_DEVICES;
        ExReleaseFastMutex(&parent->ChildMutex);
        status = fileBusy ? STATUS_DEVICE_BUSY :
            (capacityExceeded ? STATUS_INSUFFICIENT_RESOURCES : STATUS_DELETE_PENDING);
        goto Exit;
    }
    child->BusAddress = XusbAllocateBusAddress(parent);
    InsertTailList(&parent->Children, &child->Link);
    child->Linked = TRUE;
    FileContext->ChildPdo = childPdo;
    ObReferenceObject(childPdo);
    *BusAddress = child->BusAddress;
    ExReleaseFastMutex(&parent->ChildMutex);

    childPdo->Flags &= ~DO_DEVICE_INITIALIZING;
    IoInvalidateDeviceRelations(parent->PhysicalDeviceObject, BusRelations);
    return STATUS_SUCCESS;

Exit:
    if (transactionsInitialized) {
        NTSTATUS cleanupStatus;
        XusbStopTransactions(child);
        cleanupStatus = IoAcquireRemoveLock(&child->RemoveLock, childPdo);
        if (NT_SUCCESS(cleanupStatus)) {
            XusbCancelTransactions(child);
            IoReleaseRemoveLockAndWait(&child->RemoveLock, childPdo);
        }
        XusbReleaseChildConfiguration(child);
    }
    if (child->RemoveEvent != NULL) {
        ObDereferenceObject(child->RemoveEvent);
    }
    if (child->Semaphore != NULL) {
        ObDereferenceObject(child->Semaphore);
    }
    XusbFreeChildStrings(child);
    ObDereferenceObject(ParentDevice);
    ObDereferenceObject(child->PhysicalDeviceObject);
    IoDeleteDevice(childPdo);
    return status;
}

NTSTATUS XusbDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inputBytes = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputBytes = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PXUSB_FILE_CONTEXT fileContext;
    NTSTATUS status;
    ULONG_PTR information = 0;

    if (parent->Signature != XUSB_PARENT_SIGNATURE) {
        XusbCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE, 0);
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = IoAcquireRemoveLock(&parent->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        XusbCompleteIrp(Irp, status, 0);
        return status;
    }
    if (InterlockedCompareExchange(&parent->Removing, 0, 0) != 0) {
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
        return STATUS_DELETE_PENDING;
    }
    fileContext = XusbGetFileContext(stack->FileObject);
    if (fileContext == NULL) {
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE, 0);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (code == IOCTL_INTERNAL_USB_GET_HUB_NAME && parent->Role == XusbRoleRootHub) {
        status = XusbReturnHubName(parent, Irp, &information);
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, status, information);
        return status;
    }
    if (code == IOCTL_XUSB_QUERY_DRIVER_INFO) {
        if (inputBytes != 0 || outputBytes < sizeof(xusb_driver_info_t) ||
            Irp->AssociatedIrp.SystemBuffer == NULL) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            const xusb_driver_info_t info = {
                XUSB_DRIVER_INFO_SIGNATURE,
                sizeof(xusb_driver_info_t),
                XUSB_DRIVER_KIND_RECONSTRUCTED,
                XUSB_DRIVER_ABI_VERSION
            };
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &info, sizeof(info));
            status = STATUS_SUCCESS;
            information = sizeof(info);
        }
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, status, information);
        return status;
    }
    if (code != IOCTL_PDO_ADD && code != IOCTL_PDO_REMOVE) {
        if (code == IOCTL_BEGIN_TRANS_USB_DATA || code == IOCTL_END_TRANS_USB_DATA) {
            PDEVICE_OBJECT childPdo = NULL;
            ExAcquireFastMutex(&parent->ChildMutex);
            childPdo = fileContext->ChildPdo;
            if (childPdo != NULL) {
                PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)childPdo->DeviceExtension;
                if (child->Signature == XUSB_CHILD_SIGNATURE && child->Present && !child->Removed) {
                    ObReferenceObject(childPdo);
                } else {
                    childPdo = NULL;
                }
            }
            ExReleaseFastMutex(&parent->ChildMutex);
            if (childPdo == NULL) {
                status = STATUS_DEVICE_NOT_CONNECTED;
                IoReleaseRemoveLock(&parent->RemoveLock, Irp);
                XusbCompleteIrp(Irp, status, 0);
                return status;
            } else {
                IoReleaseRemoveLock(&parent->RemoveLock, Irp);
                status = code == IOCTL_BEGIN_TRANS_USB_DATA
                    ? XusbBeginUsbTransaction(childPdo, Irp)
                    : XusbEndUsbTransaction(childPdo, Irp);
                ObDereferenceObject(childPdo);
                return status;
            }
        }
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(
            Irp,
            XusbReleaseControlRemoveLock,
            &parent->RemoveLock,
            TRUE,
            TRUE,
            TRUE);
        return IoCallDriver(parent->LowerDevice, Irp);
    }

    if (code == IOCTL_PDO_ADD) {
        const ioctl_pdo_create_t* request;
        LONG address = 0;

        if (Irp->AssociatedIrp.SystemBuffer == NULL || inputBytes < sizeof(*request) ||
            outputBytes < sizeof(address)) {
            IoReleaseRemoveLock(&parent->RemoveLock, Irp);
            XusbCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            return STATUS_BUFFER_TOO_SMALL;
        }
        request = (const ioctl_pdo_create_t*)Irp->AssociatedIrp.SystemBuffer;
        status = XusbCreateChild(DeviceObject, fileContext, request, &address);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &address, sizeof(address));
            information = sizeof(address);
        }
    } else {
        if (inputBytes != 0 && Irp->AssociatedIrp.SystemBuffer == NULL) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            status = XusbDetachChildFromFile(parent, stack->FileObject)
                ? STATUS_SUCCESS
                : STATUS_NO_SUCH_DEVICE;
        }
    }

    IoReleaseRemoveLock(&parent->RemoveLock, Irp);
    XusbCompleteIrp(Irp, status, information);
    return status;
}

#pragma warning(push)
#pragma warning(disable: 6014) // The PnP manager owns the returned DEVICE_RELATIONS allocation.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS XusbEnumerateChildren(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PLIST_ENTRY entry;
    ULONG count = 0;
    ULONG index = 0;
    SIZE_T bytes;
    PDEVICE_RELATIONS relations;

    ExAcquireFastMutex(&parent->ChildMutex);
    for (entry = parent->Children.Flink; entry != &parent->Children; entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(entry, XUSB_CHILD_EXTENSION, Link);
        if (child->Present && !child->Removed) {
            ++count;
        }
    }
    if (count > (MAXULONG - FIELD_OFFSET(DEVICE_RELATIONS, Objects)) / sizeof(PDEVICE_OBJECT)) {
        ExReleaseFastMutex(&parent->ChildMutex);
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INTEGER_OVERFLOW, 0);
        return STATUS_INTEGER_OVERFLOW;
    }
    bytes = FIELD_OFFSET(DEVICE_RELATIONS, Objects) +
        ((SIZE_T)count * sizeof(PDEVICE_OBJECT));
    relations = ExAllocatePool2(POOL_FLAG_PAGED, bytes, XUSB_POOL_TAG);
    if (relations == NULL) {
        ExReleaseFastMutex(&parent->ChildMutex);
        IoReleaseRemoveLock(&parent->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    relations->Count = count;
    for (entry = parent->Children.Flink; entry != &parent->Children; entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(entry, XUSB_CHILD_EXTENSION, Link);
        if (child->Present && !child->Removed) {
            ObReferenceObject(child->Self);
            child->Reported = TRUE;
            relations->Objects[index++] = child->Self;
        }
    }
    ExReleaseFastMutex(&parent->ChildMutex);

    /* Replace the parent bus list with the PDOs reported by this enumerator. */
    IoReleaseRemoveLock(&parent->RemoveLock, Irp);
    XusbCompleteIrp(Irp, STATUS_SUCCESS, (ULONG_PTR)relations);
    return STATUS_SUCCESS;
}
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 6014) // The PnP manager owns the returned ID allocation.
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS XusbReturnPnpId(
    _Inout_ PIRP Irp,
    _In_ PCUNICODE_STRING Id,
    _Inout_ PIO_REMOVE_LOCK RemoveLock)
{
    PWSTR copy;

    copy = ExAllocatePool2(POOL_FLAG_PAGED, Id->MaximumLength, XUSB_POOL_TAG);
    if (copy == NULL) {
        IoReleaseRemoveLock(RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(copy, Id->Buffer, Id->MaximumLength);
    IoReleaseRemoveLock(RemoveLock, Irp);
    XusbCompleteIrp(Irp, STATUS_SUCCESS, (ULONG_PTR)copy);
    return STATUS_SUCCESS;
}
#pragma warning(pop)

static PDEVICE_OBJECT XusbRemoveChild(_In_ PDEVICE_OBJECT DeviceObject)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    PDEVICE_OBJECT parentDevice = child->ParentDevice;
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)parentDevice->DeviceExtension;
    PKEVENT removeEvent;
    PKSEMAPHORE semaphore;

    ExAcquireFastMutex(&parent->ChildMutex);
    child->Present = FALSE;
    child->Removed = TRUE;
    XusbStopTransactions(child);
    if (child->Linked) {
        RemoveEntryList(&child->Link);
        InitializeListHead(&child->Link);
        child->Linked = FALSE;
    }
    removeEvent = child->RemoveEvent;
    semaphore = child->Semaphore;
    child->RemoveEvent = NULL;
    child->Semaphore = NULL;
    child->Signature = 0;
    ExReleaseFastMutex(&parent->ChildMutex);

    XusbCancelTransactions(child);
    XusbReleaseChildConfiguration(child);

    if (removeEvent != NULL) {
        KeSetEvent(removeEvent, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(removeEvent);
    }
    if (semaphore != NULL) {
        ObDereferenceObject(semaphore);
    }
    XusbFreeChildStrings(child);
    ObDereferenceObject(child->PhysicalDeviceObject);
    return parentDevice;
}

#pragma warning(push)
#pragma warning(disable: 6014) // PnP owns the returned relation and ID allocations.
NTSTATUS XusbDispatchChildPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;

    status = IoAcquireRemoveLock(&child->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        XusbCompleteIrp(Irp, status, 0);
        return status;
    }
    if (child->Signature != XUSB_CHILD_SIGNATURE) {
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
        return STATUS_DELETE_PENDING;
    }
    Irp->IoStatus.Information = 0;
    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        {
        POWER_STATE powerState;
        child->DevicePowerState = PowerDeviceD0;
        powerState.DeviceState = child->DevicePowerState;
        PoSetPowerState(DeviceObject, DevicePowerState, powerState);
        break;
        }
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        break;

    case IRP_MN_SURPRISE_REMOVAL: {
        PXUSB_PARENT_EXTENSION parent =
            (PXUSB_PARENT_EXTENSION)child->ParentDevice->DeviceExtension;
        BOOLEAN changed = FALSE;
        ExAcquireFastMutex(&parent->ChildMutex);
        if (child->Present && !child->Removed) {
            child->Present = FALSE;
            XusbStopTransactions(child);
            changed = TRUE;
        }
        ExReleaseFastMutex(&parent->ChildMutex);
        if (changed) {
            XusbCancelTransactions(child);
        }
        if (changed && InterlockedCompareExchange(&parent->Removing, 0, 0) == 0) {
            IoInvalidateDeviceRelations(parent->PhysicalDeviceObject, BusRelations);
        }
        break;
    }

    case IRP_MN_QUERY_ID:
        switch (stack->Parameters.QueryId.IdType) {
        case BusQueryDeviceID:
            return XusbReturnPnpId(Irp, &child->DeviceId, &child->RemoveLock);
        case BusQueryHardwareIDs:
            return XusbReturnPnpId(Irp, &child->HardwareIds, &child->RemoveLock);
        case BusQueryCompatibleIDs:
            return XusbReturnPnpId(Irp, &child->CompatibleIds, &child->RemoveLock);
        case BusQueryInstanceID: {
            WCHAR buffer[16];
            UNICODE_STRING instanceId;

            instanceId.Buffer = buffer;
            instanceId.Length = 0;
            instanceId.MaximumLength = sizeof(buffer);
            status = RtlIntegerToUnicodeString(
                (ULONG)child->BusAddress,
                10,
                &instanceId);
            if (!NT_SUCCESS(status)) {
                break;
            }
            instanceId.MaximumLength = (USHORT)(instanceId.Length + sizeof(WCHAR));
            buffer[instanceId.Length / sizeof(WCHAR)] = UNICODE_NULL;
            return XusbReturnPnpId(Irp, &instanceId, &child->RemoveLock);
        }
        default:
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        break;

    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION busInformation = ExAllocatePool2(
            POOL_FLAG_PAGED,
            sizeof(*busInformation),
            XUSB_POOL_TAG);
        if (busInformation == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        busInformation->BusTypeGuid = GUID_BUS_TYPE_USB;
        busInformation->LegacyBusType = PNPBus;
        busInformation->BusNumber = 0;
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_SUCCESS, (ULONG_PTR)busInformation);
        return STATUS_SUCCESS;
    }

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS relations = ExAllocatePool2(
                POOL_FLAG_PAGED,
                FIELD_OFFSET(DEVICE_RELATIONS, Objects) + sizeof(PDEVICE_OBJECT),
                XUSB_POOL_TAG);
            if (relations == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            relations->Count = 1;
            ObReferenceObject(DeviceObject);
            relations->Objects[0] = DeviceObject;
            IoReleaseRemoveLock(&child->RemoveLock, Irp);
            XusbCompleteIrp(Irp, STATUS_SUCCESS, (ULONG_PTR)relations);
            return STATUS_SUCCESS;
        }
        break;

    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES capabilities = stack->Parameters.DeviceCapabilities.Capabilities;
        if (capabilities == NULL || capabilities->Size < sizeof(*capabilities)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        capabilities->Removable = TRUE;
        capabilities->SurpriseRemovalOK = TRUE;
        capabilities->UniqueID = FALSE;
        capabilities->DeviceD1 = FALSE;
        capabilities->DeviceD2 = FALSE;
        capabilities->DeviceWake = PowerDeviceUnspecified;
        capabilities->SystemWake = PowerSystemUnspecified;
        capabilities->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        capabilities->DeviceState[PowerSystemSleeping1] = PowerDeviceD3;
        capabilities->DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
        capabilities->DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
        capabilities->DeviceState[PowerSystemHibernate] = PowerDeviceD3;
        capabilities->DeviceState[PowerSystemShutdown] = PowerDeviceD3;
        capabilities->Address = (ULONG)child->BusAddress;
        capabilities->UINumber = (ULONG)child->BusAddress;
        break;
    }

    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        Irp->IoStatus.Information = 0;
        break;

    case IRP_MN_REMOVE_DEVICE:
    {
        XusbStopTransactions(child);
        XusbCancelTransactions(child);
        IoReleaseRemoveLockAndWait(&child->RemoveLock, Irp);
        PDEVICE_OBJECT parentDevice = XusbRemoveChild(DeviceObject);
        XusbCompleteIrp(Irp, STATUS_SUCCESS, 0);
        IoDeleteDevice(DeviceObject);
        ObDereferenceObject(parentDevice);
        return STATUS_SUCCESS;
    }

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    IoReleaseRemoveLock(&child->RemoveLock, Irp);
    XusbCompleteIrp(Irp, status, Irp->IoStatus.Information);
    return status;
}
#pragma warning(pop)

VOID XusbRemoveAllChildren(_In_ PDEVICE_OBJECT DeviceObject)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PLIST_ENTRY entry;
    LIST_ENTRY unreportedChildren;
    LIST_ENTRY pendingTransactions;
    BOOLEAN changed = FALSE;

    InitializeListHead(&unreportedChildren);
    InitializeListHead(&pendingTransactions);
    InterlockedExchange(&parent->Removing, 1);
    ExAcquireFastMutex(&parent->ChildMutex);
    entry = parent->Children.Flink;
    while (entry != &parent->Children) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(entry, XUSB_CHILD_EXTENSION, Link);
        PLIST_ENTRY next = entry->Flink;
        if (child->Present && !child->Removed) {
            child->Present = FALSE;
            changed = TRUE;
        }
        if (child->Reported && !child->Removed && !child->RemovalPending) {
            child->RemovalPending = TRUE;
            XusbStopTransactions(child);
            ObReferenceObject(child->Self);
            InsertTailList(&pendingTransactions, &child->RemovalLink);
        }
        if (!child->Reported) {
            RemoveEntryList(&child->Link);
            child->Linked = FALSE;
            child->Removed = TRUE;
            child->Signature = 0;
            ObReferenceObject(child->Self);
            InsertTailList(&unreportedChildren, &child->Link);
        }
        entry = next;
    }
    ExReleaseFastMutex(&parent->ChildMutex);

    while (!IsListEmpty(&unreportedChildren)) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(
            RemoveHeadList(&unreportedChildren),
            XUSB_CHILD_EXTENSION,
            Link);
        PDEVICE_OBJECT childPdo = child->Self;
        InitializeListHead(&child->Link);
        XusbDisposeUnreportedChild(childPdo);
        ObDereferenceObject(childPdo);
    }
    while (!IsListEmpty(&pendingTransactions)) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(
            RemoveHeadList(&pendingTransactions),
            XUSB_CHILD_EXTENSION,
            RemovalLink);
        InitializeListHead(&child->RemovalLink);
        XusbCancelTransactions(child);
        ExAcquireFastMutex(&parent->ChildMutex);
        child->RemovalPending = FALSE;
        ExReleaseFastMutex(&parent->ChildMutex);
        ObDereferenceObject(child->Self);
    }
    if (changed) {
        IoInvalidateDeviceRelations(parent->PhysicalDeviceObject, BusRelations);
    }
}

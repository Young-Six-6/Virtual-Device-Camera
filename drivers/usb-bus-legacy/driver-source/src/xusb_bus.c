#include <ntddk.h>
#include <wdmsec.h>
#include <wdmguid.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>
#include <usbbusif.h>
#include <ntstrsafe.h>
#include "xusb_driver.h"

IO_COMPLETION_ROUTINE XusbReleaseControlRemoveLock;

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS XusbHandleRootHubDescriptorRequest(
    _In_ PXUSB_PARENT_EXTENSION Parent,
    _Inout_ PIRP Irp,
    _In_ ULONG InputBytes,
    _In_ ULONG OutputBytes,
    _Out_ PULONG_PTR Information)
{
    PUSB_DESCRIPTOR_REQUEST request = (PUSB_DESCRIPTOR_REQUEST)Irp->AssociatedIrp.SystemBuffer;
    const ULONG requestHeaderBytes = FIELD_OFFSET(USB_DESCRIPTOR_REQUEST, Data);
    ULONG transferLength;
    PDEVICE_OBJECT childPdo = NULL;
    PDEVICE_OBJECT childStack = NULL;
    PLIST_ENTRY entry;
    URB urb;
    IO_STATUS_BLOCK ioStatus;
    KEVENT completionEvent;
    PIRP requestIrp;
    LARGE_INTEGER timeout;
    NTSTATUS status;

    *Information = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || request == NULL ||
        InputBytes < requestHeaderBytes ||
        OutputBytes < requestHeaderBytes || request->ConnectionIndex != 1) {
        return STATUS_INVALID_PARAMETER;
    }
    if (request->SetupPacket.bmRequest != 0x80 || request->SetupPacket.bRequest != 0x06) {
        return STATUS_INVALID_PARAMETER;
    }
    transferLength = request->SetupPacket.wLength;
    if (transferLength > OutputBytes - requestHeaderBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ExAcquireFastMutex(&Parent->ChildMutex);
    for (entry = Parent->Children.Flink;
        entry != &Parent->Children;
        entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(entry, XUSB_CHILD_EXTENSION, Link);
        if (child->Present && !child->Removed && !child->Internal) {
            childPdo = child->Self;
            ObReferenceObject(childPdo);
            break;
        }
    }
    ExReleaseFastMutex(&Parent->ChildMutex);
    if (childPdo == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    childStack = IoGetAttachedDeviceReference(childPdo);
    if (childStack == NULL) {
        ObDereferenceObject(childPdo);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    RtlZeroMemory(&urb, sizeof(urb));
    UsbBuildGetDescriptorRequest(
        &urb,
        sizeof(urb.UrbControlDescriptorRequest),
        (UCHAR)(request->SetupPacket.wValue >> 8),
        (UCHAR)request->SetupPacket.wValue,
        request->SetupPacket.wIndex,
        request->Data,
        NULL,
        transferLength,
        NULL);

    KeInitializeEvent(&completionEvent, NotificationEvent, FALSE);
    requestIrp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        childStack,
        NULL,
        0,
        NULL,
        0,
        TRUE,
        &completionEvent,
        &ioStatus);
    if (requestIrp == NULL) {
        ObDereferenceObject(childStack);
        ObDereferenceObject(childPdo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoGetNextIrpStackLocation(requestIrp)->Parameters.Others.Argument1 = &urb;
    /* Bound the synchronous proxy wait so an idle user-mode bridge cannot hang Hub queries forever. */
    status = IoCallDriver(childStack, requestIrp);
    if (status == STATUS_PENDING) {
        timeout.QuadPart = -5LL * 10 * 1000 * 1000;
        status = KeWaitForSingleObject(
            &completionEvent,
            Executive,
            KernelMode,
            FALSE,
            &timeout);
        if (status == STATUS_TIMEOUT) {
            (VOID)IoCancelIrp(requestIrp);
            KeWaitForSingleObject(&completionEvent, Executive, KernelMode, FALSE, NULL);
            status = NT_SUCCESS(ioStatus.Status) ? ioStatus.Status : STATUS_IO_TIMEOUT;
        } else {
            status = ioStatus.Status;
        }
    }
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb.UrbHeader.Status)) {
        status = STATUS_UNSUCCESSFUL;
    }
    if (NT_SUCCESS(status)) {
        ULONG actualLength = urb.UrbControlDescriptorRequest.TransferBufferLength;
        if (actualLength > transferLength) {
            status = STATUS_DATA_ERROR;
        } else {
            *Information = requestHeaderBytes + actualLength;
        }
    }
    ObDereferenceObject(childStack);
    ObDereferenceObject(childPdo);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS XusbHandleRootHubDriverKeyName(
    _In_ PXUSB_PARENT_EXTENSION Parent,
    _Inout_ PIRP Irp,
    _In_ ULONG InputBytes,
    _In_ ULONG OutputBytes,
    _Out_ PULONG_PTR Information)
{
    PUSB_NODE_CONNECTION_DRIVERKEY_NAME request =
        (PUSB_NODE_CONNECTION_DRIVERKEY_NAME)Irp->AssociatedIrp.SystemBuffer;
    const ULONG headerBytes = FIELD_OFFSET(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName);
    PDEVICE_OBJECT childPdo = NULL;
    PLIST_ENTRY entry;
    ULONG propertyBytes = 0;
    PWSTR propertyBuffer = NULL;
    NTSTATUS status;

    *Information = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || request == NULL ||
        InputBytes < sizeof(ULONG) || OutputBytes < headerBytes) {
        return STATUS_INVALID_PARAMETER;
    }
    if (request->ConnectionIndex != 1) {
        return STATUS_INVALID_PARAMETER;
    }
    ExAcquireFastMutex(&Parent->ChildMutex);
    for (entry = Parent->Children.Flink;
        entry != &Parent->Children;
        entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(entry, XUSB_CHILD_EXTENSION, Link);
        if (child->Present && !child->Removed && !child->Internal) {
            childPdo = child->Self;
            ObReferenceObject(childPdo);
            break;
        }
    }
    ExReleaseFastMutex(&Parent->ChildMutex);
    if (childPdo == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    status = IoGetDeviceProperty(
        childPdo,
        DevicePropertyDriverKeyName,
        0,
        NULL,
        &propertyBytes);
    if (status != STATUS_BUFFER_TOO_SMALL || propertyBytes < sizeof(WCHAR) ||
        (propertyBytes % sizeof(WCHAR)) != 0 || propertyBytes > MAXUSHORT) {
        ObDereferenceObject(childPdo);
        return NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
    }
    {
        propertyBuffer = ExAllocatePool2(POOL_FLAG_PAGED, propertyBytes, XUSB_POOL_TAG);
        if (propertyBuffer == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            status = IoGetDeviceProperty(
                childPdo,
                DevicePropertyDriverKeyName,
                propertyBytes,
                propertyBuffer,
                &propertyBytes);
        }
    }
    ObDereferenceObject(childPdo);
    if (!NT_SUCCESS(status)) {
        if (propertyBuffer != NULL) {
            ExFreePoolWithTag(propertyBuffer, XUSB_POOL_TAG);
        }
        return status;
    }

    request->ActualLength = headerBytes + propertyBytes;
    if (OutputBytes < request->ActualLength) {
        *Information = headerBytes;
        status = STATUS_BUFFER_TOO_SMALL;
    } else {
        RtlCopyMemory(request->DriverKeyName, propertyBuffer, propertyBytes);
        *Information = request->ActualLength;
        status = STATUS_SUCCESS;
    }
    ExFreePoolWithTag(propertyBuffer, XUSB_POOL_TAG);
    return status;
}

static VOID USB_BUSIFFN XusbUsbdInterfaceReference(_In_ PVOID BusContext)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)BusContext;
    ObReferenceObject(child->Self);
}

static VOID USB_BUSIFFN XusbUsbdInterfaceDereference(_In_ PVOID BusContext)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)BusContext;
    ObDereferenceObject(child->Self);
}

static VOID USB_BUSIFFN XusbUsbdGetVersion(
    _In_ PVOID BusContext,
    _Out_opt_ PUSBD_VERSION_INFORMATION VersionInformation,
    _Out_opt_ PULONG HcdCapabilities)
{
    UNREFERENCED_PARAMETER(BusContext);
    if (VersionInformation != NULL) {
        VersionInformation->USBDI_Version = 0x00000500;
        VersionInformation->Supported_USB_Version = 0x0200;
    }
    if (HcdCapabilities != NULL) {
        *HcdCapabilities = 0;
    }
}

static NTSTATUS USB_BUSIFFN XusbUsbdQueryBusTime(
    _In_ PVOID BusContext,
    _Out_opt_ PULONG CurrentUsbFrame)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(CurrentUsbFrame);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS USB_BUSIFFN XusbUsbdSubmitIsoOutUrb(
    _In_ PVOID BusContext,
    _In_ PURB Urb)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(Urb);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS USB_BUSIFFN XusbUsbdQueryBusInformation(
    _In_ PVOID BusContext,
    _In_ ULONG Level,
    _Inout_ PVOID BusInformationBuffer,
    _Inout_ PULONG BusInformationBufferLength,
    _Out_opt_ PULONG BusInformationActualLength)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(BusInformationBuffer);
    UNREFERENCED_PARAMETER(BusInformationBufferLength);
    UNREFERENCED_PARAMETER(BusInformationActualLength);
    return STATUS_UNSUCCESSFUL;
}

static BOOLEAN USB_BUSIFFN XusbUsbdIsDeviceHighSpeed(_In_opt_ PVOID BusContext)
{
    UNREFERENCED_PARAMETER(BusContext);
    return TRUE;
}

static NTSTATUS USB_BUSIFFN XusbUsbdEnumLogEntry(
    _In_ PVOID BusContext,
    _In_ ULONG DriverTag,
    _In_ ULONG EnumTag,
    _In_ ULONG P1,
    _In_ ULONG P2)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(DriverTag);
    UNREFERENCED_PARAMETER(EnumTag);
    UNREFERENCED_PARAMETER(P1);
    UNREFERENCED_PARAMETER(P2);
    return STATUS_SUCCESS;
}

static NTSTATUS USB_BUSIFFN XusbUsbdQueryBusTimeEx(
    _In_opt_ PVOID BusContext,
    _Out_opt_ PULONG HighSpeedFrameCounter)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(HighSpeedFrameCounter);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS USB_BUSIFFN XusbUsbdQueryControllerType(
    _In_opt_ PVOID BusContext,
    _Out_opt_ PULONG HcdiOptionFlags,
    _Out_opt_ PUSHORT PciVendorId,
    _Out_opt_ PUSHORT PciDeviceId,
    _Out_opt_ PUCHAR PciClass,
    _Out_opt_ PUCHAR PciSubClass,
    _Out_opt_ PUCHAR PciRevisionId,
    _Out_opt_ PUCHAR PciProgIf)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(HcdiOptionFlags);
    UNREFERENCED_PARAMETER(PciVendorId);
    UNREFERENCED_PARAMETER(PciDeviceId);
    UNREFERENCED_PARAMETER(PciClass);
    UNREFERENCED_PARAMETER(PciSubClass);
    UNREFERENCED_PARAMETER(PciRevisionId);
    UNREFERENCED_PARAMETER(PciProgIf);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS XusbQueryUsbdInterface(
    _Inout_ PXUSB_CHILD_EXTENSION Child,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PUSB_BUS_INTERFACE_USBDI_V0 busInterface;
    USHORT requiredSize;
    USHORT version = stack->Parameters.QueryInterface.Version;

    if (stack->Parameters.QueryInterface.InterfaceType == NULL ||
        !RtlEqualMemory(
            stack->Parameters.QueryInterface.InterfaceType,
            &USB_BUS_INTERFACE_USBDI_GUID,
            sizeof(GUID))) {
        return STATUS_NOT_SUPPORTED;
    }
    if (version > USB_BUSIF_USBDI_VERSION_3 ||
        stack->Parameters.QueryInterface.Interface == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    requiredSize = version == USB_BUSIF_USBDI_VERSION_0 ? sizeof(USB_BUS_INTERFACE_USBDI_V0) :
        (version == USB_BUSIF_USBDI_VERSION_1 ? sizeof(USB_BUS_INTERFACE_USBDI_V1) :
            (version == USB_BUSIF_USBDI_VERSION_2 ? sizeof(USB_BUS_INTERFACE_USBDI_V2) :
                sizeof(USB_BUS_INTERFACE_USBDI_V3)));
    if (stack->Parameters.QueryInterface.Size < requiredSize ||
        stack->Parameters.QueryInterface.Interface == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    busInterface = (PUSB_BUS_INTERFACE_USBDI_V0)stack->Parameters.QueryInterface.Interface;
    busInterface->Size = requiredSize;
    busInterface->Version = version;
    busInterface->BusContext = Child;
    busInterface->InterfaceReference = XusbUsbdInterfaceReference;
    busInterface->InterfaceDereference = XusbUsbdInterfaceDereference;
    busInterface->GetUSBDIVersion = XusbUsbdGetVersion;
    busInterface->QueryBusTime = XusbUsbdQueryBusTime;
    busInterface->SubmitIsoOutUrb = XusbUsbdSubmitIsoOutUrb;
    busInterface->QueryBusInformation = XusbUsbdQueryBusInformation;
    if (version >= USB_BUSIF_USBDI_VERSION_1) {
        ((PUSB_BUS_INTERFACE_USBDI_V1)busInterface)->IsDeviceHighSpeed =
            XusbUsbdIsDeviceHighSpeed;
    }
    if (version >= USB_BUSIF_USBDI_VERSION_2) {
        ((PUSB_BUS_INTERFACE_USBDI_V2)busInterface)->EnumLogEntry = XusbUsbdEnumLogEntry;
    }
    if (version >= USB_BUSIF_USBDI_VERSION_3) {
        ((PUSB_BUS_INTERFACE_USBDI_V3)busInterface)->QueryBusTimeEx = XusbUsbdQueryBusTimeEx;
        ((PUSB_BUS_INTERFACE_USBDI_V3)busInterface)->QueryControllerType =
            XusbUsbdQueryControllerType;
    }
    busInterface->InterfaceReference(Child);
    return STATUS_SUCCESS;
}

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

static BOOLEAN XusbIsBusAddressInUse(
    _In_ PXUSB_PARENT_EXTENSION Parent,
    _In_ ULONG Candidate)
{
    PLIST_ENTRY entry;

    for (entry = Parent->Children.Flink; entry != &Parent->Children; entry = entry->Flink) {
        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(
            entry,
            XUSB_CHILD_EXTENSION,
            Link);
        if ((ULONG)child->BusAddress == Candidate) {
            return TRUE;
        }
    }
    return FALSE;
}

static LONG XusbAllocateBusAddress(
    _Inout_ PXUSB_PARENT_EXTENSION Parent,
    _In_ LONG RequestedAddress)
{
    ULONG candidate;

    if (RequestedAddress > 0 && RequestedAddress <= XUSB_MAX_USB_ADDRESS &&
        !XusbIsBusAddressInUse(Parent, (ULONG)RequestedAddress)) {
        return RequestedAddress;
    }

    for (candidate = 1; candidate <= XUSB_MAX_USB_ADDRESS; ++candidate) {
        if (!XusbIsBusAddressInUse(Parent, candidate)) {
            return (LONG)candidate;
        }
    }
    return 0;
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

    child->BusAddress = XusbAllocateBusAddress(parent, 0);
    if (child->BusAddress == 0) {
        ExReleaseFastMutex(&parent->ChildMutex);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
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
    child->BusAddress = XusbAllocateBusAddress(parent, Request->BusAddress);
    if (child->BusAddress == 0) {
        ExReleaseFastMutex(&parent->ChildMutex);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
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
    /* The reconstructed legacy Root Hub exposes one virtual downstream port. */
    if (parent->Role == XusbRoleRootHub &&
        (code == IOCTL_USB_GET_NODE_INFORMATION ||
            code == IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION ||
            code == IOCTL_USB_GET_HUB_CAPABILITIES ||
            code == IOCTL_USB_GET_HUB_CAPABILITIES_EX ||
            code == IOCTL_USB_GET_NODE_CONNECTION_NAME ||
            code == IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME ||
            code == IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES ||
            code == IOCTL_USB_GET_HUB_INFORMATION_EX ||
            code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2 ||
            code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION ||
            code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX)) {
        PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
        if (code == IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION) {
            status = XusbHandleRootHubDescriptorRequest(
                parent,
                Irp,
                inputBytes,
                outputBytes,
                &information);
        } else if (code == IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME) {
            status = XusbHandleRootHubDriverKeyName(
                parent,
                Irp,
                inputBytes,
                outputBytes,
                &information);
        } else if (code == IOCTL_USB_GET_NODE_CONNECTION_NAME) {
            if (buffer == NULL || inputBytes < sizeof(ULONG) ||
                outputBytes < FIELD_OFFSET(USB_NODE_CONNECTION_NAME, NodeName) + sizeof(WCHAR)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                PUSB_NODE_CONNECTION_NAME connectionName =
                    (PUSB_NODE_CONNECTION_NAME)buffer;
                if (connectionName->ConnectionIndex != 1) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    connectionName->ActualLength = 0;
                    connectionName->NodeName[0] = UNICODE_NULL;
                    information = FIELD_OFFSET(USB_NODE_CONNECTION_NAME, NodeName) + sizeof(WCHAR);
                    status = STATUS_SUCCESS;
                }
            }
        } else if (buffer == NULL) {
            status = STATUS_INVALID_PARAMETER;
        } else if (code == IOCTL_USB_GET_NODE_INFORMATION) {
            if (inputBytes < sizeof(USB_NODE_INFORMATION) ||
                outputBytes < sizeof(USB_NODE_INFORMATION)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                PUSB_NODE_INFORMATION node = (PUSB_NODE_INFORMATION)buffer;
                if (node->NodeType != UsbHub) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    RtlZeroMemory(node, sizeof(*node));
                    node->NodeType = UsbHub;
                    node->u.HubInformation.HubDescriptor.bDescriptorLength = 9;
                    node->u.HubInformation.HubDescriptor.bDescriptorType =
                        USB_20_HUB_DESCRIPTOR_TYPE;
                    node->u.HubInformation.HubDescriptor.bNumberOfPorts = 1;
                    node->u.HubInformation.HubDescriptor.wHubCharacteristics = 0x0009;
                    node->u.HubInformation.HubDescriptor.bPowerOnToPowerGood = 1;
                    node->u.HubInformation.HubDescriptor.bRemoveAndPowerMask[0] = 0x01;
                    node->u.HubInformation.HubDescriptor.bRemoveAndPowerMask[1] = 0x03;
                    node->u.HubInformation.HubIsBusPowered = FALSE;
                    information = sizeof(*node);
                    status = STATUS_SUCCESS;
                }
            }
        } else if (code == IOCTL_USB_GET_HUB_CAPABILITIES) {
            if (outputBytes < sizeof(USB_HUB_CAPABILITIES)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                USB_HUB_CAPABILITIES capabilities = { 0 };
                capabilities.HubIs2xCapable = 1;
                RtlCopyMemory(buffer, &capabilities, sizeof(capabilities));
                information = sizeof(capabilities);
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_USB_GET_HUB_CAPABILITIES_EX) {
            if (outputBytes < sizeof(USB_HUB_CAPABILITIES_EX)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                USB_HUB_CAPABILITIES_EX capabilities = { 0 };
                capabilities.CapabilityFlags.HubIsHighSpeedCapable = 1;
                capabilities.CapabilityFlags.HubIsHighSpeed = 1;
                capabilities.CapabilityFlags.HubIsRoot = 1;
                RtlCopyMemory(buffer, &capabilities, sizeof(capabilities));
                information = sizeof(capabilities);
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_USB_GET_HUB_INFORMATION_EX) {
            if (outputBytes < sizeof(USB_HUB_INFORMATION_EX)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                PUSB_HUB_INFORMATION_EX informationEx = (PUSB_HUB_INFORMATION_EX)buffer;
                RtlZeroMemory(informationEx, sizeof(*informationEx));
                informationEx->HubType = UsbRootHub;
                informationEx->HighestPortNumber = 1;
                informationEx->u.UsbHubDescriptor.bDescriptorLength = 9;
                informationEx->u.UsbHubDescriptor.bDescriptorType = USB_20_HUB_DESCRIPTOR_TYPE;
                informationEx->u.UsbHubDescriptor.bNumberOfPorts = 1;
                informationEx->u.UsbHubDescriptor.wHubCharacteristics = 0x0009;
                informationEx->u.UsbHubDescriptor.bPowerOnToPowerGood = 1;
                informationEx->u.UsbHubDescriptor.bRemoveAndPowerMask[0] = 0x01;
                informationEx->u.UsbHubDescriptor.bRemoveAndPowerMask[1] = 0x03;
                information = sizeof(*informationEx);
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2) {
            if (inputBytes < sizeof(USB_NODE_CONNECTION_INFORMATION_EX_V2) ||
                outputBytes < sizeof(USB_NODE_CONNECTION_INFORMATION_EX_V2)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                PUSB_NODE_CONNECTION_INFORMATION_EX_V2 connectionV2 =
                    (PUSB_NODE_CONNECTION_INFORMATION_EX_V2)buffer;
                if (connectionV2->ConnectionIndex != 1 ||
                    connectionV2->Length < sizeof(*connectionV2) ||
                    !connectionV2->SupportedUsbProtocols.Usb300) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    /* The request requires Usb300; this port reports USB 1.x/2.0 support. */
                    connectionV2->SupportedUsbProtocols.ul = 0x3;
                    RtlZeroMemory(&connectionV2->Flags, sizeof(connectionV2->Flags));
                    information = sizeof(*connectionV2);
                    status = STATUS_SUCCESS;
                }
            }
        } else if (code == IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES) {
            if (inputBytes < sizeof(USB_NODE_CONNECTION_ATTRIBUTES) ||
                outputBytes < sizeof(USB_NODE_CONNECTION_ATTRIBUTES)) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                PUSB_NODE_CONNECTION_ATTRIBUTES attributes =
                    (PUSB_NODE_CONNECTION_ATTRIBUTES)buffer;
                ULONG index = attributes->ConnectionIndex;
                BOOLEAN connected = FALSE;
                PLIST_ENTRY entry;
                if (index != 1) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    ExAcquireFastMutex(&parent->ChildMutex);
                    for (entry = parent->Children.Flink;
                        entry != &parent->Children;
                        entry = entry->Flink) {
                        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(
                            entry, XUSB_CHILD_EXTENSION, Link);
                        if (child->Present && !child->Removed && !child->Internal) {
                            connected = TRUE;
                            break;
                        }
                    }
                    ExReleaseFastMutex(&parent->ChildMutex);
                    attributes->ConnectionStatus = connected
                        ? DeviceConnected
                        : NoDeviceConnected;
                    attributes->PortAttributes = 0;
                    information = sizeof(*attributes);
                    status = STATUS_SUCCESS;
                }
            }
        } else {
            ULONG index;
            BOOLEAN connected = FALSE;
            USHORT deviceAddress = 0;
            PXUSB_PIPE_ENTRY pipes = NULL;
            ULONG pipeCount = 0;
            USB_DEVICE_DESCRIPTOR descriptor = { 0 };
            UCHAR configurationValue = 0;
            PLIST_ENTRY entry;
            ULONG requiredBytes = code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION
                ? FIELD_OFFSET(USB_NODE_CONNECTION_INFORMATION, PipeList)
                : FIELD_OFFSET(USB_NODE_CONNECTION_INFORMATION_EX, PipeList);

            if (inputBytes < requiredBytes || outputBytes < requiredBytes) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                index = *(PULONG)buffer;
                if (index != 1) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    pipes = ExAllocatePool2(
                        POOL_FLAG_NON_PAGED,
                        XUSB_MAX_PIPES * sizeof(*pipes),
                        'pUXV');
                    if (pipes == NULL) {
                        status = STATUS_INSUFFICIENT_RESOURCES;
                    } else {
                    RtlZeroMemory(pipes, XUSB_MAX_PIPES * sizeof(*pipes));
                    ExAcquireFastMutex(&parent->ChildMutex);
                    for (entry = parent->Children.Flink;
                        entry != &parent->Children;
                        entry = entry->Flink) {
                        PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(
                            entry, XUSB_CHILD_EXTENSION, Link);
                        if (child->Present && !child->Removed && !child->Internal) {
                            ExAcquireFastMutex(&child->ConfigurationMutex);
                            if (child->DeviceDescriptorValid) {
                                descriptor = child->DeviceDescriptor;
                            }
                            configurationValue = child->CurrentConfigurationValue;
                            deviceAddress = (USHORT)child->BusAddress;
                            {
                                KIRQL oldIrql;
                                KeAcquireSpinLock(&child->TransactionLock, &oldIrql);
                                pipeCount = child->PipeCount;
                                if (pipeCount > XUSB_MAX_PIPES) {
                                    pipeCount = XUSB_MAX_PIPES;
                                }
                                RtlCopyMemory(pipes, child->Pipes, pipeCount * sizeof(pipes[0]));
                                KeReleaseSpinLock(&child->TransactionLock, oldIrql);
                            }
                            connected = TRUE;
                            ExReleaseFastMutex(&child->ConfigurationMutex);
                            break;
                        }
                    }
                    ExReleaseFastMutex(&parent->ChildMutex);
                    if (code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION) {
                        PUSB_NODE_CONNECTION_INFORMATION connection =
                            (PUSB_NODE_CONNECTION_INFORMATION)buffer;
                        connection->DeviceDescriptor = descriptor;
                        connection->CurrentConfigurationValue = configurationValue;
                        connection->LowSpeed = FALSE;
                        connection->DeviceIsHub = FALSE;
                        connection->DeviceAddress = deviceAddress;
                        connection->NumberOfOpenPipes = pipeCount;
                        connection->ConnectionStatus = connected
                            ? DeviceConnected
                            : NoDeviceConnected;
                    } else {
                        PUSB_NODE_CONNECTION_INFORMATION_EX connection =
                            (PUSB_NODE_CONNECTION_INFORMATION_EX)buffer;
                        connection->DeviceDescriptor = descriptor;
                        connection->CurrentConfigurationValue = configurationValue;
                        connection->Speed = connected ? UsbHighSpeed : UsbLowSpeed;
                        connection->DeviceIsHub = FALSE;
                        connection->DeviceAddress = deviceAddress;
                        connection->NumberOfOpenPipes = pipeCount;
                        connection->ConnectionStatus = connected
                            ? DeviceConnected
                            : NoDeviceConnected;
                    }
                    if (pipeCount >
                        (MAXULONG - requiredBytes) / sizeof(USB_PIPE_INFO)) {
                        status = STATUS_INTEGER_OVERFLOW;
                    } else {
                        ULONG fullLength = requiredBytes + pipeCount * sizeof(USB_PIPE_INFO);
                        ULONG entriesToCopy = outputBytes < fullLength
                            ? (outputBytes - requiredBytes) / sizeof(USB_PIPE_INFO)
                            : pipeCount;
                        PUSB_PIPE_INFO pipeList = code == IOCTL_USB_GET_NODE_CONNECTION_INFORMATION
                            ? ((PUSB_NODE_CONNECTION_INFORMATION)buffer)->PipeList
                            : ((PUSB_NODE_CONNECTION_INFORMATION_EX)buffer)->PipeList;
                        for (ULONG pipeIndex = 0; pipeIndex < entriesToCopy; ++pipeIndex) {
                            pipeList[pipeIndex].EndpointDescriptor.bLength =
                                sizeof(USB_ENDPOINT_DESCRIPTOR);
                            pipeList[pipeIndex].EndpointDescriptor.bDescriptorType =
                                USB_ENDPOINT_DESCRIPTOR_TYPE;
                            pipeList[pipeIndex].EndpointDescriptor.bEndpointAddress =
                                pipes[pipeIndex].EndpointAddress;
                            pipeList[pipeIndex].EndpointDescriptor.bmAttributes =
                                pipes[pipeIndex].PipeType;
                            pipeList[pipeIndex].EndpointDescriptor.wMaxPacketSize =
                                pipes[pipeIndex].MaximumPacketSize;
                            pipeList[pipeIndex].EndpointDescriptor.bInterval =
                                pipes[pipeIndex].Interval;
                            pipeList[pipeIndex].ScheduleOffset = 0;
                        }
                        information = requiredBytes + entriesToCopy * sizeof(USB_PIPE_INFO);
                        status = entriesToCopy == pipeCount
                            ? STATUS_SUCCESS
                            : STATUS_BUFFER_TOO_SMALL;
                    }
                    ExFreePoolWithTag(pipes, 'pUXV');
                    }
                }
            }
        }
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

#pragma warning(push)
#pragma warning(disable: 6014) // The PnP manager owns the returned device-text allocation.
static NTSTATUS XusbReturnDeviceText(
    _Inout_ PIRP Irp,
    _In_ PCUNICODE_STRING Text,
    _Inout_ PIO_REMOVE_LOCK RemoveLock)
{
    PWSTR copy;
    SIZE_T bytes = Text->Length + sizeof(WCHAR);

    copy = ExAllocatePool2(POOL_FLAG_PAGED, bytes, XUSB_POOL_TAG);
    if (copy == NULL) {
        IoReleaseRemoveLock(RemoveLock, Irp);
        XusbCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(copy, Text->Buffer, Text->Length);
    copy[Text->Length / sizeof(WCHAR)] = UNICODE_NULL;
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
    case IRP_MN_QUERY_INTERFACE:
        status = XusbQueryUsbdInterface(child, Irp);
        break;

    case IRP_MN_START_DEVICE:
        {
        POWER_STATE powerState;
        child->DevicePowerState = PowerDeviceD0;
        powerState.DeviceState = child->DevicePowerState;
        PoSetPowerState(DeviceObject, DevicePowerState, powerState);
        XusbStartPnpTransactions(child);
        break;
        }
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        XusbQuiescePnpTransactions(child);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        XusbCancelPnpQuiesce(child);
        break;

    case IRP_MN_STOP_DEVICE:
        XusbStopPnpTransactions(child);
        XusbReleaseChildConfiguration(child);
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

    case IRP_MN_QUERY_DEVICE_TEXT: {
        WCHAR buffer[64];
        UNICODE_STRING text;
        NTSTATUS textStatus;

        if (stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription) {
            RtlInitUnicodeString(&text, L"Virtual USB device");
        } else if (stack->Parameters.QueryDeviceText.DeviceTextType ==
            DeviceTextLocationInformation) {
            textStatus = RtlStringCchPrintfW(
                buffer,
                RTL_NUMBER_OF(buffer),
                L"Virtual USB bus address %ld",
                child->BusAddress);
            if (!NT_SUCCESS(textStatus)) {
                status = textStatus;
                break;
            }
            RtlInitUnicodeString(&text, buffer);
        } else {
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        return XusbReturnDeviceText(Irp, &text, &child->RemoveLock);
    }

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

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        {
            PXUSB_PARENT_EXTENSION parent =
                (PXUSB_PARENT_EXTENSION)child->ParentDevice->DeviceExtension;
            ExAcquireFastMutex(&parent->ChildMutex);
            if (!child->Present || child->Removed) {
                Irp->IoStatus.Information = PNP_DEVICE_REMOVED;
            }
            ExReleaseFastMutex(&parent->ChildMutex);
        }
        break;

    case IRP_MN_EJECT: {
        /* Dynamic removal is driven by XUSB_IOCTL_REMOVE_CHILD. */
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        if (stack->Parameters.UsageNotification.InPath) {
            /* Virtual cameras cannot back paging, hibernation, or crash-dump I/O. */
            status = STATUS_UNSUCCESSFUL;
        }
        break;

    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES capabilities = stack->Parameters.DeviceCapabilities.Capabilities;
        if (capabilities == NULL || capabilities->Size < sizeof(*capabilities)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        capabilities->Removable = TRUE;
        capabilities->EjectSupported = FALSE;
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

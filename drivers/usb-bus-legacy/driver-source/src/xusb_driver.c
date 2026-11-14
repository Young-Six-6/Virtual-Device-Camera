#include <ntddk.h>
#include <initguid.h>
#include <usbiodef.h>
#include "xusb_driver.h"

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE XusbAddDevice;
DRIVER_UNLOAD XusbUnload;
DRIVER_DISPATCH XusbDispatchPassThrough;
_IRQL_requires_(PASSIVE_LEVEL)
_Dispatch_type_(IRP_MJ_PNP) DRIVER_DISPATCH XusbDispatchPnp;
_Dispatch_type_(IRP_MJ_POWER) DRIVER_DISPATCH XusbDispatchPower;
IO_COMPLETION_ROUTINE XusbReleaseRemoveLock;
IO_COMPLETION_ROUTINE XusbSignalStartCompletion;

/* The USB hub and host-controller GUIDs match the standard IDs embedded in the legacy image. */
static NTSTATUS XusbRegisterRoleInterfaces(
    _Inout_ PXUSB_PARENT_EXTENSION Extension,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;

    switch (Extension->Role) {
    case XusbRoleUsbOnlyEnumerator:
        return IoRegisterDeviceInterface(
            PhysicalDeviceObject,
            &GUID_XUSB_VIRT_INTERFACE,
            NULL,
            &Extension->InterfaceName);

    case XusbRoleRootHub:
        status = IoRegisterDeviceInterface(
            PhysicalDeviceObject,
            &GUID_DEVINTERFACE_USB_HUB,
            NULL,
            &Extension->InterfaceName);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        status = IoRegisterDeviceInterface(
            PhysicalDeviceObject,
            &GUID_XUSB_VIRT_INTERFACE,
            NULL,
            &Extension->ControlInterfaceName);
        return status;

    case XusbRoleHostController:
        return IoRegisterDeviceInterface(
            PhysicalDeviceObject,
            &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
            NULL,
            &Extension->InterfaceName);

    case XusbRoleUsbTreeEnumerator:
    case XusbRoleUnknown:
    default:
        return STATUS_SUCCESS;
    }
}

static NTSTATUS XusbSetRoleInterfaces(
    _Inout_ PXUSB_PARENT_EXTENSION Extension,
    _In_ BOOLEAN Enable)
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS interfaceStatus = STATUS_SUCCESS;

    if (Enable) {
        if (Extension->InterfaceName.Buffer != NULL && !Extension->InterfaceEnabled) {
            status = IoSetDeviceInterfaceState(&Extension->InterfaceName, TRUE);
            if (NT_SUCCESS(status)) {
                Extension->InterfaceEnabled = TRUE;
            }
        }
        if (NT_SUCCESS(status) && Extension->ControlInterfaceName.Buffer != NULL &&
            !Extension->ControlInterfaceEnabled) {
            status = IoSetDeviceInterfaceState(&Extension->ControlInterfaceName, TRUE);
            if (NT_SUCCESS(status)) {
                Extension->ControlInterfaceEnabled = TRUE;
            } else if (Extension->InterfaceEnabled) {
                interfaceStatus = IoSetDeviceInterfaceState(&Extension->InterfaceName, FALSE);
                if (NT_SUCCESS(interfaceStatus)) {
                    Extension->InterfaceEnabled = FALSE;
                }
            }
        }
        return status;
    }

    if (Extension->InterfaceEnabled) {
        status = IoSetDeviceInterfaceState(&Extension->InterfaceName, FALSE);
        if (NT_SUCCESS(status)) {
            Extension->InterfaceEnabled = FALSE;
        }
    }
    if (Extension->ControlInterfaceEnabled) {
        interfaceStatus = IoSetDeviceInterfaceState(&Extension->ControlInterfaceName, FALSE);
        if (NT_SUCCESS(interfaceStatus)) {
            Extension->ControlInterfaceEnabled = FALSE;
        }
        if (NT_SUCCESS(status)) {
            status = interfaceStatus;
        }
    }
    return status;
}

static VOID XusbFreeRoleInterfaces(_Inout_ PXUSB_PARENT_EXTENSION Extension)
{
    if (Extension->ControlInterfaceName.Buffer != NULL) {
        RtlFreeUnicodeString(&Extension->ControlInterfaceName);
    }
    if (Extension->InterfaceName.Buffer != NULL) {
        RtlFreeUnicodeString(&Extension->InterfaceName);
    }
}

static XUSB_DEVICE_ROLE XusbRoleFromHardwareIds(
    _In_reads_bytes_(PropertyBytes) PCWSTR HardwareIds,
    _In_ ULONG PropertyBytes)
{
    static const UNICODE_STRING usbOnlyEnumeratorId = RTL_CONSTANT_STRING(
        L"root\\*xusb_enumbus");
    static const UNICODE_STRING rootHubId = RTL_CONSTANT_STRING(
        L"vFxsUSB\\vRootHUB&Virtual&Fanxiushu");
    static const UNICODE_STRING hostControllerId = RTL_CONSTANT_STRING(
        L"PCI\\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88");
    static const UNICODE_STRING usbTreeId = RTL_CONSTANT_STRING(
        L"root\\*xusb_tree");
    ULONG offset = 0;

    while (offset + sizeof(WCHAR) <= PropertyBytes) {
        PCWSTR current = (PCWSTR)((const UCHAR*)HardwareIds + offset);
        ULONG remainingChars = (PropertyBytes - offset) / sizeof(WCHAR);
        ULONG length = 0;
        UNICODE_STRING candidate;

        while (length < remainingChars && current[length] != UNICODE_NULL) {
            ++length;
        }
        if (length == remainingChars ||
            length > (MAXUSHORT / sizeof(WCHAR))) {
            break;
        }
        if (length == 0) {
            break;
        }

        candidate.Buffer = (PWSTR)current;
        candidate.Length = (USHORT)(length * sizeof(WCHAR));
        candidate.MaximumLength = candidate.Length;
        if (RtlEqualUnicodeString(&candidate, &usbOnlyEnumeratorId, TRUE)) {
            return XusbRoleUsbOnlyEnumerator;
        }
        if (RtlEqualUnicodeString(&candidate, &rootHubId, TRUE)) {
            return XusbRoleRootHub;
        }
        if (RtlEqualUnicodeString(&candidate, &hostControllerId, TRUE)) {
            return XusbRoleHostController;
        }
        if (RtlEqualUnicodeString(&candidate, &usbTreeId, TRUE)) {
            return XusbRoleUsbTreeEnumerator;
        }

        offset += (length + 1) * sizeof(WCHAR);
    }

    return XusbRoleUnknown;
}

NTSTATUS XusbGetDeviceRole(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ XUSB_DEVICE_ROLE* Role)
{
    ULONG propertyBytes = 0;
    PVOID hardwareIds;
    NTSTATUS status;

    status = IoGetDeviceProperty(
        PhysicalDeviceObject,
        DevicePropertyHardwareID,
        0,
        NULL,
        &propertyBytes);
    if (status != STATUS_BUFFER_TOO_SMALL || propertyBytes < sizeof(WCHAR) * 2 ||
        (propertyBytes % sizeof(WCHAR)) != 0) {
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    }

    hardwareIds = ExAllocatePool2(POOL_FLAG_PAGED, propertyBytes, 'iUXV');
    if (hardwareIds == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoGetDeviceProperty(
        PhysicalDeviceObject,
        DevicePropertyHardwareID,
        propertyBytes,
        hardwareIds,
        &propertyBytes);
    if (NT_SUCCESS(status)) {
        *Role = XusbRoleFromHardwareIds(
            (PCWSTR)hardwareIds,
            propertyBytes);
        if (*Role == XusbRoleUnknown) {
            status = STATUS_NO_SUCH_DEVICE;
        }
    }
    ExFreePoolWithTag(hardwareIds, 'iUXV');
    return status;
}

NTSTATUS XusbReleaseRemoveLock(
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

NTSTATUS XusbSignalStartCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    NT_ASSERT(Context != NULL);
    if (Context != NULL) {
        KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS XusbDispatchPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    ULONG signature = *(ULONG*)DeviceObject->DeviceExtension;
    PXUSB_PARENT_EXTENSION extension = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    NTSTATUS status;
    if (signature == XUSB_CHILD_SIGNATURE &&
        IoGetCurrentIrpStackLocation(Irp)->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) {
        return XusbQueueUsbRequest(DeviceObject, Irp);
    }
    if (signature != XUSB_PARENT_SIGNATURE) {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }
    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);

    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    if (InterlockedCompareExchange(&extension->Removing, 0, 0) != 0) {
        IoReleaseRemoveLock(&extension->RemoveLock, Irp);
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DELETE_PENDING;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        XusbReleaseRemoveLock,
        &extension->RemoveLock,
        TRUE,
        TRUE,
        TRUE);
    status = IoCallDriver(extension->LowerDevice, Irp);
    return status;
}

NTSTATUS XusbDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    ULONG signature = *(ULONG*)DeviceObject->DeviceExtension;
    PXUSB_PARENT_EXTENSION extension = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    if (signature == XUSB_CHILD_SIGNATURE) {
        return XusbDispatchChildPnp(DeviceObject, Irp);
    }
    if (signature != XUSB_PARENT_SIGNATURE) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (stack->MinorFunction == IRP_MN_QUERY_DEVICE_RELATIONS &&
        stack->Parameters.QueryDeviceRelations.Type == BusRelations &&
        (extension->Role == XusbRoleUsbOnlyEnumerator ||
            extension->Role == XusbRoleRootHub ||
            extension->Role == XusbRoleUsbTreeEnumerator ||
            extension->Role == XusbRoleHostController)) {
        status = XusbEnumerateChildren(DeviceObject, Irp);
        return status;
    }

    if (stack->MinorFunction == IRP_MN_START_DEVICE) {
        KEVENT completionEvent;
        NTSTATUS lowerStatus;

        KeInitializeEvent(&completionEvent, NotificationEvent, FALSE);
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(
            Irp,
            XusbSignalStartCompletion,
            &completionEvent,
            TRUE,
            TRUE,
            TRUE);
        lowerStatus = IoCallDriver(extension->LowerDevice, Irp);
        if (lowerStatus == STATUS_PENDING) {
            KeWaitForSingleObject(
                &completionEvent,
                Executive,
                KernelMode,
                FALSE,
                NULL);
        }
        status = Irp->IoStatus.Status;
        if (NT_SUCCESS(status)) {
            status = XusbWmiStart(extension, DeviceObject);
        }
        if (NT_SUCCESS(status)) {
            status = XusbSetRoleInterfaces(extension, TRUE);
        }
        if (NT_SUCCESS(status)) {
            status = XusbCreateRoleChild(DeviceObject);
        }
        if (!NT_SUCCESS(status)) {
            (VOID)XusbSetRoleInterfaces(extension, FALSE);
            XusbWmiStop(extension, DeviceObject);
        }
        IoReleaseRemoveLock(&extension->RemoveLock, Irp);
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (stack->MinorFunction == IRP_MN_STOP_DEVICE ||
        stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
        status = XusbSetRoleInterfaces(extension, FALSE);
        if (!NT_SUCCESS(status) && stack->MinorFunction == IRP_MN_STOP_DEVICE) {
            IoReleaseRemoveLock(&extension->RemoveLock, Irp);
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }
        if (stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
            XusbRemoveAllChildren(DeviceObject);
            XusbWmiStop(extension, DeviceObject);
        }
    }

    if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        InterlockedExchange(&extension->Removing, 1);
        (VOID)XusbSetRoleInterfaces(extension, FALSE);
        XusbRemoveAllChildren(DeviceObject);
        XusbWmiStop(extension, DeviceObject);
        IoReleaseRemoveLockAndWait(&extension->RemoveLock, Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(extension->LowerDevice, Irp);
        IoDetachDevice(extension->LowerDevice);
        XusbFreeRoleInterfaces(extension);
        ObDereferenceObject(extension->PhysicalDeviceObject);
        IoDeleteDevice(DeviceObject);
        return status;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        XusbReleaseRemoveLock,
        &extension->RemoveLock,
        TRUE,
        TRUE,
        TRUE);
    status = IoCallDriver(extension->LowerDevice, Irp);
    return status;
}

NTSTATUS XusbDispatchSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    ULONG signature = *(ULONG*)DeviceObject->DeviceExtension;
    PXUSB_PARENT_EXTENSION extension = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    SYSCTL_IRP_DISPOSITION disposition = IrpNotWmi;
    NTSTATUS status;

    if (signature != XUSB_PARENT_SIGNATURE) {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (InterlockedCompareExchange(&extension->Removing, 0, 0) != 0) {
        IoReleaseRemoveLock(&extension->RemoveLock, Irp);
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DELETE_PENDING;
    }

    status = WmiSystemControl(&extension->WmiLibInfo, DeviceObject, Irp, &disposition);
    switch (disposition) {
    case IrpProcessed:
        IoReleaseRemoveLock(&extension->RemoveLock, Irp);
        return status;

    case IrpNotCompleted:
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoReleaseRemoveLock(&extension->RemoveLock, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;

    case IrpNotWmi:
    case IrpForward:
    default:
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(
            Irp,
            XusbReleaseRemoveLock,
            &extension->RemoveLock,
            TRUE,
            TRUE,
            TRUE);
        return IoCallDriver(extension->LowerDevice, Irp);
    }
}

NTSTATUS XusbDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    ULONG signature = *(ULONG*)DeviceObject->DeviceExtension;
    PXUSB_PARENT_EXTENSION extension = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    NTSTATUS status;
    if (signature == XUSB_CHILD_SIGNATURE) {
        PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
        NTSTATUS childStatus = STATUS_SUCCESS;
        PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;

        PoStartNextPowerIrp(Irp);
        childStatus = IoAcquireRemoveLock(&child->RemoveLock, Irp);
        if (!NT_SUCCESS(childStatus)) {
            Irp->IoStatus.Status = childStatus;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return childStatus;
        }
        if (child->Signature != XUSB_CHILD_SIGNATURE) {
            childStatus = STATUS_DELETE_PENDING;
        } else
        if (stack->MinorFunction == IRP_MN_SET_POWER &&
            stack->Parameters.Power.Type == DevicePowerState) {
            DEVICE_POWER_STATE newState = stack->Parameters.Power.State.DeviceState;
            if (newState < PowerDeviceD0 || newState > PowerDeviceD3) {
                childStatus = STATUS_INVALID_PARAMETER;
            } else {
                POWER_STATE powerState;
                child->DevicePowerState = newState;
                powerState.DeviceState = newState;
                PoSetPowerState(DeviceObject, DevicePowerState, powerState);
            }
        } else if (stack->MinorFunction != IRP_MN_QUERY_POWER &&
            stack->MinorFunction != IRP_MN_SET_POWER) {
            childStatus = STATUS_NOT_SUPPORTED;
        }
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = childStatus;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return childStatus;
    }
    if (signature != XUSB_PARENT_SIGNATURE) {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);

    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        XusbReleaseRemoveLock,
        &extension->RemoveLock,
        TRUE,
        TRUE,
        TRUE);
    PoStartNextPowerIrp(Irp);
    return PoCallDriver(extension->LowerDevice, Irp);
}

#pragma warning(push)
#pragma warning(disable: 6014) // InterfaceName is released in IRP_MN_REMOVE_DEVICE.
NTSTATUS XusbAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT deviceObject = NULL;
    XUSB_DEVICE_ROLE role;
    PXUSB_PARENT_EXTENSION extension;
    NTSTATUS status;

    status = XusbGetDeviceRole(PhysicalDeviceObject, &role);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoCreateDevice(
        DriverObject,
        sizeof(XUSB_PARENT_EXTENSION),
        NULL,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    extension = (PXUSB_PARENT_EXTENSION)deviceObject->DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));
    extension->Signature = XUSB_PARENT_SIGNATURE;
    extension->PhysicalDeviceObject = PhysicalDeviceObject;
    extension->NextBusAddress = 1;
    extension->Role = role;
    XusbWmiInitialize(extension);
    ExInitializeFastMutex(&extension->ChildMutex);
    InitializeListHead(&extension->Children);
    IoInitializeRemoveLock(&extension->RemoveLock, 'sUXV', 0, 0);

    extension->LowerDevice = IoAttachDeviceToDeviceStack(
        deviceObject,
        PhysicalDeviceObject);
    if (extension->LowerDevice == NULL) {
        IoDeleteDevice(deviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    status = XusbRegisterRoleInterfaces(extension, PhysicalDeviceObject);
    if (!NT_SUCCESS(status)) {
        XusbFreeRoleInterfaces(extension);
        IoDetachDevice(extension->LowerDevice);
        IoDeleteDevice(deviceObject);
        return status;
    }
    ObReferenceObject(PhysicalDeviceObject);

    deviceObject->Flags |= extension->LowerDevice->Flags &
        (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
#pragma warning(pop)

VOID XusbUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    XusbWmiReleaseServiceRegistryPath();
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    ULONG index;
    NTSTATUS status;

    status = XusbWmiSetServiceRegistryPath(RegistryPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

#pragma warning(push)
#pragma warning(disable: 28168) // The common dispatcher intentionally serves multiple major codes.
    for (index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index) {
        DriverObject->MajorFunction[index] = XusbDispatchPassThrough;
    }
#pragma warning(pop)
    DriverObject->MajorFunction[IRP_MJ_CREATE] = XusbCreateFile;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = XusbCleanupFile;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = XusbCloseFile;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = XusbDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = XusbDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = XusbDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = XusbDispatchSystemControl;
    DriverObject->DriverExtension->AddDevice = XusbAddDevice;
    DriverObject->DriverUnload = XusbUnload;
    return STATUS_SUCCESS;
}

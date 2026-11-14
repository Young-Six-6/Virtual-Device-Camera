#include <ntddk.h>
#include <wmilib.h>
#include <wmistr.h>
#include "xusb_driver.h"

#define XUSB_WMI_TAG 'mUXV'
#define XUSB_WMI_DATA_LENGTH (sizeof(ULONG) * 2)
#define XUSB_WMI_DATA_ITEM_VALUE 2

static const GUID XusbWmiGuid = {
    0xfd2700e7,
    0x1773,
    0x43ab,
    { 0x82, 0xb5, 0xe6, 0xf1, 0x1c, 0x14, 0x97, 0xc8 }
};

static WMIGUIDREGINFO XusbWmiGuidList[] = {
    { &XusbWmiGuid, 1, 0 }
};
static UNICODE_STRING XusbWmiRegistryPath;
static volatile LONG XusbWmiGlobalValue;

_Function_class_(WMI_QUERY_REGINFO_CALLBACK)
static NTSTATUS XusbWmiQueryRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Outptr_result_maybenull_ PUNICODE_STRING* RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Outptr_result_maybenull_ PDEVICE_OBJECT* Pdo)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    UNREFERENCED_PARAMETER(InstanceName);

    if (parent->Signature != XUSB_PARENT_SIGNATURE || RegistryPath == NULL || Pdo == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    if (XusbWmiRegistryPath.Buffer == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    *RegistryPath = &XusbWmiRegistryPath;
    RtlInitUnicodeString(MofResourceName, L"BusEnumWMI");
    *Pdo = parent->PhysicalDeviceObject;
    return STATUS_SUCCESS;
}

_Function_class_(WMI_QUERY_DATABLOCK_CALLBACK)
static NTSTATUS XusbWmiQueryDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_writes_opt_(InstanceCount) PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_writes_bytes_opt_(BufferAvail) PUCHAR Buffer)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    ULONG values[2];

    if (GuidIndex != 0) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_GUID_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    if (InstanceIndex != 0 || InstanceCount != 1) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_INSTANCE_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    if (InstanceLengthArray != NULL) {
        InstanceLengthArray[0] = XUSB_WMI_DATA_LENGTH;
    }
    if (InstanceLengthArray == NULL || Buffer == NULL || BufferAvail < XUSB_WMI_DATA_LENGTH) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_BUFFER_TOO_SMALL, XUSB_WMI_DATA_LENGTH, IO_NO_INCREMENT);
    }

    values[0] = (ULONG)InterlockedCompareExchange(&parent->WmiValue0, 0, 0);
    values[1] = (ULONG)InterlockedCompareExchange(&parent->WmiValue1, 0, 0);
    RtlCopyMemory(Buffer, values, XUSB_WMI_DATA_LENGTH);
    return WmiCompleteRequest(
        DeviceObject, Irp, STATUS_SUCCESS, XUSB_WMI_DATA_LENGTH, IO_NO_INCREMENT);
}

_Function_class_(WMI_SET_DATABLOCK_CALLBACK)
static NTSTATUS XusbWmiSetDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG BufferSize,
    _In_reads_bytes_(BufferSize) PUCHAR Buffer)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    ULONG value;

    if (GuidIndex != 0) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_GUID_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    if (InstanceIndex != 0) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_INSTANCE_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    if (Buffer == NULL || BufferSize < XUSB_WMI_DATA_LENGTH) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_BUFFER_TOO_SMALL, XUSB_WMI_DATA_LENGTH, IO_NO_INCREMENT);
    }

    RtlCopyMemory(&value, Buffer + sizeof(ULONG), sizeof(value));
    InterlockedExchange(&parent->WmiValue1, (LONG)value);
    InterlockedExchange(&XusbWmiGlobalValue, (LONG)value);
    return WmiCompleteRequest(
        DeviceObject, Irp, STATUS_SUCCESS, XUSB_WMI_DATA_LENGTH, IO_NO_INCREMENT);
}

_Function_class_(WMI_SET_DATAITEM_CALLBACK)
static NTSTATUS XusbWmiSetDataItem(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG DataItemId,
    _In_ ULONG BufferSize,
    _In_reads_bytes_(BufferSize) PUCHAR Buffer)
{
    PXUSB_PARENT_EXTENSION parent = (PXUSB_PARENT_EXTENSION)DeviceObject->DeviceExtension;
    ULONG value;

    if (GuidIndex != 0) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_GUID_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    if (InstanceIndex != 0) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_INSTANCE_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    if (DataItemId != XUSB_WMI_DATA_ITEM_VALUE) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_WMI_READ_ONLY, 0, IO_NO_INCREMENT);
    }
    if (Buffer == NULL || BufferSize < sizeof(value)) {
        return WmiCompleteRequest(
            DeviceObject, Irp, STATUS_BUFFER_TOO_SMALL, sizeof(value), IO_NO_INCREMENT);
    }

    RtlCopyMemory(&value, Buffer, sizeof(value));
    InterlockedExchange(&parent->WmiValue1, (LONG)value);
    return WmiCompleteRequest(
        DeviceObject, Irp, STATUS_SUCCESS, sizeof(value), IO_NO_INCREMENT);
}

VOID XusbWmiInitialize(_Inout_ PXUSB_PARENT_EXTENSION Parent)
{
    RtlZeroMemory(&Parent->WmiLibInfo, sizeof(Parent->WmiLibInfo));
    Parent->WmiLibInfo.GuidCount = RTL_NUMBER_OF(XusbWmiGuidList);
    Parent->WmiLibInfo.GuidList = XusbWmiGuidList;
    Parent->WmiLibInfo.QueryWmiRegInfo = XusbWmiQueryRegInfo;
    Parent->WmiLibInfo.QueryWmiDataBlock = XusbWmiQueryDataBlock;
    Parent->WmiLibInfo.SetWmiDataBlock = XusbWmiSetDataBlock;
    Parent->WmiLibInfo.SetWmiDataItem = XusbWmiSetDataItem;
    Parent->WmiValue0 = 0;
    Parent->WmiValue1 = 0;
    Parent->WmiRegistered = FALSE;
}

NTSTATUS XusbWmiSetServiceRegistryPath(_In_ PCUNICODE_STRING RegistryPath)
{
    USHORT allocationLength;
    PWCHAR buffer;

    if (RegistryPath == NULL || RegistryPath->Buffer == NULL ||
        RegistryPath->Length == 0 ||
        RegistryPath->Length > MAXUSHORT - sizeof(WCHAR)) {
        return STATUS_INVALID_PARAMETER;
    }
    allocationLength = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    buffer = ExAllocatePool2(POOL_FLAG_PAGED, allocationLength, XUSB_WMI_TAG);
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(buffer, RegistryPath->Buffer, RegistryPath->Length);
    {
        const WCHAR terminator = UNICODE_NULL;
        RtlCopyMemory((PUCHAR)buffer + RegistryPath->Length, &terminator, sizeof(terminator));
    }
    XusbWmiRegistryPath.Buffer = buffer;
    XusbWmiRegistryPath.Length = RegistryPath->Length;
    XusbWmiRegistryPath.MaximumLength = allocationLength;
    return STATUS_SUCCESS;
}

VOID XusbWmiReleaseServiceRegistryPath(VOID)
{
    if (XusbWmiRegistryPath.Buffer != NULL) {
        ExFreePoolWithTag(XusbWmiRegistryPath.Buffer, XUSB_WMI_TAG);
        RtlZeroMemory(&XusbWmiRegistryPath, sizeof(XusbWmiRegistryPath));
    }
}

NTSTATUS XusbWmiStart(
    _Inout_ PXUSB_PARENT_EXTENSION Parent,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS status;

    if (Parent->WmiRegistered) {
        return STATUS_SUCCESS;
    }
    status = IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_REGISTER);
    if (NT_SUCCESS(status)) {
        Parent->WmiRegistered = TRUE;
    }
    return status;
}

VOID XusbWmiStop(
    _Inout_ PXUSB_PARENT_EXTENSION Parent,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    if (Parent->WmiRegistered) {
        (VOID)IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);
        Parent->WmiRegistered = FALSE;
    }
}

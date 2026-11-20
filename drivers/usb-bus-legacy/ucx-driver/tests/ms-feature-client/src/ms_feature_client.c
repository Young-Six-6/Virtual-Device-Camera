#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <initguid.h>

/*
 * Test-only function driver for the synthetic VID_FFFF&PID_7E41 device.
 * The IOCTL deliberately submits URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR via
 * the KMDF USB target. It must never be linked into the production HCD.
 */

#define MS_FEATURE_TEST_IOCTL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define MS_FEATURE_TEST_TIMEOUT_MS 2000u
#define MS_FEATURE_DESCRIPTOR_4_BYTES 40u
#define MS_FEATURE_DESCRIPTOR_5_BYTES 10u

DEFINE_GUID(GUID_DEVINTERFACE_UCX_MS_FEATURE_TEST,
    0x7d85244a, 0xf4db, 0x4b4c, 0x98, 0x26, 0xa0, 0x8d, 0x73, 0x32, 0x4c, 0x91);

typedef struct _DEVICE_CONTEXT {
    WDFUSBDEVICE UsbDevice;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtDeviceIoControl;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtDeviceReleaseHardware;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
DRIVER_INITIALIZE DriverEntry;

typedef struct _MS_FEATURE_TEST_RESULT {
    ULONG Size;
    ULONG Version;
    NTSTATUS Descriptor4Status;
    USBD_STATUS Descriptor4UsbStatus;
    ULONG Descriptor4Length;
    UCHAR Descriptor4[MS_FEATURE_DESCRIPTOR_4_BYTES];
    NTSTATUS Descriptor5Status;
    USBD_STATUS Descriptor5UsbStatus;
    ULONG Descriptor5Length;
    UCHAR Descriptor5[MS_FEATURE_DESCRIPTOR_5_BYTES];
} MS_FEATURE_TEST_RESULT, *PMS_FEATURE_TEST_RESULT;

static NTSTATUS SendFeatureDescriptor(
    _In_ WDFUSBDEVICE UsbDevice,
    _In_ USHORT DescriptorIndex,
    _In_ UCHAR Recipient,
    _Out_writes_bytes_(BufferLength) UCHAR* Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG Transferred,
    _Out_ USBD_STATUS* UsbStatus)
{
    WDFMEMORY urbMemory = WDF_NO_HANDLE;
    PURB urb = NULL;
    size_t urbBufferLength = 0;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;

    /*
     * This fixture registers USBD_CLIENT_CONTRACT_VERSION_602. URBs sent under
     * that contract must be allocated by the USB stack, not placed on the
     * driver stack or allocated with a generic pool allocator.
     */
    status = WdfUsbTargetDeviceCreateUrb(
        UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &urbMemory, NULL);
    if (!NT_SUCCESS(status)) return status;

    urb = (PURB)WdfMemoryGetBuffer(urbMemory, &urbBufferLength);
    if (urb == NULL || urbBufferLength < sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST)) {
        WdfObjectDelete(urbMemory);
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(urb, sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST));
    urb->UrbHeader.Function = URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR;
    urb->UrbHeader.Length = (USHORT)sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST);
    urb->UrbOSFeatureDescriptorRequest.TransferBufferLength = BufferLength;
    urb->UrbOSFeatureDescriptorRequest.TransferBuffer = Buffer;
    urb->UrbOSFeatureDescriptorRequest.InterfaceNumber = 0;
    urb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex = DescriptorIndex;
    urb->UrbOSFeatureDescriptorRequest.Recipient = Recipient;
    urb->UrbOSFeatureDescriptorRequest.MS_PageIndex = 0;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(MS_FEATURE_TEST_TIMEOUT_MS));
    status = WdfUsbTargetDeviceSendUrbSynchronously(UsbDevice, WDF_NO_HANDLE, &options, urb);
    *UsbStatus = urb->UrbHeader.Status;
    *Transferred = urb->UrbOSFeatureDescriptorRequest.TransferBufferLength;
    WdfObjectDelete(urbMemory);
    return status;
}

static VOID EvtDeviceIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = GetDeviceContext(device);
    PMS_FEATURE_TEST_RESULT result;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(InputBufferLength);
    if (IoControlCode != MS_FEATURE_TEST_IOCTL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    if (OutputBufferLength < sizeof(*result)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*result), (PVOID*)&result, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    if (context->UsbDevice == WDF_NO_HANDLE) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    RtlZeroMemory(result, sizeof(*result));
    result->Size = sizeof(*result);
    result->Version = 1;
    result->Descriptor4Status = SendFeatureDescriptor(
        context->UsbDevice, 4, 0, result->Descriptor4, sizeof(result->Descriptor4),
        &result->Descriptor4Length, &result->Descriptor4UsbStatus);
    result->Descriptor5Status = SendFeatureDescriptor(
        context->UsbDevice, 5, 1, result->Descriptor5, sizeof(result->Descriptor5),
        &result->Descriptor5Length, &result->Descriptor5UsbStatus);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*result));
}

static NTSTATUS EvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    WDF_USB_DEVICE_CREATE_CONFIG createConfig;
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    if (context->UsbDevice != WDF_NO_HANDLE) return STATUS_INVALID_DEVICE_STATE;

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createConfig, USBD_CLIENT_CONTRACT_VERSION_602);
    return WdfUsbTargetDeviceCreateWithParameters(
        Device, &createConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->UsbDevice);
}

static NTSTATUS EvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (context->UsbDevice != WDF_NO_HANDLE) {
        WdfObjectDelete(context->UsbDevice);
        context->UsbDevice = WDF_NO_HANDLE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    UNICODE_STRING sddl;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(
        DeviceInit,
        FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME,
        FALSE);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) return status;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) return status;
    GetDeviceContext(device)->UsbDevice = WDF_NO_HANDLE;

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_UCX_MS_FEATURE_TEST, NULL);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtDeviceIoControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfIoQueueCreate(device, &queueConfig, &queueAttributes, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
}

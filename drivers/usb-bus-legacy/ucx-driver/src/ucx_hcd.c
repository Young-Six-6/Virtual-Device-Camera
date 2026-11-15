#include <initguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbspec.h>
#include <usbdlib.h>
#include <ucxclass.h>
#include <ucxcontroller.h>
#include <ucxroothub.h>
#include <ucxusbdevice.h>
#include <ucxendpoint.h>
#include "../../driver-source/include/xusb_abi.h"

static WDFDEVICE gDevice;
static WDFQUEUE gIoQueue;
static UCXCONTROLLER gController;
static UCXROOTHUB gRootHub;
#define VHCD_MAX_PORTS 16
#define VHCD_MAX_TRANSACTION_BYTES (32u * 1024u * 1024u)
#define VHCD_MAX_SLOT_TRANSACTION_BYTES (64u * 1024u * 1024u)
#define VHCD_MAX_SLOT_TRANSACTIONS 256u
#define VHCD_MAX_CONTROLLER_TRANSACTION_BYTES (256u * 1024u * 1024u)
#define VHCD_MAX_CONTROLLER_TRANSACTIONS 1024u
#define VHCD_FILE_SIGNATURE 'FUXV'
#define VHCD_TX_SIGNATURE 'TUXV'

typedef struct _VHCD_SLOT {
    BOOLEAN Allocated;
    BOOLEAN Present;
    BOOLEAN Enabled;
    BOOLEAN ConnectionChanged;
    BOOLEAN ResetChanged;
    BOOLEAN RemoveRequested;
    LONG UsbDeviceRefs;
    LONG TransferRefs;
    ULONGLONG TransactionBytes;
    ULONG TransactionCount;
    ULONGLONG NextTransactionId;
    LONG NextSequence;
    LIST_ENTRY Transactions;
    LONG BusAddress;
    PKSEMAPHORE Semaphore;
    PKEVENT RemoveEvent;
} VHCD_SLOT, *PVHCD_SLOT;

typedef struct _VHCD_FILE_CONTEXT {
    ULONG Signature;
    LONG SlotIndex;
} VHCD_FILE_CONTEXT, *PVHCD_FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_FILE_CONTEXT, VhcdGetFileContext)

typedef struct _VHCD_USBDEVICE_CONTEXT {
    ULONG SlotIndex;
} VHCD_USBDEVICE_CONTEXT, *PVHCD_USBDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_USBDEVICE_CONTEXT, VhcdGetUsbDeviceContext)

typedef struct _VHCD_QUEUE_CONTEXT {
    ULONG SlotIndex;
    UCHAR EndpointAddress;
    UCHAR PipeType;
} VHCD_QUEUE_CONTEXT, *PVHCD_QUEUE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_QUEUE_CONTEXT, VhcdGetQueueContext)

typedef struct _VHCD_ENDPOINT_CONTEXT {
    ULONG SlotIndex;
    UCHAR EndpointAddress;
    UCHAR PipeType;
} VHCD_ENDPOINT_CONTEXT, *PVHCD_ENDPOINT_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_ENDPOINT_CONTEXT, VhcdGetEndpointContext)

typedef struct _VHCD_USER_IO_CONTEXT {
    WDFMEMORY InputMemory;
    WDFMEMORY OutputMemory;
    PKSEMAPHORE AddSemaphore;
    PKEVENT AddRemoveEvent;
} VHCD_USER_IO_CONTEXT, *PVHCD_USER_IO_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_USER_IO_CONTEXT, VhcdGetUserIoContext)

typedef struct _VHCD_TRANSFER_CONTEXT {
    PVOID Transaction;
} VHCD_TRANSFER_CONTEXT, *PVHCD_TRANSFER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_TRANSFER_CONTEXT, VhcdGetTransferContext)

typedef struct _VHCD_TRANSACTION {
    LIST_ENTRY Link;
    ULONG Signature;
    LONG SlotIndex;
    ULONGLONG Id;
    LONG Sequence;
    BOOLEAN Begun;
    BOOLEAN NeedsRetry;
    BOOLEAN Linked;
    BOOLEAN Isochronous;
    BOOLEAN IsRead;
    BOOLEAN Ending;
    BOOLEAN CancelableReady;
    BOOLEAN CancelRequested;
    UCHAR EndpointAddress;
    volatile LONG References;
    volatile LONG OwnerFinished;
    volatile LONG CancelCompletionQueued;
    WDFREQUEST Request;
    PURB Urb;
    PUCHAR TransferBuffer;
    ULONG TransferLength;
    ULONG MetadataLength;
    ULONG PayloadCapacity;
    ULONG AllocationBytes;
    KDPC CancelDpc;
    UCHAR Buffer[1];
} VHCD_TRANSACTION, *PVHCD_TRANSACTION;

typedef struct _VHCD_WIRE_REQUEST {
    LONG Type;
    LONG Result;
    LONG DataLength;
    LONG Reserved;
    union {
        struct { LONG Type, Subtype, IsRead, Index, Value; } Descriptor;
        struct { LONG Type, Subtype, IsRead, Request, Index, Value; } Vendor;
        struct { LONG Type, EpAddress, IsRead; union { struct { LONG NumberPackets, Reserved0; CHAR IsSplit, Reserved[3]; }; struct { UCHAR SetupPacket[8]; ULONG Timeout; }; }; } Transfer;
        struct { LONG Type, EpAddress; } Reset;
        struct { LONG Type, Index; } Status;
        struct { LONG Type, Subtype, Index, Value; } Feature;
    } Data;
} VHCD_WIRE_REQUEST, *PVHCD_WIRE_REQUEST;

C_ASSERT(sizeof(VHCD_WIRE_REQUEST) == 40);

static VHCD_SLOT gSlots[VHCD_MAX_PORTS];
static BOOLEAN gAddressInUse[128];
static WDFSPINLOCK gSlotLock;
static ULONGLONG gTransactionBytes;
static ULONG gTransactionCount;

typedef struct _VHCD_CANCEL_DPC {
    KDPC Dpc;
    WDFREQUEST Request;
    PURB Urb;
} VHCD_CANCEL_DPC, *PVHCD_CANCEL_DPC;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD VhcdEvtDeviceAdd;
EVT_WDF_IO_IN_CALLER_CONTEXT VhcdEvtIoInCallerContext;
EVT_WDF_REQUEST_CANCEL VhcdEvtTransferCanceled;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VhcdEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL VhcdEvtEndpointIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE VhcdEvtIoCanceledOnQueue;
EVT_WDF_DEVICE_FILE_CREATE VhcdEvtFileCreate;
EVT_WDF_FILE_CLEANUP VhcdEvtFileCleanup;
EVT_WDF_FILE_CLOSE VhcdEvtFileClose;
EVT_WDF_OBJECT_CONTEXT_CLEANUP VhcdUserIoContextCleanup;
EVT_UCX_CONTROLLER_QUERY_USB_CAPABILITY VhcdEvtControllerQueryUsbCapability;
EVT_UCX_CONTROLLER_GET_CURRENT_FRAMENUMBER VhcdEvtControllerGetCurrentFrameNumber;
EVT_UCX_CONTROLLER_RESET VhcdEvtControllerReset;
static VOID VhcdFinalizeSlotIfQuiescent(_In_ LONG SlotIndex);
static VOID VhcdUsbDeviceCleanup(_In_ WDFOBJECT Object);
static VOID VhcdCancelSlotTransactions(_In_ LONG SlotIndex, _In_ UCHAR EndpointAddress, _In_ BOOLEAN AllEndpoints);
static VOID VhcdCancelEndpointTransfers(_In_ UCXENDPOINT Endpoint);
static NTSTATUS VhcdQueuePortResetNotification(_In_ LONG SlotIndex, _In_ LONG ResetType);
EVT_UCX_CONTROLLER_USBDEVICE_ADD VhcdEvtControllerUsbDeviceAdd;
EVT_UCX_ROOTHUB_CONTROL_URB VhcdEvtRootHubControlUrb;
EVT_UCX_ROOTHUB_INTERRUPT_TX VhcdEvtRootHubInterruptTx;
EVT_UCX_ROOTHUB_GET_INFO VhcdEvtRootHubGetInfo;
EVT_UCX_ROOTHUB_GET_20PORT_INFO VhcdEvtRootHubGet20PortInfo;
EVT_UCX_ROOTHUB_GET_30PORT_INFO VhcdEvtRootHubGet30PortInfo;
EVT_UCX_USBDEVICE_ENDPOINTS_CONFIGURE VhcdEvtUsbDeviceEndpointsConfigure;
EVT_UCX_USBDEVICE_ENABLE VhcdEvtUsbDeviceEnable;
EVT_UCX_USBDEVICE_DISABLE VhcdEvtUsbDeviceDisable;
EVT_UCX_USBDEVICE_RESET VhcdEvtUsbDeviceReset;
EVT_UCX_USBDEVICE_ADDRESS VhcdEvtUsbDeviceAddress;
EVT_UCX_USBDEVICE_UPDATE VhcdEvtUsbDeviceUpdate;
EVT_UCX_USBDEVICE_HUB_INFO VhcdEvtUsbDeviceHubInfo;
EVT_UCX_USBDEVICE_DEFAULT_ENDPOINT_ADD VhcdEvtUsbDeviceDefaultEndpointAdd;
EVT_UCX_USBDEVICE_ENDPOINT_ADD VhcdEvtUsbDeviceEndpointAdd;
EVT_UCX_ENDPOINT_PURGE VhcdEvtEndpointPurge;
EVT_UCX_ENDPOINT_START VhcdEvtEndpointStart;
EVT_UCX_ENDPOINT_ABORT VhcdEvtEndpointAbort;
EVT_UCX_ENDPOINT_OK_TO_CANCEL_TRANSFERS VhcdEvtEndpointOkToCancelTransfers;
EVT_UCX_ENDPOINT_RESET VhcdEvtEndpointReset;
EVT_UCX_DEFAULT_ENDPOINT_UPDATE VhcdEvtDefaultEndpointUpdate;

VOID VhcdEvtEndpointPurge(UCXCONTROLLER C, UCXENDPOINT E) { UNREFERENCED_PARAMETER(C); VhcdCancelEndpointTransfers(E); }
VOID VhcdEvtEndpointStart(UCXCONTROLLER C, UCXENDPOINT E) { UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(E); }
VOID VhcdEvtEndpointAbort(UCXCONTROLLER C, UCXENDPOINT E) { UNREFERENCED_PARAMETER(C); VhcdCancelEndpointTransfers(E); }
VOID VhcdEvtEndpointOkToCancelTransfers(UCXENDPOINT E) { UNREFERENCED_PARAMETER(E); }
VOID VhcdEvtEndpointReset(UCXCONTROLLER C, UCXENDPOINT E, WDFREQUEST R) { UNREFERENCED_PARAMETER(C); VhcdCancelEndpointTransfers(E); WdfRequestComplete(R, STATUS_SUCCESS); }
VOID VhcdEvtDefaultEndpointUpdate(UCXCONTROLLER C, WDFREQUEST R) { UNREFERENCED_PARAMETER(C); WdfRequestComplete(R, STATUS_SUCCESS); }

static VOID VhcdReleaseSlot(_In_ LONG SlotIndex, _In_ BOOLEAN SignalRemoval)
{
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_PORTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (gSlots[SlotIndex].Allocated) {
        gSlots[SlotIndex].Present = FALSE;
        gSlots[SlotIndex].Enabled = FALSE;
        gSlots[SlotIndex].ConnectionChanged = TRUE;
        gSlots[SlotIndex].RemoveRequested = TRUE;
    }
    WdfSpinLockRelease(gSlotLock);
    if (gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
    VhcdCancelSlotTransactions(SlotIndex, 0, TRUE);
    UNREFERENCED_PARAMETER(SignalRemoval);
    VhcdFinalizeSlotIfQuiescent(SlotIndex);
}

static VOID VhcdFinalizeSlotIfQuiescent(_In_ LONG SlotIndex)
{
    PKSEMAPHORE semaphore = NULL;
    PKEVENT removeEvent = NULL;
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_PORTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (gSlots[SlotIndex].Allocated && gSlots[SlotIndex].RemoveRequested &&
        !gSlots[SlotIndex].ConnectionChanged && gSlots[SlotIndex].UsbDeviceRefs == 0 &&
        gSlots[SlotIndex].TransferRefs == 0) {
        semaphore = gSlots[SlotIndex].Semaphore;
        removeEvent = gSlots[SlotIndex].RemoveEvent;
        if (gSlots[SlotIndex].BusAddress > 0 && gSlots[SlotIndex].BusAddress <= 127)
            gAddressInUse[gSlots[SlotIndex].BusAddress] = FALSE;
        RtlZeroMemory(&gSlots[SlotIndex], sizeof(gSlots[SlotIndex]));
    }
    WdfSpinLockRelease(gSlotLock);
    if (removeEvent != NULL) KeSetEvent(removeEvent, IO_NO_INCREMENT, FALSE);
    if (removeEvent != NULL) ObDereferenceObject(removeEvent);
    if (semaphore != NULL) ObDereferenceObject(semaphore);
}

static VOID VhcdUsbDeviceCleanup(WDFOBJECT Object)
{
    PVHCD_USBDEVICE_CONTEXT context = VhcdGetUsbDeviceContext((UCXUSBDEVICE)Object);
    LONG slot = (LONG)context->SlotIndex;
    if (slot < 0 || slot >= VHCD_MAX_PORTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (gSlots[slot].UsbDeviceRefs > 0) --gSlots[slot].UsbDeviceRefs;
    WdfSpinLockRelease(gSlotLock);
    VhcdFinalizeSlotIfQuiescent(slot);
}

VOID VhcdEvtFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    PVHCD_FILE_CONTEXT context = VhcdGetFileContext(FileObject);
    UNREFERENCED_PARAMETER(Device);
    context->Signature = VHCD_FILE_SIGNATURE;
    context->SlotIndex = -1;
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID VhcdEvtFileCleanup(WDFFILEOBJECT FileObject)
{
    PVHCD_FILE_CONTEXT context = VhcdGetFileContext(FileObject);
    if (context != NULL && context->Signature == VHCD_FILE_SIGNATURE && context->SlotIndex >= 0) {
        VhcdReleaseSlot(context->SlotIndex, TRUE);
        context->SlotIndex = -1;
    }
}

VOID VhcdEvtFileClose(WDFFILEOBJECT FileObject)
{
    PVHCD_FILE_CONTEXT context = VhcdGetFileContext(FileObject);
    if (context != NULL) {
        context->Signature = 0;
        context->SlotIndex = -1;
    }
}

VOID VhcdUserIoContextCleanup(WDFOBJECT Object)
{
    PVHCD_USER_IO_CONTEXT context = VhcdGetUserIoContext(Object);
    if (context->AddRemoveEvent != NULL) ObDereferenceObject(context->AddRemoveEvent);
    if (context->AddSemaphore != NULL) ObDereferenceObject(context->AddSemaphore);
}

static NTSTATUS VhcdCreateEndpoint(UCXUSBDEVICE UsbDevice, PUCXENDPOINT_INIT EndpointInit, BOOLEAN IsDefault, UCHAR EndpointAddress, UCHAR PipeType, UCXENDPOINT* Endpoint)
{
    WDF_IO_QUEUE_CONFIG q;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES endpointAttributes;
    WDFQUEUE queue;
    NTSTATUS status;
    UCX_ENDPOINT_EVENT_CALLBACKS callbacks;
    UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS defaultCallbacks;
    WDF_IO_QUEUE_CONFIG_INIT(&q, WdfIoQueueDispatchParallel);
    q.EvtIoInternalDeviceControl = VhcdEvtEndpointIoInternalDeviceControl;
    q.EvtIoCanceledOnQueue = VhcdEvtIoCanceledOnQueue;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, VHCD_QUEUE_CONTEXT);
    status = WdfIoQueueCreate(gDevice, &q, &a, &queue);
    if (!NT_SUCCESS(status)) return status;
    VhcdGetQueueContext(queue)->SlotIndex = VhcdGetUsbDeviceContext(UsbDevice)->SlotIndex;
    VhcdGetQueueContext(queue)->EndpointAddress = EndpointAddress;
    VhcdGetQueueContext(queue)->PipeType = PipeType;
    if (IsDefault) {
        UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS_INIT(&defaultCallbacks, VhcdEvtEndpointPurge, VhcdEvtEndpointStart, VhcdEvtEndpointAbort, VhcdEvtEndpointOkToCancelTransfers, VhcdEvtDefaultEndpointUpdate);
        UcxDefaultEndpointInitSetEventCallbacks(EndpointInit, &defaultCallbacks);
    } else {
        UCX_ENDPOINT_EVENT_CALLBACKS_INIT(&callbacks, VhcdEvtEndpointPurge, VhcdEvtEndpointStart, VhcdEvtEndpointAbort, VhcdEvtEndpointReset, VhcdEvtEndpointOkToCancelTransfers, NULL, NULL, NULL);
        UcxEndpointInitSetEventCallbacks(EndpointInit, &callbacks);
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&endpointAttributes, VHCD_ENDPOINT_CONTEXT);
    status = UcxEndpointCreate(UsbDevice, &EndpointInit, &endpointAttributes, Endpoint);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(queue); return status; }
    VhcdGetEndpointContext(*Endpoint)->SlotIndex = VhcdGetUsbDeviceContext(UsbDevice)->SlotIndex;
    VhcdGetEndpointContext(*Endpoint)->EndpointAddress = EndpointAddress;
    VhcdGetEndpointContext(*Endpoint)->PipeType = PipeType;
    UcxEndpointSetWdfIoQueue(*Endpoint, queue);
    return STATUS_SUCCESS;
}

NTSTATUS VhcdEvtUsbDeviceDefaultEndpointAdd(UCXCONTROLLER Controller, UCXUSBDEVICE UsbDevice, ULONG MaxPacketSize, PUCXENDPOINT_INIT EndpointInit)
{
    UCXENDPOINT endpoint;
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(MaxPacketSize);
    return VhcdCreateEndpoint(UsbDevice, EndpointInit, TRUE, 0, UsbdPipeTypeControl, &endpoint);
}
NTSTATUS VhcdEvtUsbDeviceEndpointAdd(UCXCONTROLLER Controller, UCXUSBDEVICE UsbDevice, PUSB_ENDPOINT_DESCRIPTOR Descriptor, ULONG DescriptorLength, PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR Companion, PUCXENDPOINT_INIT EndpointInit)
{
    UCXENDPOINT endpoint;
    if (Descriptor == NULL || DescriptorLength < sizeof(*Descriptor) || Descriptor->bLength < sizeof(*Descriptor))
        return STATUS_INVALID_PARAMETER;
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(Companion);
    UCHAR pipeType = (UCHAR)(Descriptor->bmAttributes & 0x03);
    return VhcdCreateEndpoint(UsbDevice, EndpointInit, FALSE, Descriptor->bEndpointAddress, pipeType, &endpoint);
}
NTSTATUS VhcdEvtControllerUsbDeviceAdd(UCXCONTROLLER Controller, PUCXUSBDEVICE_INFO DeviceInfo, PUCXUSBDEVICE_INIT UsbDeviceInit)
{
    UCX_USBDEVICE_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    UCXUSBDEVICE usbDevice;
    PVHCD_USBDEVICE_CONTEXT deviceContext;
    ULONG portNumber;
    NTSTATUS status;
    if (DeviceInfo == NULL || DeviceInfo->PortPath.PortPathDepth == 0) return STATUS_NOT_SUPPORTED;
    portNumber = DeviceInfo->PortPath.PortPath[0];
    if (portNumber == 0 || portNumber > VHCD_MAX_PORTS) return STATUS_NOT_SUPPORTED;
    WdfSpinLockAcquire(gSlotLock);
    if (!gSlots[portNumber - 1].Allocated || !gSlots[portNumber - 1].Present) {
        WdfSpinLockRelease(gSlotLock);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    ++gSlots[portNumber - 1].UsbDeviceRefs;
    WdfSpinLockRelease(gSlotLock);
    UCX_USBDEVICE_EVENT_CALLBACKS_INIT(&callbacks, VhcdEvtUsbDeviceEndpointsConfigure, VhcdEvtUsbDeviceEnable, VhcdEvtUsbDeviceDisable, VhcdEvtUsbDeviceReset, VhcdEvtUsbDeviceAddress, VhcdEvtUsbDeviceUpdate, VhcdEvtUsbDeviceHubInfo, VhcdEvtUsbDeviceDefaultEndpointAdd, VhcdEvtUsbDeviceEndpointAdd);
    UcxUsbDeviceInitSetEventCallbacks(UsbDeviceInit, &callbacks);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_USBDEVICE_CONTEXT);
    attributes.EvtCleanupCallback = VhcdUsbDeviceCleanup;
    status = UcxUsbDeviceCreate(Controller, &UsbDeviceInit, &attributes, &usbDevice);
    if (NT_SUCCESS(status)) {
        deviceContext = VhcdGetUsbDeviceContext(usbDevice);
        deviceContext->SlotIndex = portNumber - 1;
    } else {
        WdfSpinLockAcquire(gSlotLock);
        if (gSlots[portNumber - 1].UsbDeviceRefs > 0) --gSlots[portNumber - 1].UsbDeviceRefs;
        WdfSpinLockRelease(gSlotLock);
        VhcdFinalizeSlotIfQuiescent((LONG)portNumber - 1);
    }
    return status;
}
static PVOID VhcdRequestArgument1(WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS parameters;
    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(Request, &parameters);
    return parameters.Parameters.Others.Arg1;
}

VOID VhcdEvtUsbDeviceEndpointsConfigure(UCXCONTROLLER C, WDFREQUEST R)
{
    PENDPOINTS_CONFIGURE request = (PENDPOINTS_CONFIGURE)VhcdRequestArgument1(R);
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    RtlZeroMemory(&request->FailureFlags, sizeof(request->FailureFlags));
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceEnable(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_ENABLE request = (PUSBDEVICE_ENABLE)VhcdRequestArgument1(R);
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    RtlZeroMemory(&request->FailureFlags, sizeof(request->FailureFlags));
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceDisable(UCXCONTROLLER C, WDFREQUEST R) { UNREFERENCED_PARAMETER(C); WdfRequestComplete(R, STATUS_SUCCESS); }
VOID VhcdEvtUsbDeviceReset(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_RESET request = (PUSBDEVICE_RESET)VhcdRequestArgument1(R);
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceAddress(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_ADDRESS request = (PUSBDEVICE_ADDRESS)VhcdRequestArgument1(R);
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceUpdate(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_UPDATE request = (PUSBDEVICE_UPDATE)VhcdRequestArgument1(R);
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    RtlZeroMemory(&request->FailureFlags, sizeof(request->FailureFlags));
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceHubInfo(UCXCONTROLLER C, WDFREQUEST R) { UNREFERENCED_PARAMETER(C); WdfRequestComplete(R, STATUS_SUCCESS); }

static PURB VhcdRequestUrb(WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS p;
    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    return (PURB)p.Parameters.Others.Arg1;
}

static PUCHAR VhcdTransferBuffer(ULONG Length, PVOID Buffer, PMDL Mdl)
{
    if (Length == 0) return NULL;
    if (Mdl != NULL) {
        if (MmGetMdlByteCount(Mdl) < Length) return NULL;
        return (PUCHAR)MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority | MdlMappingNoExecute);
    }
    return (PUCHAR)Buffer;
}

static VOID VhcdReadPort(_In_ USHORT Port, _Out_ PUSHORT Status, _Out_ PUSHORT Change)
{
    LONG i = (LONG)Port - 1;
    *Status = USB_PORT_STATUS_POWER;
    *Change = 0;
    if (i < 0 || i >= VHCD_MAX_PORTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (gSlots[i].Present) *Status |= USB_PORT_STATUS_CONNECT | USB_PORT_STATUS_HIGH_SPEED;
    if (gSlots[i].Enabled) *Status |= USB_PORT_STATUS_ENABLE;
    if (gSlots[i].ConnectionChanged) *Change |= 0x0001;
    if (gSlots[i].ResetChanged) *Change |= 0x0010;
    WdfSpinLockRelease(gSlotLock);
}

VOID VhcdEvtRootHubControlUrb(UCXROOTHUB H, WDFREQUEST R)
{
    PURB urb = VhcdRequestUrb(R);
    UCHAR* setup;
    PUCHAR data;
    ULONG length;
    USHORT value, index, statusWord, changeWord;
    UNREFERENCED_PARAMETER(H);
    if (urb == NULL || urb->UrbHeader.Function != URB_FUNCTION_CONTROL_TRANSFER) {
        if (urb != NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    setup = urb->UrbControlTransfer.SetupPacket;
    value = (USHORT)(setup[2] | ((USHORT)setup[3] << 8));
    index = (USHORT)(setup[4] | ((USHORT)setup[5] << 8));
    length = urb->UrbControlTransfer.TransferBufferLength;
    data = VhcdTransferBuffer(length, urb->UrbControlTransfer.TransferBuffer, urb->UrbControlTransfer.TransferBufferMDL);
    urb->UrbHeader.Status = USBD_STATUS_SUCCESS;

    if (setup[1] == USB_REQUEST_GET_DESCRIPTOR && (setup[0] & 0x60) == 0x20 && (value >> 8) == USB_20_HUB_DESCRIPTOR_TYPE) {
        UCHAR hub[13] = { 13, USB_20_HUB_DESCRIPTOR_TYPE, VHCD_MAX_PORTS, 0x01, 0x00, 0x01, 0x00,
                          0x00, 0x00, 0x00, 0xfe, 0xff, 0x01 };
        ULONG copyLength;
        /* 16 ports need three bytes per bitmap (hub bit plus 16 port bits). */
        copyLength = min(length, (ULONG)sizeof(hub));
        if (copyLength != 0 && data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        else if (copyLength != 0) RtlCopyMemory(data, hub, copyLength);
        urb->UrbControlTransfer.TransferBufferLength = copyLength;
    } else if (setup[1] == USB_REQUEST_GET_STATUS && (setup[0] & 0x60) == 0x20) {
        if ((setup[0] & 0x1f) == 0) {
            ULONG hubStatus = 0;
            if (length < sizeof(hubStatus) || data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
            else { RtlCopyMemory(data, &hubStatus, sizeof(hubStatus)); urb->UrbControlTransfer.TransferBufferLength = sizeof(hubStatus); }
        } else if ((setup[0] & 0x1f) == 3 && index != 0) {
            VhcdReadPort(index, &statusWord, &changeWord);
            if (length < sizeof(ULONG) || data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
            else { ULONG portStatus = statusWord | ((ULONG)changeWord << 16); RtlCopyMemory(data, &portStatus, sizeof(portStatus)); urb->UrbControlTransfer.TransferBufferLength = sizeof(portStatus); }
        } else {
            urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        }
    } else if ((setup[1] == USB_REQUEST_SET_FEATURE || setup[1] == USB_REQUEST_CLEAR_FEATURE) && (setup[0] & 0x60) == 0x20 && (setup[0] & 0x1f) == 0) {
        /* The virtual root hub has no hub-wide power or over-current state. */
        urb->UrbControlTransfer.TransferBufferLength = 0;
    } else if ((setup[1] == USB_REQUEST_SET_FEATURE || setup[1] == USB_REQUEST_CLEAR_FEATURE) && (setup[0] & 0x1f) == 3 && index > 0 && index <= VHCD_MAX_PORTS) {
        LONG i = (LONG)index - 1;
        BOOLEAN changed = FALSE;
        BOOLEAN supported = TRUE;
        BOOLEAN resetRequested = FALSE;
        WdfSpinLockAcquire(gSlotLock);
        if (value == 8 && (setup[1] == USB_REQUEST_SET_FEATURE || setup[1] == USB_REQUEST_CLEAR_FEATURE)) { /* PORT_POWER */ }
        else if (value == 4 && setup[1] == USB_REQUEST_SET_FEATURE) { resetRequested = TRUE; }
        else if (value == 16 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].ConnectionChanged = FALSE; }
        else if (value == 17 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].Enabled = FALSE; }
        else if (value == 20 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].ResetChanged = FALSE; }
        else supported = FALSE;
        WdfSpinLockRelease(gSlotLock);
        if (resetRequested) {
            NTSTATUS resetStatus = VhcdQueuePortResetNotification(i, 1);
            if (NT_SUCCESS(resetStatus)) {
                WdfSpinLockAcquire(gSlotLock);
                if (gSlots[i].Allocated && gSlots[i].Present) {
                    gSlots[i].Enabled = TRUE;
                    gSlots[i].ResetChanged = TRUE;
                    changed = TRUE;
                } else {
                    resetStatus = STATUS_DEVICE_NOT_CONNECTED;
                }
                WdfSpinLockRelease(gSlotLock);
            }
            if (!NT_SUCCESS(resetStatus)) {
                supported = FALSE;
                urb->UrbHeader.Status = USBD_STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        if (value == 16 && setup[1] == USB_REQUEST_CLEAR_FEATURE) VhcdFinalizeSlotIfQuiescent(i);
        if (changed && gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
        urb->UrbControlTransfer.TransferBufferLength = 0;
        if (!supported) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
    } else {
        urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
    }
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubInterruptTx(UCXROOTHUB H, WDFREQUEST R)
{
    PURB urb = VhcdRequestUrb(R);
    PUCHAR data;
    ULONG length, bytes;
    ULONG bitmap = 0;
    LONG i;
    UNREFERENCED_PARAMETER(H);
    if (urb == NULL || urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
        if (urb != NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    length = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    data = VhcdTransferBuffer(length, urb->UrbBulkOrInterruptTransfer.TransferBuffer, urb->UrbBulkOrInterruptTransfer.TransferBufferMDL);
    WdfSpinLockAcquire(gSlotLock);
    for (i = 0; i < VHCD_MAX_PORTS; ++i) if (gSlots[i].ConnectionChanged || gSlots[i].ResetChanged) bitmap |= 1u << (i + 1);
    WdfSpinLockRelease(gSlotLock);
    bytes = (VHCD_MAX_PORTS + 1 + 7) / 8;
    if (length < bytes || data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
    else { RtlZeroMemory(data, bytes); RtlCopyMemory(data, &bitmap, bytes); urb->UrbHeader.Status = USBD_STATUS_SUCCESS; urb->UrbBulkOrInterruptTransfer.TransferBufferLength = bytes; }
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubGetInfo(UCXROOTHUB H, WDFREQUEST R)
{
    WDF_REQUEST_PARAMETERS p;
    PROOTHUB_INFO info;
    UNREFERENCED_PARAMETER(H);
    WDF_REQUEST_PARAMETERS_INIT(&p); WdfRequestGetParameters(R, &p);
    info = (PROOTHUB_INFO)p.Parameters.Others.Arg1;
    if (info == NULL || info->Size < sizeof(*info)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    info->ControllerType = ControllerTypeSoftXhci;
    info->NumberOf20Ports = VHCD_MAX_PORTS;
    info->NumberOf30Ports = 0;
    info->MaxU1ExitLatency = 0;
    info->MaxU2ExitLatency = 0;
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubGet20PortInfo(UCXROOTHUB H, WDFREQUEST R)
{
    WDF_REQUEST_PARAMETERS p;
    PROOTHUB_20PORTS_INFO info;
    USHORT i;
    UNREFERENCED_PARAMETER(H);
    WDF_REQUEST_PARAMETERS_INIT(&p); WdfRequestGetParameters(R, &p);
    info = (PROOTHUB_20PORTS_INFO)p.Parameters.Others.Arg1;
    if (info == NULL || info->PortInfoArray == NULL || info->NumberOfPorts < VHCD_MAX_PORTS || info->PortInfoSize < sizeof(ROOTHUB_20PORT_INFO)) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
    for (i = 0; i < VHCD_MAX_PORTS; ++i) {
        PROOTHUB_20PORT_INFO port = info->PortInfoArray[i];
        if (port == NULL) { WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return; }
        port->PortNumber = i + 1; port->MinorRevision = 0; port->HubDepth = 0;
        port->Removable = TriStateTrue; port->IntegratedHubImplemented = TriStateFalse; port->DebugCapable = TriStateFalse;
        port->ControllerUsb20HardwareLpmFlags.AsUchar = 0;
    }
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubGet30PortInfo(UCXROOTHUB H, WDFREQUEST R)
{
    UNREFERENCED_PARAMETER(H);
    WdfRequestComplete(R, STATUS_SUCCESS);
}

static VOID VhcdTxAddRef(PVHCD_TRANSACTION Transaction)
{
    InterlockedIncrement(&Transaction->References);
}

static VOID VhcdTxRelease(PVHCD_TRANSACTION Transaction)
{
    if (InterlockedDecrement(&Transaction->References) == 0)
        ExFreePoolWithTag(Transaction, VHCD_TX_SIGNATURE);
}

static VOID VhcdFinishTransfer(PVHCD_TRANSACTION Transaction, NTSTATUS Status, USBD_STATUS UsbStatus)
{
    LONG slot = Transaction->SlotIndex;
    if (InterlockedExchange(&Transaction->OwnerFinished, 1) != 0) return;
    if (Transaction->Urb != NULL) Transaction->Urb->UrbHeader.Status = UsbStatus;
    if (Transaction->Request != NULL) WdfRequestComplete(Transaction->Request, Status);
    WdfSpinLockAcquire(gSlotLock);
    if (slot >= 0 && slot < VHCD_MAX_PORTS && gSlots[slot].TransferRefs > 0)
        --gSlots[slot].TransferRefs;
    if (slot >= 0 && slot < VHCD_MAX_PORTS && gSlots[slot].TransactionBytes >= Transaction->AllocationBytes)
        gSlots[slot].TransactionBytes -= Transaction->AllocationBytes;
    if (slot >= 0 && slot < VHCD_MAX_PORTS && gSlots[slot].TransactionCount > 0)
        --gSlots[slot].TransactionCount;
    if (gTransactionBytes >= Transaction->AllocationBytes) gTransactionBytes -= Transaction->AllocationBytes;
    if (gTransactionCount > 0) --gTransactionCount;
    WdfSpinLockRelease(gSlotLock);
    VhcdFinalizeSlotIfQuiescent(slot);
    VhcdTxRelease(Transaction); /* Release the pending USB request's ownership. */
}

static VOID VhcdTransactionCancelDpc(_In_ PKDPC Dpc, _In_opt_ PVOID DeferredContext,
                                     _In_opt_ PVOID SystemArgument1, _In_opt_ PVOID SystemArgument2)
{
    PVHCD_TRANSACTION transaction = (PVHCD_TRANSACTION)DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    VhcdFinishTransfer(transaction, STATUS_CANCELLED, USBD_STATUS_CANCELED);
}

static VOID VhcdQueueTransactionCancellation(PVHCD_TRANSACTION Transaction)
{
    if (InterlockedCompareExchange(&Transaction->CancelCompletionQueued, 1, 0) != 0) return;
    KeInitializeDpc(&Transaction->CancelDpc, VhcdTransactionCancelDpc, Transaction);
    KeInsertQueueDpc(&Transaction->CancelDpc, NULL, NULL);
}

static NTSTATUS VhcdQueuePortResetNotification(LONG SlotIndex, LONG ResetType)
{
    PVHCD_TRANSACTION transaction;
    PKSEMAPHORE semaphore;
    ULONG allocationBytes = sizeof(*transaction) + sizeof(ioctl_usbtx_header_t);
    ioctl_usbtx_header_t* outer;
    PVHCD_WIRE_REQUEST inner;
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_PORTS) return STATUS_INVALID_PARAMETER;
    transaction = (PVHCD_TRANSACTION)ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationBytes, VHCD_TX_SIGNATURE);
    if (transaction == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(transaction, allocationBytes);
    transaction->Signature = VHCD_TX_SIGNATURE;
    transaction->SlotIndex = SlotIndex;
    transaction->EndpointAddress = 0;
    transaction->AllocationBytes = allocationBytes;
    transaction->PayloadCapacity = 0;
    transaction->References = 1; /* Owned until END or port teardown. */
    transaction->CancelableReady = TRUE;
    outer = (ioctl_usbtx_header_t*)transaction->Buffer;
    inner = (PVHCD_WIRE_REQUEST)((PUCHAR)transaction->Buffer + FIELD_OFFSET(ioctl_usbtx_header_t, type));
    outer->data_length = 0;
    outer->result = -1;
    inner->Type = 4;
    inner->Result = -1;
    inner->DataLength = 0;
    inner->Data.Reset.Type = ResetType;

    WdfSpinLockAcquire(gSlotLock);
    if (!gSlots[SlotIndex].Allocated || !gSlots[SlotIndex].Present ||
        gSlots[SlotIndex].TransactionCount >= VHCD_MAX_SLOT_TRANSACTIONS ||
        gSlots[SlotIndex].TransactionBytes > VHCD_MAX_SLOT_TRANSACTION_BYTES ||
        allocationBytes > VHCD_MAX_SLOT_TRANSACTION_BYTES - gSlots[SlotIndex].TransactionBytes ||
        gTransactionCount >= VHCD_MAX_CONTROLLER_TRANSACTIONS ||
        gTransactionBytes > VHCD_MAX_CONTROLLER_TRANSACTION_BYTES ||
        allocationBytes > VHCD_MAX_CONTROLLER_TRANSACTION_BYTES - gTransactionBytes) {
        WdfSpinLockRelease(gSlotLock);
        ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    transaction->Id = ++gSlots[SlotIndex].NextTransactionId;
    transaction->Sequence = ++gSlots[SlotIndex].NextSequence;
    outer->inter_handle = transaction->Id;
    outer->inter_seqno = transaction->Sequence;
    InsertTailList(&gSlots[SlotIndex].Transactions, &transaction->Link);
    transaction->Linked = TRUE;
    ++gSlots[SlotIndex].TransferRefs;
    gSlots[SlotIndex].TransactionBytes += allocationBytes;
    ++gSlots[SlotIndex].TransactionCount;
    gTransactionBytes += allocationBytes;
    ++gTransactionCount;
    semaphore = gSlots[SlotIndex].Semaphore;
    ObReferenceObject(semaphore);
    KeReleaseSemaphore(semaphore, IO_NO_INCREMENT, 1, FALSE);
    WdfSpinLockRelease(gSlotLock);
    ObDereferenceObject(semaphore);
    return STATUS_SUCCESS;
}

VOID VhcdEvtTransferCanceled(WDFREQUEST Request)
{
    PVHCD_TRANSFER_CONTEXT context = VhcdGetTransferContext(Request);
    PVHCD_TRANSACTION transaction = (PVHCD_TRANSACTION)context->Transaction;
    if (transaction == NULL) return;
    WdfSpinLockAcquire(gSlotLock);
    if (transaction->Linked) {
        RemoveEntryList(&transaction->Link);
        transaction->Linked = FALSE;
    }
    transaction->CancelRequested = TRUE;
    {
        BOOLEAN ready = transaction->CancelableReady;
        WdfSpinLockRelease(gSlotLock);
        if (ready) VhcdQueueTransactionCancellation(transaction);
    }
}

static VOID VhcdCancelSlotTransactions(LONG SlotIndex, UCHAR EndpointAddress, BOOLEAN AllEndpoints)
{
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_PORTS) return;
    for (;;) {
        PVHCD_TRANSACTION transaction = NULL;
        BOOLEAN ready = FALSE;
        WdfSpinLockAcquire(gSlotLock);
        if (gSlots[SlotIndex].Allocated) {
            for (PLIST_ENTRY entry = gSlots[SlotIndex].Transactions.Flink; entry != &gSlots[SlotIndex].Transactions; entry = entry->Flink) {
                PVHCD_TRANSACTION candidate = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
                if (!candidate->Ending && (AllEndpoints || candidate->EndpointAddress == EndpointAddress)) {
                    candidate->Ending = TRUE;
                    candidate->CancelRequested = TRUE;
                    candidate->Linked = FALSE;
                    RemoveEntryList(&candidate->Link);
                    ready = candidate->CancelableReady;
                    VhcdTxAddRef(candidate);
                    transaction = candidate;
                    break;
                }
            }
        }
        WdfSpinLockRelease(gSlotLock);
        if (transaction == NULL) break;
        if (ready) {
            if (transaction->Request != NULL) (VOID)WdfRequestUnmarkCancelable(transaction->Request);
            VhcdQueueTransactionCancellation(transaction);
        }
        VhcdTxRelease(transaction);
    }
}

static VOID VhcdCancelEndpointTransfers(UCXENDPOINT Endpoint)
{
    PVHCD_ENDPOINT_CONTEXT context = VhcdGetEndpointContext(Endpoint);
    if (context != NULL) VhcdCancelSlotTransactions((LONG)context->SlotIndex, context->EndpointAddress, FALSE);
}

NTSTATUS VhcdEvtControllerQueryUsbCapability(UCXCONTROLLER Controller, PGUID CapabilityType,
                                              ULONG OutputBufferLength, PVOID OutputBuffer, PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(CapabilityType);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    if (ResultLength == NULL) return STATUS_INVALID_PARAMETER;
    *ResultLength = 0;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VhcdEvtControllerGetCurrentFrameNumber(UCXCONTROLLER Controller, PULONG FrameNumber)
{
    UNREFERENCED_PARAMETER(Controller);
    if (FrameNumber == NULL) return STATUS_INVALID_PARAMETER;
    *FrameNumber = (ULONG)(KeQueryInterruptTime() / 10000ULL);
    return STATUS_SUCCESS;
}

VOID VhcdEvtControllerReset(UCXCONTROLLER Controller)
{
    UCX_CONTROLLER_RESET_COMPLETE_INFO resetInfo;
    UCX_CONTROLLER_RESET_COMPLETE_INFO_INIT(&resetInfo, UcxControllerStatePreserved, TRUE);
    UcxControllerResetComplete(Controller, &resetInfo);
}

static NTSTATUS VhcdMapUrb(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ PURB Urb,
                           _Out_ PVHCD_TRANSACTION* OutTransaction)
{
    PVHCD_QUEUE_CONTEXT queueContext = VhcdGetQueueContext(Queue);
    LONG slot = (LONG)queueContext->SlotIndex;
    ULONG transferLength = 0, metadataLength = 0, payloadLength = 0, allocationBytes;
    PUCHAR transferBuffer = NULL;
    PMDL transferMdl = NULL;
    ULONG transferFlags = 0;
    BOOLEAN isRead = FALSE, isIso = FALSE;
    USHORT function = Urb->UrbHeader.Function;
    PVHCD_TRANSACTION transaction;
    PKSEMAPHORE signalSemaphore = NULL;
    *OutTransaction = NULL;
    if (slot < 0 || slot >= VHCD_MAX_PORTS) return STATUS_INVALID_DEVICE_STATE;

    switch (function) {
    case URB_FUNCTION_CONTROL_TRANSFER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlTransfer) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlTransfer.TransferBufferLength;
        transferBuffer = (PUCHAR)Urb->UrbControlTransfer.TransferBuffer;
        transferMdl = Urb->UrbControlTransfer.TransferBufferMDL;
        transferFlags = Urb->UrbControlTransfer.TransferFlags;
        break;
    case URB_FUNCTION_CONTROL_TRANSFER_EX:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlTransferEx) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlTransferEx.TransferBufferLength;
        transferBuffer = (PUCHAR)Urb->UrbControlTransferEx.TransferBuffer;
        transferMdl = Urb->UrbControlTransferEx.TransferBufferMDL;
        transferFlags = Urb->UrbControlTransferEx.TransferFlags;
        break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlDescriptorRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlDescriptorRequest.TransferBufferLength;
        transferBuffer = (PUCHAR)Urb->UrbControlDescriptorRequest.TransferBuffer;
        transferMdl = Urb->UrbControlDescriptorRequest.TransferBufferMDL;
        isRead = function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE || function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT || function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE;
        break;
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlVendorClassRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlVendorClassRequest.TransferBufferLength;
        transferBuffer = (PUCHAR)Urb->UrbControlVendorClassRequest.TransferBuffer;
        transferMdl = Urb->UrbControlVendorClassRequest.TransferBufferMDL;
        transferFlags = Urb->UrbControlVendorClassRequest.TransferFlags;
        break;
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlGetStatusRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlGetStatusRequest.TransferBufferLength;
        if (transferLength < sizeof(USHORT)) return STATUS_BUFFER_TOO_SMALL;
        transferBuffer = (PUCHAR)Urb->UrbControlGetStatusRequest.TransferBuffer;
        transferMdl = Urb->UrbControlGetStatusRequest.TransferBufferMDL;
        isRead = TRUE;
        break;
    case URB_FUNCTION_GET_CONFIGURATION:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlGetConfigurationRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlGetConfigurationRequest.TransferBufferLength;
        if (transferLength < sizeof(UCHAR)) return STATUS_BUFFER_TOO_SMALL;
        transferBuffer = (PUCHAR)Urb->UrbControlGetConfigurationRequest.TransferBuffer;
        transferMdl = Urb->UrbControlGetConfigurationRequest.TransferBufferMDL;
        isRead = TRUE;
        break;
    case URB_FUNCTION_GET_INTERFACE:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlGetInterfaceRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbControlGetInterfaceRequest.TransferBufferLength;
        if (transferLength < sizeof(UCHAR)) return STATUS_BUFFER_TOO_SMALL;
        transferBuffer = (PUCHAR)Urb->UrbControlGetInterfaceRequest.TransferBuffer;
        transferMdl = Urb->UrbControlGetInterfaceRequest.TransferBufferMDL;
        isRead = TRUE;
        break;
    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbOSFeatureDescriptorRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength;
        if (transferLength < sizeof(UCHAR)) return STATUS_BUFFER_TOO_SMALL;
        transferBuffer = (PUCHAR)Urb->UrbOSFeatureDescriptorRequest.TransferBuffer;
        transferMdl = Urb->UrbOSFeatureDescriptorRequest.TransferBufferMDL;
        isRead = TRUE;
        break;
    case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbGetCurrentFrameNumber) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        transferLength = sizeof(Urb->UrbGetCurrentFrameNumber.FrameNumber);
        transferBuffer = (PUCHAR)&Urb->UrbGetCurrentFrameNumber.FrameNumber;
        isRead = TRUE;
        break;
    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_SET_FEATURE_TO_OTHER:
    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlFeatureRequest) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        break;
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbPipeRequest) || queueContext->PipeType == UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        VhcdCancelSlotTransactions(slot, queueContext->EndpointAddress, FALSE);
        break;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbBulkOrInterruptTransfer) || (queueContext->PipeType != UsbdPipeTypeBulk && queueContext->PipeType != UsbdPipeTypeInterrupt)) return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        transferBuffer = (PUCHAR)Urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        transferMdl = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;
        transferFlags = Urb->UrbBulkOrInterruptTransfer.TransferFlags;
        break;
    case URB_FUNCTION_ISOCH_TRANSFER: {
        ULONG count = Urb->UrbIsochronousTransfer.NumberOfPackets;
        ULONG headerLength = FIELD_OFFSET(iso_packet_hdr_t, packets);
        if (queueContext->PipeType != UsbdPipeTypeIsochronous || count == 0 || count > 1024 || Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) + count * sizeof(USBD_ISO_PACKET_DESCRIPTOR)) return STATUS_INVALID_PARAMETER;
        if (count > (MAXULONG - headerLength) / sizeof(iso_packet_t)) return STATUS_INVALID_PARAMETER;
        metadataLength = headerLength + count * sizeof(iso_packet_t);
        transferLength = Urb->UrbIsochronousTransfer.TransferBufferLength;
        transferBuffer = (PUCHAR)Urb->UrbIsochronousTransfer.TransferBuffer;
        transferMdl = Urb->UrbIsochronousTransfer.TransferBufferMDL;
        transferFlags = Urb->UrbIsochronousTransfer.TransferFlags;
        isIso = TRUE;
        break;
    }
    case URB_FUNCTION_SELECT_CONFIGURATION:
        if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        break;
    case URB_FUNCTION_SELECT_INTERFACE:
        if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) + FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) || queueContext->PipeType != UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    if (function == URB_FUNCTION_CONTROL_TRANSFER || function == URB_FUNCTION_CONTROL_TRANSFER_EX ||
        function == URB_FUNCTION_VENDOR_DEVICE || function == URB_FUNCTION_VENDOR_INTERFACE ||
        function == URB_FUNCTION_VENDOR_ENDPOINT || function == URB_FUNCTION_VENDOR_OTHER ||
        function == URB_FUNCTION_CLASS_DEVICE || function == URB_FUNCTION_CLASS_INTERFACE ||
        function == URB_FUNCTION_CLASS_ENDPOINT || function == URB_FUNCTION_CLASS_OTHER ||
        function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER || function == URB_FUNCTION_ISOCH_TRANSFER)
        isRead = (transferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
    if (isIso) {
        ULONG index;
        for (index = 0; index < Urb->UrbIsochronousTransfer.NumberOfPackets; ++index) {
            const USBD_ISO_PACKET_DESCRIPTOR* packet = &Urb->UrbIsochronousTransfer.IsoPacket[index];
            if (packet->Offset > transferLength || packet->Length > transferLength - packet->Offset) return STATUS_INVALID_PARAMETER;
        }
    }
    if (transferLength > VHCD_MAX_TRANSACTION_BYTES || metadataLength > MAXULONG - transferLength) return STATUS_INVALID_PARAMETER;
    payloadLength = transferLength + metadataLength;
    if (payloadLength > VHCD_MAX_TRANSACTION_BYTES) return STATUS_INVALID_PARAMETER;
    allocationBytes = sizeof(*transaction) + sizeof(ioctl_usbtx_header_t) + payloadLength;
    transaction = (PVHCD_TRANSACTION)ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationBytes, VHCD_TX_SIGNATURE);
    if (transaction == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(transaction, allocationBytes);
    transaction->Signature = VHCD_TX_SIGNATURE;
    transaction->SlotIndex = slot;
    transaction->Request = Request;
    transaction->Urb = Urb;
    transaction->TransferLength = transferLength;
    transaction->MetadataLength = metadataLength;
    transaction->PayloadCapacity = payloadLength;
    transaction->AllocationBytes = allocationBytes;
    transaction->EndpointAddress = queueContext->EndpointAddress;
    transaction->TransferBuffer = VhcdTransferBuffer(transferLength, transferBuffer, transferMdl);
    transaction->IsRead = isRead;
    transaction->Isochronous = isIso;
    transaction->References = 1;
    if (transferLength != 0 && transaction->TransferBuffer == NULL) { ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE); return STATUS_INVALID_USER_BUFFER; }

    {
        ioctl_usbtx_header_t* outer = (ioctl_usbtx_header_t*)transaction->Buffer;
        PVHCD_WIRE_REQUEST inner = (PVHCD_WIRE_REQUEST)((PUCHAR)transaction->Buffer + FIELD_OFFSET(ioctl_usbtx_header_t, type));
        outer->data_length = (LONG)payloadLength;
        outer->result = -1;
        inner->Result = -1;
        inner->DataLength = (LONG)payloadLength;
        switch (function) {
        case URB_FUNCTION_CONTROL_TRANSFER:
        case URB_FUNCTION_CONTROL_TRANSFER_EX: {
            PUCHAR setup = function == URB_FUNCTION_CONTROL_TRANSFER
                ? Urb->UrbControlTransfer.SetupPacket : Urb->UrbControlTransferEx.SetupPacket;
            inner->Type = 3;
            inner->Data.Transfer.Type = 1;
            inner->Data.Transfer.EpAddress = 0;
            inner->Data.Transfer.IsRead = isRead ? 1 : 0;
            RtlCopyMemory(inner->Data.Transfer.SetupPacket, setup, sizeof(inner->Data.Transfer.SetupPacket));
            inner->Data.Transfer.Timeout = function == URB_FUNCTION_CONTROL_TRANSFER_EX
                ? Urb->UrbControlTransferEx.Timeout : 5000;
            break;
        }
        case URB_FUNCTION_ISOCH_TRANSFER: {
            iso_packet_hdr_t* iso = (iso_packet_hdr_t*)((PUCHAR)inner + sizeof(*inner));
            ULONG i;
            inner->Type = 3;
            inner->Data.Transfer.Type = 3;
            inner->Data.Transfer.EpAddress = queueContext->EndpointAddress;
            inner->Data.Transfer.IsRead = isRead ? 1 : 0;
            inner->Data.Transfer.NumberPackets = (LONG)Urb->UrbIsochronousTransfer.NumberOfPackets;
            iso->start_frame = Urb->UrbIsochronousTransfer.StartFrame;
            iso->flags = Urb->UrbIsochronousTransfer.TransferFlags;
            iso->number_packets = Urb->UrbIsochronousTransfer.NumberOfPackets;
            iso->error_count = 0;
            for (i = 0; i < iso->number_packets; ++i) {
                iso->packets[i].offset = Urb->UrbIsochronousTransfer.IsoPacket[i].Offset;
                iso->packets[i].length = Urb->UrbIsochronousTransfer.IsoPacket[i].Length;
                iso->packets[i].status = Urb->UrbIsochronousTransfer.IsoPacket[i].Status;
            }
            break;
        }
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
            inner->Type = 3;
            inner->Data.Transfer.Type = 2;
            inner->Data.Transfer.EpAddress = queueContext->EndpointAddress;
            inner->Data.Transfer.IsRead = isRead ? 1 : 0;
            break;
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
            inner->Type = 1;
            inner->Data.Descriptor.Type = (function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE || function == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE) ? 1 :
                ((function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE || function == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE) ? 3 : 4);
            inner->Data.Descriptor.Subtype = Urb->UrbControlDescriptorRequest.DescriptorType;
            inner->Data.Descriptor.IsRead = isRead ? 1 : 0;
            inner->Data.Descriptor.Index = Urb->UrbControlDescriptorRequest.Index;
            inner->Data.Descriptor.Value = Urb->UrbControlDescriptorRequest.LanguageId;
            break;
        case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
            inner->Type = 1;
            inner->Data.Descriptor.Type = 5;
            inner->Data.Descriptor.Subtype = 0;
            inner->Data.Descriptor.IsRead = 1;
            inner->Data.Descriptor.Index = ((LONG)Urb->UrbOSFeatureDescriptorRequest.Recipient << 8) | Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber;
            inner->Data.Descriptor.Value = Urb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex;
            break;
        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        case URB_FUNCTION_VENDOR_OTHER:
        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_ENDPOINT:
        case URB_FUNCTION_CLASS_OTHER:
            inner->Type = 2;
            inner->Data.Vendor.Type = (function == URB_FUNCTION_CLASS_DEVICE || function == URB_FUNCTION_CLASS_INTERFACE ||
                function == URB_FUNCTION_CLASS_ENDPOINT || function == URB_FUNCTION_CLASS_OTHER) ? 1 : 2;
            inner->Data.Vendor.Subtype = (function == URB_FUNCTION_VENDOR_DEVICE || function == URB_FUNCTION_CLASS_DEVICE) ? 1 :
                ((function == URB_FUNCTION_VENDOR_INTERFACE || function == URB_FUNCTION_CLASS_INTERFACE) ? 2 :
                 ((function == URB_FUNCTION_VENDOR_ENDPOINT || function == URB_FUNCTION_CLASS_ENDPOINT) ? 3 : 4));
            inner->Data.Vendor.IsRead = isRead ? 1 : 0;
            inner->Data.Vendor.Request = Urb->UrbControlVendorClassRequest.Request;
            inner->Data.Vendor.Index = Urb->UrbControlVendorClassRequest.Index;
            inner->Data.Vendor.Value = Urb->UrbControlVendorClassRequest.Value;
            break;
        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        case URB_FUNCTION_GET_CONFIGURATION:
        case URB_FUNCTION_GET_INTERFACE:
        case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
            inner->Type = 5;
            inner->Data.Status.Type = function == URB_FUNCTION_GET_STATUS_FROM_DEVICE ? 1 :
                (function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ? 2 :
                 (function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ? 3 :
                  (function == URB_FUNCTION_GET_STATUS_FROM_OTHER ? 4 :
                   (function == URB_FUNCTION_GET_CONFIGURATION ? 5 :
                    (function == URB_FUNCTION_GET_INTERFACE ? 6 : 7)))));
            inner->Data.Status.Index = function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ||
                function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT || function == URB_FUNCTION_GET_STATUS_FROM_OTHER
                ? Urb->UrbControlGetStatusRequest.Index :
                (function == URB_FUNCTION_GET_INTERFACE ? Urb->UrbControlGetInterfaceRequest.Interface : 0);
            break;
        case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
        case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
        case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
        case URB_FUNCTION_SET_FEATURE_TO_OTHER:
        case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
        case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
        case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
        case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
            inner->Type = 6;
            inner->Data.Feature.Type = function == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE || function == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE ||
                function == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT || function == URB_FUNCTION_CLEAR_FEATURE_TO_OTHER ? 2 : 1;
            inner->Data.Feature.Subtype = function == URB_FUNCTION_SET_FEATURE_TO_DEVICE || function == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE ? 1 :
                (function == URB_FUNCTION_SET_FEATURE_TO_INTERFACE || function == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE ? 2 :
                 (function == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT || function == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT ? 3 : 4));
            inner->Data.Feature.Index = Urb->UrbControlFeatureRequest.Index;
            inner->Data.Feature.Value = Urb->UrbControlFeatureRequest.FeatureSelector;
            break;
        case URB_FUNCTION_ABORT_PIPE:
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
            inner->Type = 4;
            inner->Data.Reset.Type = function == URB_FUNCTION_ABORT_PIPE ? 4 : 3;
            inner->Data.Reset.EpAddress = queueContext->EndpointAddress;
            break;
        case URB_FUNCTION_SELECT_CONFIGURATION: {
            LONG value = -1;
            if (Urb->UrbSelectConfiguration.ConfigurationDescriptor != NULL) {
                __try { value = Urb->UrbSelectConfiguration.ConfigurationDescriptor->bConfigurationValue; }
                __except (EXCEPTION_EXECUTE_HANDLER) { ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE); return GetExceptionCode(); }
            }
            inner->Type = 1; inner->Data.Descriptor.Type = 2; inner->Data.Descriptor.Subtype = 1; inner->Data.Descriptor.Index = -1; inner->Data.Descriptor.Value = value; inner->DataLength = 0;
            break;
        }
        case URB_FUNCTION_SELECT_INTERFACE:
            inner->Type = 1; inner->Data.Descriptor.Type = 2; inner->Data.Descriptor.Subtype = 2;
            inner->Data.Descriptor.Index = Urb->UrbSelectInterface.Interface.InterfaceNumber;
            inner->Data.Descriptor.Value = Urb->UrbSelectInterface.Interface.AlternateSetting; inner->DataLength = 0;
            break;
        default:
            ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
            return STATUS_NOT_SUPPORTED;
        }
        if (!isRead && transferLength != 0) {
            RtlCopyMemory((PUCHAR)inner + sizeof(*inner) + metadataLength, transaction->TransferBuffer, transferLength);
        }
    }
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        PVHCD_TRANSFER_CONTEXT requestContext;
        NTSTATUS contextStatus;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_TRANSFER_CONTEXT);
        contextStatus = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&requestContext);
        if (!NT_SUCCESS(contextStatus)) { ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE); return contextStatus; }
        requestContext->Transaction = transaction;
    }
    WdfSpinLockAcquire(gSlotLock);
    if (!gSlots[slot].Allocated || !gSlots[slot].Present) {
        WdfSpinLockRelease(gSlotLock); VhcdGetTransferContext(Request)->Transaction = NULL; ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE); return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (gSlots[slot].TransactionCount >= VHCD_MAX_SLOT_TRANSACTIONS ||
        gSlots[slot].TransactionBytes > VHCD_MAX_SLOT_TRANSACTION_BYTES ||
        transaction->AllocationBytes > VHCD_MAX_SLOT_TRANSACTION_BYTES - gSlots[slot].TransactionBytes ||
        gTransactionCount >= VHCD_MAX_CONTROLLER_TRANSACTIONS ||
        gTransactionBytes > VHCD_MAX_CONTROLLER_TRANSACTION_BYTES ||
        transaction->AllocationBytes > VHCD_MAX_CONTROLLER_TRANSACTION_BYTES - gTransactionBytes) {
        WdfSpinLockRelease(gSlotLock); VhcdGetTransferContext(Request)->Transaction = NULL; ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE); return STATUS_INSUFFICIENT_RESOURCES;
    }
    transaction->Id = ++gSlots[slot].NextTransactionId;
    transaction->Sequence = ++gSlots[slot].NextSequence;
    ((ioctl_usbtx_header_t*)transaction->Buffer)->inter_handle = transaction->Id;
    ((ioctl_usbtx_header_t*)transaction->Buffer)->inter_seqno = transaction->Sequence;
    InsertTailList(&gSlots[slot].Transactions, &transaction->Link);
    transaction->Linked = TRUE;
    VhcdTxAddRef(transaction); /* Creator reference spans cancel registration and wakeup. */
    ++gSlots[slot].TransferRefs;
    gSlots[slot].TransactionBytes += transaction->AllocationBytes;
    ++gSlots[slot].TransactionCount;
    gTransactionBytes += transaction->AllocationBytes;
    ++gTransactionCount;
    signalSemaphore = gSlots[slot].Semaphore;
    ObReferenceObject(signalSemaphore);
    WdfSpinLockRelease(gSlotLock);
    Urb->UrbHeader.Status = USBD_STATUS_PENDING;
    {
        NTSTATUS status = WdfRequestMarkCancelableEx(Request, VhcdEvtTransferCanceled);
        if (!NT_SUCCESS(status)) {
            WdfSpinLockAcquire(gSlotLock);
            if (transaction->Linked) { RemoveEntryList(&transaction->Link); transaction->Linked = FALSE; }
            transaction->Ending = TRUE;
            WdfSpinLockRelease(gSlotLock);
            VhcdQueueTransactionCancellation(transaction);
            ObDereferenceObject(signalSemaphore);
            VhcdTxRelease(transaction);
            return STATUS_PENDING;
        }
    }
    {
        BOOLEAN cancelNow;
        WdfSpinLockAcquire(gSlotLock);
        transaction->CancelableReady = TRUE;
        cancelNow = transaction->CancelRequested || !gSlots[slot].Present || !transaction->Linked;
        if (cancelNow) {
            if (transaction->Linked) { RemoveEntryList(&transaction->Link); transaction->Linked = FALSE; }
            transaction->Ending = TRUE;
        } else {
            KeReleaseSemaphore(signalSemaphore, IO_NO_INCREMENT, 1, FALSE);
        }
        WdfSpinLockRelease(gSlotLock);
        if (cancelNow) {
            (VOID)WdfRequestUnmarkCancelable(Request);
            /* The cancel callback may already have returned before readiness was
             * published. Queue here as well; the transaction gate makes this
             * exactly once if the callback also queued it. */
            VhcdQueueTransactionCancellation(transaction);
            ObDereferenceObject(signalSemaphore);
            VhcdTxRelease(transaction);
            return STATUS_PENDING;
        }
    }
    ObDereferenceObject(signalSemaphore);
    VhcdTxRelease(transaction);
    *OutTransaction = transaction;
    return STATUS_SUCCESS;
}

VOID VhcdEvtEndpointIoInternalDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG IoControlCode)
{
    WDF_REQUEST_PARAMETERS parameters;
    PURB urb = NULL;
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    if (IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WDF_REQUEST_PARAMETERS_INIT(&parameters);
        WdfRequestGetParameters(Request, &parameters);
        urb = (PURB)parameters.Parameters.Others.Arg1;
    }
    if (urb == NULL) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return; }
    {
        PVHCD_TRANSACTION transaction = NULL;
        NTSTATUS status = VhcdMapUrb(Queue, Request, urb, &transaction);
        if (NT_SUCCESS(status)) return;
        urb->UrbHeader.Status = status == STATUS_NOT_SUPPORTED ? USBD_STATUS_NOT_SUPPORTED :
            (status == STATUS_DEVICE_NOT_CONNECTED ? USBD_STATUS_DEVICE_GONE :
             (status == STATUS_BUFFER_TOO_SMALL ? USBD_STATUS_BUFFER_TOO_SMALL : USBD_STATUS_INVALID_PARAMETER));
        WdfRequestComplete(Request, STATUS_SUCCESS);
    }
}

static VOID VhcdCancelDpc(_In_ PKDPC Dpc, _In_opt_ PVOID DeferredContext,
                           _In_opt_ PVOID SystemArgument1, _In_opt_ PVOID SystemArgument2)
{
    PVHCD_CANCEL_DPC context = (PVHCD_CANCEL_DPC)DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    if (context->Urb != NULL) context->Urb->UrbHeader.Status = USBD_STATUS_CANCELED;
    WdfRequestComplete(context->Request, STATUS_CANCELLED);
    ExFreePoolWithTag(context, 'cUXV');
}

VOID VhcdEvtIoCanceledOnQueue(WDFQUEUE Queue, WDFREQUEST Request)
{
    PVHCD_CANCEL_DPC context;
    WDF_REQUEST_PARAMETERS parameters;
    PIO_STACK_LOCATION stack;
    UNREFERENCED_PARAMETER(Queue);
    context = (PVHCD_CANCEL_DPC)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*context), 'cUXV');
    if (context == NULL) {
        // Allocation failure is exceptional; WDF completion is still legal at
        // DISPATCH_LEVEL, but no URB fields are touched on this fallback.
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
    RtlZeroMemory(context, sizeof(*context));
    context->Request = Request;
    stack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request));
    if (stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL &&
        stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WDF_REQUEST_PARAMETERS_INIT(&parameters);
        WdfRequestGetParameters(Request, &parameters);
        context->Urb = (PURB)parameters.Parameters.Others.Arg1;
    }
    KeInitializeDpc(&context->Dpc, VhcdCancelDpc, context);
    KeInsertQueueDpc(&context->Dpc, NULL, NULL);
}

VOID VhcdEvtIoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS parameters;
    WDF_OBJECT_ATTRIBUTES attributes;
    PVHCD_USER_IO_CONTEXT context;
    PVOID userBuffer = NULL;
    size_t bufferLength = 0;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Device);
    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(Request, &parameters);
    if (WdfRequestGetRequestorMode(Request) != UserMode) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return; }
    if (parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD) {
        ioctl_pdo_create_t* create;
        PKSEMAPHORE semaphore = NULL;
        PKEVENT removeEvent = NULL;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*create), (PVOID*)&create, &bufferLength);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        if (create->hSemaphore == 0 || create->hSemaphore > MAXULONG_PTR) { WdfRequestComplete(Request, STATUS_INVALID_HANDLE); return; }
        status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)create->hSemaphore, SEMAPHORE_MODIFY_STATE,
            *ExSemaphoreObjectType, UserMode, (PVOID*)&semaphore, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        if (create->hRemoveEvent != 0) {
            if (create->hRemoveEvent > MAXULONG_PTR) status = STATUS_INVALID_HANDLE;
            else status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)create->hRemoveEvent, EVENT_MODIFY_STATE,
                *ExEventObjectType, UserMode, (PVOID*)&removeEvent, NULL);
            if (!NT_SUCCESS(status)) { ObDereferenceObject(semaphore); WdfRequestComplete(Request, status); return; }
        }
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_USER_IO_CONTEXT);
        attributes.EvtCleanupCallback = VhcdUserIoContextCleanup;
        status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&context);
        if (!NT_SUCCESS(status)) {
            if (removeEvent != NULL) ObDereferenceObject(removeEvent);
            ObDereferenceObject(semaphore);
            WdfRequestComplete(Request, status);
            return;
        }
        RtlZeroMemory(context, sizeof(*context));
        context->AddSemaphore = semaphore;
        context->AddRemoveEvent = removeEvent;
        status = WdfRequestForwardToIoQueue(Request, gIoQueue);
        if (!NT_SUCCESS(status)) {
            context->AddSemaphore = NULL;
            context->AddRemoveEvent = NULL;
            if (removeEvent != NULL) ObDereferenceObject(removeEvent);
            ObDereferenceObject(semaphore);
            WdfRequestComplete(Request, status);
        }
        return;
    }
    if (parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_BEGIN_TRANS_USB_DATA &&
        parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_END_TRANS_USB_DATA) {
        status = WdfRequestForwardToIoQueue(Request, gIoQueue);
        if (!NT_SUCCESS(status)) WdfRequestComplete(Request, status);
        return;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_USER_IO_CONTEXT);
    attributes.EvtCleanupCallback = VhcdUserIoContextCleanup;
    status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&context);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    RtlZeroMemory(context, sizeof(*context));
    if (parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_BEGIN_TRANS_USB_DATA) {
        status = WdfRequestRetrieveUnsafeUserOutputBuffer(Request, sizeof(ioctl_usbtx_header_t), &userBuffer, &bufferLength);
        if (NT_SUCCESS(status)) status = WdfRequestProbeAndLockUserBufferForWrite(Request, userBuffer, bufferLength, &context->OutputMemory);
    } else {
        status = WdfRequestRetrieveUnsafeUserInputBuffer(Request, sizeof(ioctl_usbtx_header_t), &userBuffer, &bufferLength);
        if (NT_SUCCESS(status)) status = WdfRequestProbeAndLockUserBufferForRead(Request, userBuffer, bufferLength, &context->InputMemory);
    }
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    status = WdfRequestForwardToIoQueue(Request, gIoQueue);
    if (!NT_SUCCESS(status)) WdfRequestComplete(Request, status);
}

static NTSTATUS VhcdCompleteIsoResponse(_Inout_ PVHCD_TRANSACTION Transaction, _In_ PVHCD_WIRE_REQUEST Inner)
{
    PURB urb = Transaction->Urb;
    ULONG packetCount = urb->UrbIsochronousTransfer.NumberOfPackets;
    ULONG headerBytes = FIELD_OFFSET(iso_packet_hdr_t, packets);
    ULONG responseBytes, dataBytes, i, used = 0;
    iso_packet_hdr_t* response = (iso_packet_hdr_t*)((PUCHAR)Inner + sizeof(*Inner));
    if (Inner->DataLength < 0 || (ULONG)Inner->DataLength < Transaction->MetadataLength ||
        response->number_packets != packetCount || Transaction->MetadataLength != headerBytes + packetCount * sizeof(iso_packet_t))
        return STATUS_INVALID_PARAMETER;
    responseBytes = (ULONG)Inner->DataLength;
    dataBytes = responseBytes - Transaction->MetadataLength;
    if (dataBytes > Transaction->TransferLength) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < packetCount; ++i) {
        ULONG offset = response->packets[i].offset;
        ULONG length = response->packets[i].length;
        USBD_ISO_PACKET_DESCRIPTOR* original = &urb->UrbIsochronousTransfer.IsoPacket[i];
        if (offset != original->Offset || offset > dataBytes || length > dataBytes - offset ||
            offset > Transaction->TransferLength || length > Transaction->TransferLength - offset)
            return STATUS_INVALID_PARAMETER;
        if (offset + length > used) used = offset + length;
        original->Length = length;
        original->Status = response->packets[i].status == 0 ? USBD_STATUS_SUCCESS : (USBD_STATUS)0x80000000;
    }
    if (Transaction->IsRead && used != 0)
        RtlCopyMemory(Transaction->TransferBuffer, (PUCHAR)response + Transaction->MetadataLength, used);
    urb->UrbIsochronousTransfer.StartFrame = response->start_frame;
    urb->UrbIsochronousTransfer.ErrorCount = response->error_count;
    urb->UrbIsochronousTransfer.TransferBufferLength = used;
    return STATUS_SUCCESS;
}

static NTSTATUS VhcdApplyResponse(_Inout_ PVHCD_TRANSACTION Transaction, _In_ PVHCD_WIRE_REQUEST Inner, _In_ ULONG ResponseBytes)
{
    if (Inner->DataLength < 0 || (ULONG)Inner->DataLength > Transaction->PayloadCapacity ||
        ResponseBytes < sizeof(ioctl_usbtx_header_t) + (ULONG)Inner->DataLength)
        return STATUS_INVALID_PARAMETER;
    if (Inner->Result != 0) return STATUS_UNSUCCESSFUL;
    if (Transaction->Urb == NULL)
        return Inner->DataLength == 0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    if (Transaction->Isochronous) return VhcdCompleteIsoResponse(Transaction, Inner);
    if ((ULONG)Inner->DataLength > Transaction->TransferLength) return STATUS_INVALID_PARAMETER;
    if (Transaction->Urb->UrbHeader.Function == URB_FUNCTION_GET_CURRENT_FRAME_NUMBER) {
        if (Inner->DataLength == sizeof(USHORT)) {
            USHORT frame;
            RtlCopyMemory(&frame, (PUCHAR)Inner + sizeof(*Inner), sizeof(frame));
            Transaction->Urb->UrbGetCurrentFrameNumber.FrameNumber = frame;
            return STATUS_SUCCESS;
        }
        if (Inner->DataLength == sizeof(ULONG)) {
            RtlCopyMemory(&Transaction->Urb->UrbGetCurrentFrameNumber.FrameNumber, (PUCHAR)Inner + sizeof(*Inner), sizeof(ULONG));
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_PARAMETER;
    }
    if (Transaction->IsRead && Inner->DataLength != 0)
        RtlCopyMemory(Transaction->TransferBuffer, (PUCHAR)Inner + sizeof(*Inner), (ULONG)Inner->DataLength);
    switch (Transaction->Urb->UrbHeader.Function) {
    case URB_FUNCTION_CONTROL_TRANSFER: Transaction->Urb->UrbControlTransfer.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_CONTROL_TRANSFER_EX: Transaction->Urb->UrbControlTransferEx.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER: Transaction->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE: Transaction->Urb->UrbControlDescriptorRequest.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER: Transaction->Urb->UrbControlVendorClassRequest.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER: Transaction->Urb->UrbControlGetStatusRequest.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_GET_CONFIGURATION: Transaction->Urb->UrbControlGetConfigurationRequest.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_GET_INTERFACE: Transaction->Urb->UrbControlGetInterfaceRequest.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR: Transaction->Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength = (ULONG)Inner->DataLength; break;
    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_SET_FEATURE_TO_OTHER:
    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case URB_FUNCTION_SELECT_CONFIGURATION:
    case URB_FUNCTION_SELECT_INTERFACE:
        if (Inner->DataLength != 0) return STATUS_INVALID_PARAMETER;
        break;
    case URB_FUNCTION_ISOCH_TRANSFER: break;
    default: return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

static VOID VhcdBeginTransaction(WDFREQUEST Request, size_t OutputLength)
{
    PVHCD_FILE_CONTEXT fileContext = VhcdGetFileContext(WdfRequestGetFileObject(Request));
    PVHCD_USER_IO_CONTEXT ioContext = VhcdGetUserIoContext(Request);
    PVHCD_TRANSACTION transaction = NULL;
    size_t bufferSize = 0;
    ULONG copySize;
    PUCHAR output;
    if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE || fileContext->SlotIndex < 0 || ioContext->OutputMemory == NULL) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
    output = (PUCHAR)WdfMemoryGetBuffer(ioContext->OutputMemory, &bufferSize);
    WdfSpinLockAcquire(gSlotLock);
    if (fileContext->SlotIndex < VHCD_MAX_PORTS && gSlots[fileContext->SlotIndex].Allocated) {
        for (PLIST_ENTRY entry = gSlots[fileContext->SlotIndex].Transactions.Flink; entry != &gSlots[fileContext->SlotIndex].Transactions; entry = entry->Flink) {
            PVHCD_TRANSACTION candidate = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
            if (!candidate->Ending && (!candidate->Begun || candidate->NeedsRetry)) {
                candidate->Begun = TRUE;
                candidate->NeedsRetry = FALSE;
                VhcdTxAddRef(candidate);
                transaction = candidate;
                break;
            }
        }
    }
    WdfSpinLockRelease(gSlotLock);
    if (transaction == NULL) { WdfRequestComplete(Request, STATUS_NO_MORE_ENTRIES); return; }
    copySize = sizeof(ioctl_usbtx_header_t) + transaction->PayloadCapacity;
    if (OutputLength < sizeof(ioctl_usbtx_header_t) || bufferSize < sizeof(ioctl_usbtx_header_t)) {
        WdfSpinLockAcquire(gSlotLock);
        if (!transaction->Ending) transaction->NeedsRetry = TRUE;
        WdfSpinLockRelease(gSlotLock);
        VhcdTxRelease(transaction); WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return;
    }
    {
        BOOLEAN truncated = OutputLength < copySize || bufferSize < copySize;
        if (truncated) {
            copySize = sizeof(ioctl_usbtx_header_t);
            WdfSpinLockAcquire(gSlotLock);
            if (!transaction->Ending) transaction->NeedsRetry = TRUE;
            WdfSpinLockRelease(gSlotLock);
        }
        RtlCopyMemory(output, transaction->Buffer, copySize);
        VhcdTxRelease(transaction);
        WdfRequestCompleteWithInformation(Request, truncated ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, copySize);
    }
}

static VOID VhcdEndTransaction(WDFREQUEST Request, size_t InputLength)
{
    PVHCD_FILE_CONTEXT fileContext = VhcdGetFileContext(WdfRequestGetFileObject(Request));
    PVHCD_USER_IO_CONTEXT ioContext = VhcdGetUserIoContext(Request);
    ioctl_usbtx_header_t header;
    PVHCD_WIRE_REQUEST inner;
    PVHCD_TRANSACTION transaction = NULL;
    PUCHAR input;
    size_t bufferSize = 0;
    ULONG responseBytes;
    NTSTATUS status;
    LONG slot;
    if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE || fileContext->SlotIndex < 0 || ioContext->InputMemory == NULL || InputLength < sizeof(header)) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
    input = (PUCHAR)WdfMemoryGetBuffer(ioContext->InputMemory, &bufferSize);
    if (bufferSize < sizeof(header)) { WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return; }
    RtlCopyMemory(&header, input, sizeof(header));
    if (header.data_length < 0 || (ULONG)header.data_length > VHCD_MAX_TRANSACTION_BYTES || (ULONG)header.data_length > InputLength - sizeof(header) || InputLength != sizeof(header) + (ULONG)header.data_length) { WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return; }
    responseBytes = sizeof(header) + (ULONG)header.data_length;
    inner = (PVHCD_WIRE_REQUEST)(input + FIELD_OFFSET(ioctl_usbtx_header_t, type));
    if (header.result != inner->Result || header.data_length != inner->DataLength || inner->DataLength < 0) { WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return; }
    slot = fileContext->SlotIndex;
    WdfSpinLockAcquire(gSlotLock);
    if (slot < VHCD_MAX_PORTS && gSlots[slot].Allocated) {
        for (PLIST_ENTRY entry = gSlots[slot].Transactions.Flink; entry != &gSlots[slot].Transactions; entry = entry->Flink) {
            PVHCD_TRANSACTION candidate = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
            if (candidate->Id == header.inter_handle && candidate->Sequence == header.inter_seqno && candidate->Begun && !candidate->Ending) {
                candidate->Ending = TRUE;
                VhcdTxAddRef(candidate);
                RemoveEntryList(&candidate->Link);
                candidate->Linked = FALSE;
                transaction = candidate;
                break;
            }
        }
    }
    WdfSpinLockRelease(gSlotLock);
    if (transaction == NULL) { WdfRequestComplete(Request, STATUS_NOT_FOUND); return; }
    status = transaction->Request != NULL
        ? WdfRequestUnmarkCancelable(transaction->Request)
        : STATUS_SUCCESS;
    if (status == STATUS_CANCELLED) {
        VhcdQueueTransactionCancellation(transaction);
        VhcdTxRelease(transaction);
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
    if (!NT_SUCCESS(status)) {
        VhcdTxRelease(transaction);
        WdfRequestComplete(Request, status);
        return;
    }
    status = VhcdApplyResponse(transaction, inner, responseBytes);
    if (status == STATUS_UNSUCCESSFUL) {
        VhcdFinishTransfer(transaction, STATUS_UNSUCCESSFUL, USBD_STATUS_STALL_PID);
    } else if (!NT_SUCCESS(status)) {
        VhcdFinishTransfer(transaction, STATUS_INVALID_PARAMETER, USBD_STATUS_INVALID_PARAMETER);
    } else {
        VhcdFinishTransfer(transaction, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
    }
    VhcdTxRelease(transaction);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID VhcdEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG IoControlCode)
{
    PVOID buffer = NULL;
    size_t length = 0;
    NTSTATUS status;
    if (IoControlCode == IOCTL_BEGIN_TRANS_USB_DATA) { VhcdBeginTransaction(Request, OutputLength); return; }
    if (IoControlCode == IOCTL_END_TRANS_USB_DATA) { VhcdEndTransaction(Request, InputLength); return; }
    UNREFERENCED_PARAMETER(Queue);
    if (IoControlCode == IOCTL_PDO_ADD) {
        ioctl_pdo_create_t* create;
        PVHCD_FILE_CONTEXT fileContext;
        PVHCD_USER_IO_CONTEXT ioContext = VhcdGetUserIoContext(Request);
        PKSEMAPHORE semaphore;
        PKEVENT removeEvent;
        LONG address;
        LONG suggested;
        LONG slot = -1;
        LONG i;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*create), (PVOID*)&create, &length);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(address), &buffer, &length);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        fileContext = VhcdGetFileContext(WdfRequestGetFileObject(Request));
        if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE || fileContext->SlotIndex >= 0 || ioContext == NULL || ioContext->AddSemaphore == NULL) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
        semaphore = ioContext->AddSemaphore;
        removeEvent = ioContext->AddRemoveEvent;
        WdfSpinLockAcquire(gSlotLock);
        for (i = 0; i < VHCD_MAX_PORTS; ++i) if (!gSlots[i].Allocated) { slot = i; break; }
        suggested = create->BusAddress;
        if (suggested < 1 || suggested > 127 || gAddressInUse[suggested]) {
            suggested = 1;
            while (suggested <= 127 && gAddressInUse[suggested]) ++suggested;
        }
        if (slot >= 0 && suggested <= 127) {
            gSlots[slot].Allocated = TRUE;
            gSlots[slot].Present = TRUE;
            gSlots[slot].Enabled = FALSE;
            gSlots[slot].ConnectionChanged = TRUE;
            gSlots[slot].BusAddress = suggested;
            gSlots[slot].Semaphore = semaphore;
            gSlots[slot].RemoveEvent = removeEvent;
            gAddressInUse[suggested] = TRUE;
            InitializeListHead(&gSlots[slot].Transactions);
        } else {
            slot = -1;
        }
        WdfSpinLockRelease(gSlotLock);
        if (slot < 0) { if (removeEvent != NULL) ObDereferenceObject(removeEvent); ObDereferenceObject(semaphore); ioContext->AddSemaphore = NULL; ioContext->AddRemoveEvent = NULL; WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES); return; }
        ioContext->AddSemaphore = NULL;
        ioContext->AddRemoveEvent = NULL;
        fileContext->SlotIndex = slot;
        address = (LONG)suggested;
        RtlCopyMemory(buffer, &address, sizeof(address));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(address));
        if (gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
        return;
    }
    if (IoControlCode == IOCTL_PDO_REMOVE) {
        PVHCD_FILE_CONTEXT fileContext = VhcdGetFileContext(WdfRequestGetFileObject(Request));
        if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE || fileContext->SlotIndex < 0) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
        VhcdReleaseSlot(fileContext->SlotIndex, TRUE);
        fileContext->SlotIndex = -1;
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }
    if (IoControlCode == IOCTL_XUSB_QUERY_DRIVER_INFO) {
        xusb_driver_info_t* info;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info), (PVOID*)&info, &length);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        info->Signature = XUSB_DRIVER_INFO_SIGNATURE;
        info->StructureSize = sizeof(*info);
        info->DriverKind = XUSB_DRIVER_KIND_RECONSTRUCTED;
        info->AbiVersion = XUSB_DRIVER_ABI_VERSION;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*info));
        return;
    }
    if (!UcxIoDeviceControl(WdfIoQueueGetDevice(Queue), Request, OutputLength, InputLength, IoControlCode)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
    }
}

NTSTATUS VhcdEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    UCX_CONTROLLER_CONFIG controllerConfig;
    UCX_ROOTHUB_CONFIG rootHubConfig;
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)");
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    status = UcxInitializeDeviceInit(DeviceInit);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, VhcdEvtFileCreate, VhcdEvtFileClose, VhcdEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, VHCD_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, VhcdEvtIoInCallerContext);
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) return status;
    gDevice = device;
    {
        WDF_OBJECT_ATTRIBUTES lockAttributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
        lockAttributes.ParentObject = device;
        status = WdfSpinLockCreate(&lockAttributes, &gSlotLock);
        if (!NT_SUCCESS(status)) return status;
    }
    status = WdfDeviceCreateDeviceInterface(device, &GUID_XUSB_VIRT_INTERFACE, NULL);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = VhcdEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &gIoQueue);
    if (!NT_SUCCESS(status)) return status;
    UCX_CONTROLLER_CONFIG_INIT(&controllerConfig, "Virtual Device Camera UCX HCD");
    controllerConfig.EvtControllerUsbDeviceAdd = VhcdEvtControllerUsbDeviceAdd;
    controllerConfig.EvtControllerQueryUsbCapability = VhcdEvtControllerQueryUsbCapability;
    controllerConfig.EvtControllerGetCurrentFrameNumber = VhcdEvtControllerGetCurrentFrameNumber;
    controllerConfig.EvtControllerReset = VhcdEvtControllerReset;
    status = UcxControllerCreate(device, &controllerConfig, WDF_NO_OBJECT_ATTRIBUTES, &gController);
    if (!NT_SUCCESS(status)) return status;
    UCX_ROOTHUB_CONFIG_INIT_WITH_CONTROL_URB_HANDLER(&rootHubConfig, VhcdEvtRootHubControlUrb, VhcdEvtRootHubInterruptTx, VhcdEvtRootHubGetInfo, VhcdEvtRootHubGet20PortInfo, VhcdEvtRootHubGet30PortInfo);
    return UcxRootHubCreate(gController, &rootHubConfig, WDF_NO_OBJECT_ATTRIBUTES, &gRootHub);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, VhcdEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

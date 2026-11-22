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
#include "../../../../src/protocol/iso_reply.h"

static WDFDEVICE gDevice;
static WDFQUEUE gIoQueue;
static UCXCONTROLLER gController;
static UCXROOTHUB gRootHub;
static WDFDRIVER gDriver;
static KEVENT gTransactionsDrained;
static KEVENT gRemovalWorkItemsDrained;
static volatile LONG gPendingRemovalWorkItems;
static BOOLEAN gStopping;
static xusb_ucx_state_t gUcxState;
#define VHCD_MAX_PORTS 16
#define VHCD_MAX_USB30_PORTS 7
#define VHCD_MAX_SLOTS (VHCD_MAX_PORTS + VHCD_MAX_USB30_PORTS)
#define VHCD_MAX_NONDEFAULT_ENDPOINTS 30u
#define VHCD_USB30_FIRST_SLOT_INDEX VHCD_MAX_PORTS
#define VHCD_USB30_FIRST_PORT_NUMBER (VHCD_MAX_PORTS + 1)
#define VHCD_USB30_PORT_BITMAP_BYTES ((VHCD_MAX_USB30_PORTS + 1 + 7) / 8)
#define VHCD_USB20_PORT_BITMAP_BYTES ((VHCD_MAX_PORTS + 1 + 7) / 8)
#define VHCD_COMBINED_PORT_BITMAP_BYTES ((VHCD_MAX_PORTS + VHCD_MAX_USB30_PORTS + 1 + 7) / 8)
/* Preserve unique global USB 3 port numbers and fit the combined 1..23 bitmap
 * in the existing three-byte root-hub buffer. Local USB 3 requests need one byte. */
C_ASSERT(VHCD_USB30_FIRST_PORT_NUMBER > VHCD_MAX_PORTS);
C_ASSERT(VHCD_USB30_PORT_BITMAP_BYTES < VHCD_USB20_PORT_BITMAP_BYTES);
C_ASSERT(VHCD_COMBINED_PORT_BITMAP_BYTES == VHCD_USB20_PORT_BITMAP_BYTES);
#define VHCD_MAX_TRANSACTION_BYTES (32u * 1024u * 1024u)
#define VHCD_MAX_MS_FEATURE_DESCRIPTOR_BYTES 4096u
#define VHCD_MAX_SLOT_TRANSACTION_BYTES (64u * 1024u * 1024u)
#define VHCD_MAX_SLOT_TRANSACTIONS 256u
#define VHCD_MAX_CONTROLLER_TRANSACTION_BYTES (256u * 1024u * 1024u)
#define VHCD_MAX_CONTROLLER_TRANSACTIONS 1024u
#define VHCD_FILE_SIGNATURE 'FUXV'
#define VHCD_TX_SIGNATURE 'TUXV'

/* Port power belongs to the root hub and must survive virtual-device slot reuse. */
static BOOLEAN gPortPowerOff[VHCD_MAX_PORTS];
static BOOLEAN gUsb30PortPowerOff[VHCD_MAX_USB30_PORTS];

typedef struct _VHCD_SLOT {
    ULONGLONG Generation;
    BOOLEAN Allocated;
    BOOLEAN Present;
    BOOLEAN Enabled;
    BOOLEAN EnableChanged;
    USB_DEVICE_SPEED DeviceSpeed;
    BOOLEAN Suspended;
    BOOLEAN SuspendChanged;
    BOOLEAN ConnectionChanged;
    BOOLEAN ResetChanged;
    BOOLEAN ResetInProgress;
    BOOLEAN BHResetChanged;
    BOOLEAN LinkStateChanged;
    BOOLEAN ConfigErrorChanged;
    BOOLEAN ForceLinkPmAccept;
    BOOLEAN ComplianceModeOnNextAttach;
    UCHAR LinkState;
    UCHAR U1TimeoutValue;
    UCHAR U2TimeoutValue;
    UCHAR RemoteWakeMask;
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

typedef struct _VHCD_CANCELLED_END {
    BOOLEAN Valid;
    ULONG SlotIndex;
    ULONGLONG Generation;
    ULONGLONG Id;
    LONG Sequence;
} VHCD_CANCELLED_END, *PVHCD_CANCELLED_END;

typedef struct _VHCD_FILE_CONTEXT {
    ULONG Signature;
    LONG SlotIndex;
    ULONGLONG Generation;
    BOOLEAN Closing;
} VHCD_FILE_CONTEXT, *PVHCD_FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_FILE_CONTEXT, VhcdGetFileContext)

typedef struct _VHCD_USBDEVICE_CONTEXT {
    ULONG SlotIndex;
    ULONGLONG Generation;
    USB_DEVICE_SPEED DeviceSpeed;
} VHCD_USBDEVICE_CONTEXT, *PVHCD_USBDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_USBDEVICE_CONTEXT, VhcdGetUsbDeviceContext)

typedef struct _VHCD_QUEUE_CONTEXT {
    ULONG SlotIndex;
    ULONGLONG Generation;
    UCHAR EndpointAddress;
    UCHAR PipeType;
    ULONG MaxIsoPacketBytes;
    /* A periodic interrupt request is not visible to user mode before this interval. */
    ULONGLONG InterruptInterval100ns;
    /* Atomic deadline reservation serializes concurrently queued endpoint requests. */
    volatile LONG64 NextInterruptDueTime100ns;
    UCXENDPOINT Endpoint;
} VHCD_QUEUE_CONTEXT, *PVHCD_QUEUE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_QUEUE_CONTEXT, VhcdGetQueueContext)

typedef struct _VHCD_ENDPOINT_CONTEXT {
    ULONG SlotIndex;
    ULONGLONG Generation;
    UCHAR EndpointAddress;
    UCHAR PipeType;
    WDFQUEUE Queue;
} VHCD_ENDPOINT_CONTEXT, *PVHCD_ENDPOINT_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_ENDPOINT_CONTEXT, VhcdGetEndpointContext)

typedef struct _VHCD_USER_IO_CONTEXT {
    WDFMEMORY InputMemory;
    WDFMEMORY OutputMemory;
    PKSEMAPHORE AddSemaphore;
    PKEVENT AddRemoveEvent;
} VHCD_USER_IO_CONTEXT, *PVHCD_USER_IO_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_USER_IO_CONTEXT, VhcdGetUserIoContext)

typedef struct _VHCD_REMOVE_EVENT_WORK_CONTEXT {
    PKSEMAPHORE Semaphore;
    PKEVENT RemoveEvent;
} VHCD_REMOVE_EVENT_WORK_CONTEXT, *PVHCD_REMOVE_EVENT_WORK_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_REMOVE_EVENT_WORK_CONTEXT, VhcdGetRemoveEventWorkContext)

typedef struct _VHCD_TRANSFER_CONTEXT {
    PVOID Transaction;
    KDPC CompletionDpc;
    WDFREQUEST CompletionRequest;
    PURB CompletionUrb;
    NTSTATUS CompletionStatus;
    USBD_STATUS CompletionUsbStatus;
} VHCD_TRANSFER_CONTEXT, *PVHCD_TRANSFER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHCD_TRANSFER_CONTEXT, VhcdGetTransferContext)

typedef struct _VHCD_TRANSACTION {
    LIST_ENTRY Link;
    ULONG Signature;
    LONG SlotIndex;
    ULONGLONG Generation;
    ULONGLONG Id;
    LONG Sequence;
    BOOLEAN Begun;
    BOOLEAN NeedsRetry;
    BOOLEAN Linked;
    /* BEGIN may return this transaction only after its endpoint interval expires. */
    BOOLEAN Ready;
    BOOLEAN Isochronous;
    BOOLEAN IsRead;
    BOOLEAN Ending;
    BOOLEAN IsResetNotification;
    BOOLEAN IsPortReset;
    BOOLEAN IsWarmReset;
    BOOLEAN CancellationRecorded;
    BOOLEAN EndAcknowledged;
    LONG ResetType;
    UCHAR EndpointAddress;
    volatile LONG References;
    volatile LONG OwnerFinished;
    volatile LONG CancelCompletionQueued;
    WDFREQUEST Request;
    PURB Urb;
    PUCHAR TransferBuffer;
    ULONG TransferLength;
    ULONG MaxIsoPacketBytes;
    ULONG MetadataLength;
    ULONG PayloadCapacity;
    ULONG AllocationBytes;
    KDPC CancelDpc;
    /* The ready timer owns a transaction reference until cancellation or DPC exit. */
    KTIMER ReadyTimer;
    KDPC ReadyDpc;
    ULONGLONG ReadyTime100ns;
    volatile LONG ReadyTimerReferenceHeld;
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

static VHCD_SLOT gSlots[VHCD_MAX_SLOTS];
static VHCD_CANCELLED_END gCancelledEnds[VHCD_MAX_CONTROLLER_TRANSACTIONS];
static ULONG gCancelledEndCursor;
/* Never reset during PnP stop/start or slot reuse; zero denotes no binding. */
static ULONGLONG gNextSlotGeneration;
static BOOLEAN gAddressInUse[128];
static WDFSPINLOCK gSlotLock;
static ULONGLONG gTransactionBytes;
static ULONG gTransactionCount;

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
EVT_WDF_OBJECT_CONTEXT_CLEANUP VhcdEvtDeviceCleanup;
EVT_WDF_WORKITEM VhcdRemoveEventSignalWorkItem;
EVT_WDF_DEVICE_PREPARE_HARDWARE VhcdEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE VhcdEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY VhcdEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT VhcdEvtDeviceD0Exit;
EVT_UCX_CONTROLLER_QUERY_USB_CAPABILITY VhcdEvtControllerQueryUsbCapability;
EVT_UCX_CONTROLLER_GET_CURRENT_FRAMENUMBER VhcdEvtControllerGetCurrentFrameNumber;
EVT_UCX_CONTROLLER_RESET VhcdEvtControllerReset;
static VOID VhcdFinalizeSlotIfQuiescent(_In_ LONG SlotIndex, _In_ ULONGLONG Generation);
static VOID VhcdNotifySlotRemoval(_In_opt_ PKEVENT RemoveEvent, _In_opt_ PKSEMAPHORE Semaphore);
static EVT_WDF_OBJECT_CONTEXT_CLEANUP VhcdUsbDeviceCleanup;
static VOID VhcdCancelSlotTransactions(_In_ LONG SlotIndex, _In_ ULONGLONG Generation,
                                      _In_ UCHAR EndpointAddress, _In_ BOOLEAN AllEndpoints,
                                      _In_ BOOLEAN PreserveResetNotifications);
static VOID VhcdCancelEndpointTransfers(_In_ UCXENDPOINT Endpoint);
static VOID VhcdCancelEndpointTransfers(_In_ UCXENDPOINT Endpoint);
static NTSTATUS VhcdQueuePortResetNotification(_In_ LONG SlotIndex, _In_ ULONGLONG Generation, _In_ LONG ResetType,
                                               _In_ BOOLEAN AllowPoweredOffPort,
                                               _In_ BOOLEAN ChangesPortState,
                                               _In_ BOOLEAN IsWarmReset);
/* UCX URB queue requests normally complete on a separate DPC, even on fast failures. */
static KDEFERRED_ROUTINE VhcdTransactionCancelDpc;
static KDEFERRED_ROUTINE VhcdInterruptReadyDpc;
static KDEFERRED_ROUTINE VhcdCompleteRequestDpc;
static EVT_WDF_IO_QUEUE_STATE VhcdEndpointPurgeComplete;
static EVT_WDF_IO_QUEUE_STATE VhcdEndpointAbortComplete;
static VOID VhcdCompleteRequestDeferred(_In_ WDFREQUEST Request, _In_opt_ PURB Urb,
                                        _In_ NTSTATUS RequestStatus, _In_ USBD_STATUS UsbStatus);
static NTSTATUS VhcdEnsureTransferContext(_In_ WDFREQUEST Request,
                                         _Outptr_ PVHCD_TRANSFER_CONTEXT* Context);
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
EVT_UCX_ENDPOINT_STATIC_STREAMS_ADD VhcdEvtEndpointStaticStreamsAdd;
EVT_UCX_ENDPOINT_STATIC_STREAMS_ENABLE VhcdEvtEndpointStaticStreamsEnable;
EVT_UCX_ENDPOINT_STATIC_STREAMS_DISABLE VhcdEvtEndpointStaticStreamsDisable;
EVT_UCX_DEFAULT_ENDPOINT_UPDATE VhcdEvtDefaultEndpointUpdate;

VOID VhcdEvtEndpointPurge(UCXCONTROLLER C, UCXENDPOINT E)
{
    WDFQUEUE queue = VhcdGetEndpointContext(E)->Queue;
    UNREFERENCED_PARAMETER(C);
    VhcdCancelEndpointTransfers(E);
    WdfIoQueuePurge(queue, VhcdEndpointPurgeComplete, (WDFCONTEXT)E);
}
VOID VhcdEvtEndpointStart(UCXCONTROLLER C, UCXENDPOINT E)
{
    UNREFERENCED_PARAMETER(C);
    WdfIoQueueStart(VhcdGetEndpointContext(E)->Queue);
}
VOID VhcdEvtEndpointAbort(UCXCONTROLLER C, UCXENDPOINT E)
{
    WDFQUEUE queue = VhcdGetEndpointContext(E)->Queue;
    UNREFERENCED_PARAMETER(C);
    VhcdCancelEndpointTransfers(E);
    WdfIoQueuePurge(queue, VhcdEndpointAbortComplete, (WDFCONTEXT)E);
}
VOID VhcdEvtEndpointOkToCancelTransfers(UCXENDPOINT E) { UNREFERENCED_PARAMETER(E); }
VOID VhcdEvtEndpointReset(UCXCONTROLLER C, UCXENDPOINT E, WDFREQUEST R) { UNREFERENCED_PARAMETER(C); VhcdCancelEndpointTransfers(E); WdfRequestComplete(R, STATUS_SUCCESS); }
NTSTATUS VhcdEvtEndpointStaticStreamsAdd(UCXENDPOINT E, ULONG NumberOfStreams, PUCXSSTREAMS_INIT Init)
{
    UNREFERENCED_PARAMETER(E);
    UNREFERENCED_PARAMETER(NumberOfStreams);
    UNREFERENCED_PARAMETER(Init);
    return STATUS_NOT_SUPPORTED;
}
VOID VhcdEvtEndpointStaticStreamsEnable(UCXENDPOINT E, UCXSSTREAMS Streams, WDFREQUEST R)
{
    UNREFERENCED_PARAMETER(E);
    UNREFERENCED_PARAMETER(Streams);
    WdfRequestComplete(R, STATUS_NOT_SUPPORTED);
}
VOID VhcdEvtEndpointStaticStreamsDisable(UCXENDPOINT E, UCXSSTREAMS Streams, WDFREQUEST R)
{
    UNREFERENCED_PARAMETER(E);
    UNREFERENCED_PARAMETER(Streams);
    WdfRequestComplete(R, STATUS_NOT_SUPPORTED);
}
VOID VhcdEvtDefaultEndpointUpdate(UCXCONTROLLER C, WDFREQUEST R) { UNREFERENCED_PARAMETER(C); WdfRequestComplete(R, STATUS_SUCCESS); }

/* Caller holds gSlotLock. A port number alone is not a stable device identity. */
static BOOLEAN VhcdSlotMatches(_In_ LONG SlotIndex, _In_ ULONGLONG Generation)
{
    return SlotIndex >= 0 && SlotIndex < VHCD_MAX_SLOTS && Generation != 0 &&
        gSlots[SlotIndex].Allocated && gSlots[SlotIndex].Generation == Generation;
}

/* Caller holds gSlotLock. SuperSpeed root ports have separate power state. */
static BOOLEAN VhcdSlotPowerOff(_In_ LONG SlotIndex)
{
    if (SlotIndex >= VHCD_USB30_FIRST_SLOT_INDEX &&
        SlotIndex < VHCD_USB30_FIRST_SLOT_INDEX + VHCD_MAX_USB30_PORTS)
        return gUsb30PortPowerOff[SlotIndex - VHCD_USB30_FIRST_SLOT_INDEX];
    return SlotIndex >= 0 && SlotIndex < VHCD_MAX_PORTS ? gPortPowerOff[SlotIndex] : TRUE;
}

static LONG VhcdUsb30SlotFromPort(_In_ USHORT Port)
{
    if (Port < VHCD_USB30_FIRST_PORT_NUMBER ||
        Port >= VHCD_USB30_FIRST_PORT_NUMBER + VHCD_MAX_USB30_PORTS) return -1;
    return VHCD_USB30_FIRST_SLOT_INDEX + (LONG)(Port - VHCD_USB30_FIRST_PORT_NUMBER);
}

static BOOLEAN VhcdReleaseSlot(_In_ LONG SlotIndex, _In_ ULONGLONG Generation)
{
    BOOLEAN matched;
    WdfSpinLockAcquire(gSlotLock);
    matched = VhcdSlotMatches(SlotIndex, Generation);
    if (matched) {
        gSlots[SlotIndex].Present = FALSE;
        if (gSlots[SlotIndex].Enabled) gSlots[SlotIndex].EnableChanged = TRUE;
        gSlots[SlotIndex].Enabled = FALSE;
        gSlots[SlotIndex].Suspended = FALSE;
        gSlots[SlotIndex].SuspendChanged = FALSE;
        gSlots[SlotIndex].ResetInProgress = FALSE;
        if (SlotIndex >= VHCD_USB30_FIRST_SLOT_INDEX) {
            gSlots[SlotIndex].LinkState = PORT_LINK_STATE_RX_DETECT;
            gSlots[SlotIndex].LinkStateChanged = TRUE;
        }
        gSlots[SlotIndex].ConnectionChanged = TRUE;
        gSlots[SlotIndex].RemoveRequested = TRUE;
    }
    WdfSpinLockRelease(gSlotLock);
    if (!matched) return FALSE;
    if (gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
    VhcdCancelSlotTransactions(SlotIndex, Generation, 0, TRUE, FALSE);
    VhcdFinalizeSlotIfQuiescent(SlotIndex, Generation);
    return TRUE;
}

static VOID VhcdNotifySlotRemoval(_In_opt_ PKEVENT RemoveEvent, _In_opt_ PKSEMAPHORE Semaphore)
{
    WDF_WORKITEM_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFWORKITEM workItem;
    PVHCD_REMOVE_EVENT_WORK_CONTEXT context;
    NTSTATUS status;

    if (RemoveEvent == NULL && Semaphore == NULL) return;

    WDF_WORKITEM_CONFIG_INIT(&config, VhcdRemoveEventSignalWorkItem);
    config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_REMOVE_EVENT_WORK_CONTEXT);
    attributes.ParentObject = gDevice;
    status = WdfWorkItemCreate(&config, &attributes, &workItem);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "VirtualCameraUcxHcd: unable to queue removal notification (0x%08X)\n", status);
        /* Work-item allocation can fail under memory pressure. Signal the
         * already-referenced event inline so the user-mode unplug waiter is
         * not stranded. Wait=FALSE keeps this legal through DISPATCH_LEVEL. */
        if (RemoveEvent != NULL) {
            KeSetEvent(RemoveEvent, IO_NO_INCREMENT, FALSE);
            ObDereferenceObject(RemoveEvent);
        }
        if (Semaphore != NULL) ObDereferenceObject(Semaphore);
        WdfSpinLockAcquire(gSlotLock);
        if (--gPendingRemovalWorkItems == 0)
            KeSetEvent(&gRemovalWorkItemsDrained, IO_NO_INCREMENT, FALSE);
        WdfSpinLockRelease(gSlotLock);
        return;
    }

    context = VhcdGetRemoveEventWorkContext(workItem);
    context->RemoveEvent = RemoveEvent;
    context->Semaphore = Semaphore;
    WdfWorkItemEnqueue(workItem);
}

VOID VhcdRemoveEventSignalWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PVHCD_REMOVE_EVENT_WORK_CONTEXT context = VhcdGetRemoveEventWorkContext(WorkItem);
    if (context->RemoveEvent != NULL) {
        KeSetEvent(context->RemoveEvent, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(context->RemoveEvent);
        context->RemoveEvent = NULL;
    }
    if (context->Semaphore != NULL) {
        ObDereferenceObject(context->Semaphore);
        context->Semaphore = NULL;
    }
    WdfObjectDelete(WorkItem);
    WdfSpinLockAcquire(gSlotLock);
    if (--gPendingRemovalWorkItems == 0)
        KeSetEvent(&gRemovalWorkItemsDrained, IO_NO_INCREMENT, FALSE);
    WdfSpinLockRelease(gSlotLock);
}

static VOID VhcdFinalizeSlotIfQuiescent(_In_ LONG SlotIndex, _In_ ULONGLONG Generation)
{
    PKSEMAPHORE semaphore = NULL;
    PKEVENT removeEvent = NULL;
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_SLOTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (!gStopping && VhcdSlotMatches(SlotIndex, Generation) && gSlots[SlotIndex].RemoveRequested &&
        !gSlots[SlotIndex].ConnectionChanged && gSlots[SlotIndex].UsbDeviceRefs == 0 &&
        gSlots[SlotIndex].TransferRefs == 0) {
        semaphore = gSlots[SlotIndex].Semaphore;
        removeEvent = gSlots[SlotIndex].RemoveEvent;
        if (gSlots[SlotIndex].BusAddress > 0 && gSlots[SlotIndex].BusAddress <= 127)
            gAddressInUse[gSlots[SlotIndex].BusAddress] = FALSE;
        RtlZeroMemory(&gSlots[SlotIndex], sizeof(gSlots[SlotIndex]));
        if (removeEvent != NULL || semaphore != NULL) {
            /* Pair the count and event under the same lock as completion. A
             * late zero-count signal must not overtake a new work item. */
            if (++gPendingRemovalWorkItems == 1)
                KeClearEvent(&gRemovalWorkItemsDrained);
        }
    }
    WdfSpinLockRelease(gSlotLock);
    VhcdNotifySlotRemoval(removeEvent, semaphore);
}

static VOID VhcdUsbDeviceCleanup(_In_ WDFOBJECT Object)
{
    PVHCD_USBDEVICE_CONTEXT context = VhcdGetUsbDeviceContext((UCXUSBDEVICE)Object);
    LONG slot = (LONG)context->SlotIndex;
    if (slot < 0 || slot >= VHCD_MAX_SLOTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (VhcdSlotMatches(slot, context->Generation) && gSlots[slot].UsbDeviceRefs > 0)
        --gSlots[slot].UsbDeviceRefs;
    WdfSpinLockRelease(gSlotLock);
    VhcdFinalizeSlotIfQuiescent(slot, context->Generation);
}

VOID VhcdEvtFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    PVHCD_FILE_CONTEXT context = VhcdGetFileContext(FileObject);
    UNREFERENCED_PARAMETER(Device);
    context->Signature = VHCD_FILE_SIGNATURE;
    context->SlotIndex = -1;
    context->Generation = 0;
    context->Closing = FALSE;
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID VhcdEvtFileCleanup(WDFFILEOBJECT FileObject)
{
    PVHCD_FILE_CONTEXT context = VhcdGetFileContext(FileObject);
    LONG slot = -1;
    ULONGLONG generation = 0;
    if (context == NULL || context->Signature != VHCD_FILE_SIGNATURE) return;
    WdfSpinLockAcquire(gSlotLock);
    context->Closing = TRUE;
    slot = context->SlotIndex;
    generation = context->Generation;
    context->SlotIndex = -1;
    context->Generation = 0;
    WdfSpinLockRelease(gSlotLock);
    if (slot >= 0) (VOID)VhcdReleaseSlot(slot, generation);
}

VOID VhcdEvtFileClose(WDFFILEOBJECT FileObject)
{
    PVHCD_FILE_CONTEXT context = VhcdGetFileContext(FileObject);
    if (context != NULL) {
        context->Signature = 0;
        context->SlotIndex = -1;
        context->Generation = 0;
    }
}

VOID VhcdUserIoContextCleanup(WDFOBJECT Object)
{
    PVHCD_USER_IO_CONTEXT context = VhcdGetUserIoContext(Object);
    if (context->AddRemoveEvent != NULL) ObDereferenceObject(context->AddRemoveEvent);
    if (context->AddSemaphore != NULL) ObDereferenceObject(context->AddSemaphore);
}

static NTSTATUS VhcdCreateEndpoint(UCXUSBDEVICE UsbDevice, PUCXENDPOINT_INIT EndpointInit, BOOLEAN IsDefault,
                                   UCHAR EndpointAddress, UCHAR PipeType, ULONG MaxIsoPacketBytes,
                                   ULONGLONG InterruptInterval100ns,
                                   UCXENDPOINT* Endpoint)
{
    WDF_IO_QUEUE_CONFIG q;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES endpointAttributes;
    WDFQUEUE queue;
    NTSTATUS status;
    UCX_ENDPOINT_EVENT_CALLBACKS callbacks;
    UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS defaultCallbacks;
    PVHCD_USBDEVICE_CONTEXT deviceContext = VhcdGetUsbDeviceContext(UsbDevice);
    WdfSpinLockAcquire(gSlotLock);
    if (gStopping || !VhcdSlotMatches((LONG)deviceContext->SlotIndex, deviceContext->Generation) ||
        !gSlots[deviceContext->SlotIndex].Present) {
        WdfSpinLockRelease(gSlotLock);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    WdfSpinLockRelease(gSlotLock);
    WDF_IO_QUEUE_CONFIG_INIT(&q, WdfIoQueueDispatchParallel);
    q.EvtIoInternalDeviceControl = VhcdEvtEndpointIoInternalDeviceControl;
    q.EvtIoCanceledOnQueue = VhcdEvtIoCanceledOnQueue;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, VHCD_QUEUE_CONTEXT);
    status = WdfIoQueueCreate(gDevice, &q, &a, &queue);
    if (!NT_SUCCESS(status)) return status;
    VhcdGetQueueContext(queue)->SlotIndex = VhcdGetUsbDeviceContext(UsbDevice)->SlotIndex;
    VhcdGetQueueContext(queue)->Generation = deviceContext->Generation;
    VhcdGetQueueContext(queue)->EndpointAddress = EndpointAddress;
    VhcdGetQueueContext(queue)->PipeType = PipeType;
    VhcdGetQueueContext(queue)->MaxIsoPacketBytes = MaxIsoPacketBytes;
    VhcdGetQueueContext(queue)->InterruptInterval100ns = InterruptInterval100ns;
    VhcdGetQueueContext(queue)->NextInterruptDueTime100ns = 0;
    VhcdGetQueueContext(queue)->Endpoint = NULL;
    if (IsDefault) {
        UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS_INIT(&defaultCallbacks, VhcdEvtEndpointPurge, VhcdEvtEndpointStart, VhcdEvtEndpointAbort, VhcdEvtEndpointOkToCancelTransfers, VhcdEvtDefaultEndpointUpdate);
        UcxDefaultEndpointInitSetEventCallbacks(EndpointInit, &defaultCallbacks);
    } else {
        UCX_ENDPOINT_EVENT_CALLBACKS_INIT(&callbacks, VhcdEvtEndpointPurge, VhcdEvtEndpointStart, VhcdEvtEndpointAbort, VhcdEvtEndpointReset, VhcdEvtEndpointOkToCancelTransfers, VhcdEvtEndpointStaticStreamsAdd, VhcdEvtEndpointStaticStreamsEnable, VhcdEvtEndpointStaticStreamsDisable);
        UcxEndpointInitSetEventCallbacks(EndpointInit, &callbacks);
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&endpointAttributes, VHCD_ENDPOINT_CONTEXT);
    status = UcxEndpointCreate(UsbDevice, &EndpointInit, &endpointAttributes, Endpoint);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(queue); return status; }
    VhcdGetEndpointContext(*Endpoint)->SlotIndex = VhcdGetUsbDeviceContext(UsbDevice)->SlotIndex;
    VhcdGetEndpointContext(*Endpoint)->Generation = deviceContext->Generation;
    VhcdGetEndpointContext(*Endpoint)->EndpointAddress = EndpointAddress;
    VhcdGetEndpointContext(*Endpoint)->PipeType = PipeType;
    VhcdGetEndpointContext(*Endpoint)->Queue = queue;
    VhcdGetQueueContext(queue)->Endpoint = *Endpoint;
    /* UCX owns the endpoint and its queue from here, including their deletion. */
    UcxEndpointSetWdfIoQueue(*Endpoint, queue);
    return STATUS_SUCCESS;
}

static VOID VhcdEndpointPurgeComplete(WDFQUEUE Queue, WDFCONTEXT Context)
{
    UCXENDPOINT endpoint = (UCXENDPOINT)Context;
    UNREFERENCED_PARAMETER(Queue);
    UcxEndpointPurgeComplete(endpoint);
}

static VOID VhcdEndpointAbortComplete(WDFQUEUE Queue, WDFCONTEXT Context)
{
    UCXENDPOINT endpoint = (UCXENDPOINT)Context;
    UNREFERENCED_PARAMETER(Queue);
    WdfIoQueueStart(VhcdGetEndpointContext(endpoint)->Queue);
    UcxEndpointAbortComplete(endpoint);
}

NTSTATUS VhcdEvtUsbDeviceDefaultEndpointAdd(UCXCONTROLLER Controller, UCXUSBDEVICE UsbDevice, ULONG MaxPacketSize, PUCXENDPOINT_INIT EndpointInit)
{
    UCXENDPOINT endpoint;
    PVHCD_USBDEVICE_CONTEXT deviceContext = VhcdGetUsbDeviceContext(UsbDevice);
    UNREFERENCED_PARAMETER(Controller);
    if (deviceContext == NULL) return STATUS_INVALID_DEVICE_STATE;
    switch (deviceContext->DeviceSpeed) {
    case UsbLowSpeed:
        if (MaxPacketSize != 8) return STATUS_INVALID_PARAMETER;
        break;
    case UsbFullSpeed:
        if (MaxPacketSize != 8 && MaxPacketSize != 16 && MaxPacketSize != 32 && MaxPacketSize != 64)
            return STATUS_INVALID_PARAMETER;
        break;
    case UsbHighSpeed:
        if (MaxPacketSize != 64) return STATUS_INVALID_PARAMETER;
        break;
    case UsbSuperSpeed:
        if (MaxPacketSize != 512) return STATUS_INVALID_PARAMETER;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    return VhcdCreateEndpoint(UsbDevice, EndpointInit, TRUE, 0, UsbdPipeTypeControl, 0, 0, &endpoint);
}

static ULONGLONG VhcdInterruptInterval100ns(USB_DEVICE_SPEED Speed, UCHAR Interval)
{
    if (Interval == 0) return 0;
    if (Speed == UsbLowSpeed || Speed == UsbFullSpeed)
        return (ULONGLONG)Interval * 10000ULL;
    if ((Speed == UsbHighSpeed || Speed == UsbSuperSpeed) && Interval <= 16)
        return 1250ULL << (Interval - 1);
    return 0;
}

NTSTATUS VhcdEvtUsbDeviceEndpointAdd(UCXCONTROLLER Controller, UCXUSBDEVICE UsbDevice, PUSB_ENDPOINT_DESCRIPTOR Descriptor, ULONG DescriptorLength, PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR Companion, PUCXENDPOINT_INIT EndpointInit)
{
    UCXENDPOINT endpoint;
    PVHCD_USBDEVICE_CONTEXT deviceContext;
    ULONG maxIsoPacketBytes = 0;
    ULONGLONG interruptInterval100ns = 0;
    ULONG packetSize;
    ULONG encodedTransactions;
    UCHAR pipeType;
    if (Descriptor == NULL || DescriptorLength < sizeof(*Descriptor) ||
        Descriptor->bLength < sizeof(*Descriptor) || Descriptor->bLength > DescriptorLength ||
        Descriptor->bDescriptorType != USB_ENDPOINT_DESCRIPTOR_TYPE)
        return STATUS_INVALID_PARAMETER;
    UNREFERENCED_PARAMETER(Controller);
    pipeType = (UCHAR)(Descriptor->bmAttributes & 0x03);
    deviceContext = VhcdGetUsbDeviceContext(UsbDevice);
    if (deviceContext == NULL) return STATUS_INVALID_DEVICE_STATE;
    packetSize = Descriptor->wMaxPacketSize & 0x07ff;
    encodedTransactions = (Descriptor->wMaxPacketSize >> 11) & 0x03;
    if (deviceContext->DeviceSpeed == UsbSuperSpeed) {
        ULONG maxBurst;
        ULONG mult;
        ULONG theoreticalMaximum;
        if (Companion == NULL || Companion->bLength != sizeof(*Companion) ||
            Companion->bDescriptorType != USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR_TYPE ||
            (Descriptor->wMaxPacketSize & 0xf800) != 0 || packetSize == 0 || packetSize > 1024)
            return STATUS_NOT_SUPPORTED;
        maxBurst = Companion->bMaxBurst;
        if (maxBurst > 15) return STATUS_INVALID_PARAMETER;
        switch (pipeType) {
        case UsbdPipeTypeIsochronous:
            mult = Companion->bmAttributes.Isochronous.Mult;
            theoreticalMaximum = (maxBurst + 1) * (mult + 1) * packetSize;
            if (packetSize != 1024 || mult > USB_SUPERSPEED_ISOCHRONOUS_MAX_MULTIPLIER ||
                Companion->bmAttributes.Isochronous.Reserved2 != 0 ||
                Companion->bmAttributes.Isochronous.SspCompanion != 0 ||
                Companion->wBytesPerInterval == 0 || Companion->wBytesPerInterval > theoreticalMaximum)
                return STATUS_INVALID_PARAMETER;
            maxIsoPacketBytes = Companion->wBytesPerInterval;
            break;
        case UsbdPipeTypeBulk:
            if (packetSize != 1024 || Companion->bmAttributes.Bulk.Reserved1 != 0 ||
                Companion->bmAttributes.Bulk.MaxStreams > 16 || Companion->wBytesPerInterval != 0)
                return STATUS_INVALID_PARAMETER;
            break;
        case UsbdPipeTypeInterrupt:
            theoreticalMaximum = (maxBurst + 1) * packetSize;
            if (Companion->bmAttributes.AsUchar != 0 || Companion->wBytesPerInterval == 0 ||
                Companion->wBytesPerInterval > theoreticalMaximum || Descriptor->bInterval == 0 ||
                Descriptor->bInterval > 16)
                return STATUS_INVALID_PARAMETER;
            interruptInterval100ns = VhcdInterruptInterval100ns(
                deviceContext->DeviceSpeed, Descriptor->bInterval);
            break;
        default:
            return STATUS_NOT_SUPPORTED;
        }
    } else {
        if ((Descriptor->wMaxPacketSize & 0xe000) != 0 || packetSize == 0)
            return STATUS_INVALID_PARAMETER;
        switch (deviceContext->DeviceSpeed) {
        case UsbLowSpeed:
            if (pipeType != UsbdPipeTypeInterrupt || packetSize > 8 || encodedTransactions != 0 ||
                Descriptor->bInterval < 10)
                return STATUS_INVALID_PARAMETER;
            interruptInterval100ns = VhcdInterruptInterval100ns(
                deviceContext->DeviceSpeed, Descriptor->bInterval);
            break;
        case UsbFullSpeed:
            if (encodedTransactions != 0) return STATUS_INVALID_PARAMETER;
            if (pipeType == UsbdPipeTypeBulk) {
                if (packetSize != 8 && packetSize != 16 && packetSize != 32 && packetSize != 64)
                    return STATUS_INVALID_PARAMETER;
            } else if (pipeType == UsbdPipeTypeInterrupt) {
                if (packetSize > 64 || Descriptor->bInterval == 0) return STATUS_INVALID_PARAMETER;
                interruptInterval100ns = VhcdInterruptInterval100ns(
                    deviceContext->DeviceSpeed, Descriptor->bInterval);
            } else if (pipeType == UsbdPipeTypeIsochronous) {
                if (packetSize > 1023 || Descriptor->bInterval == 0 || Descriptor->bInterval > 16)
                    return STATUS_INVALID_PARAMETER;
                maxIsoPacketBytes = packetSize;
            } else {
                return STATUS_NOT_SUPPORTED;
            }
            break;
        case UsbHighSpeed:
            if (pipeType == UsbdPipeTypeBulk) {
                if (packetSize != 512 || encodedTransactions != 0) return STATUS_INVALID_PARAMETER;
            } else if (pipeType == UsbdPipeTypeInterrupt || pipeType == UsbdPipeTypeIsochronous) {
                if (packetSize > 1024 || encodedTransactions > 2 ||
                    Descriptor->bInterval == 0 || Descriptor->bInterval > 16)
                    return STATUS_INVALID_PARAMETER;
                if (pipeType == UsbdPipeTypeIsochronous)
                    maxIsoPacketBytes = packetSize * (encodedTransactions + 1);
                else
                    interruptInterval100ns = VhcdInterruptInterval100ns(
                        deviceContext->DeviceSpeed, Descriptor->bInterval);
            } else {
                return STATUS_NOT_SUPPORTED;
            }
            break;
        default:
            return STATUS_NOT_SUPPORTED;
        }
    }
    return VhcdCreateEndpoint(UsbDevice, EndpointInit, FALSE, Descriptor->bEndpointAddress,
        pipeType, maxIsoPacketBytes, interruptInterval100ns, &endpoint);
}
NTSTATUS VhcdEvtControllerUsbDeviceAdd(UCXCONTROLLER Controller, PUCXUSBDEVICE_INFO DeviceInfo, PUCXUSBDEVICE_INIT UsbDeviceInit)
{
    UCX_USBDEVICE_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    UCXUSBDEVICE usbDevice;
    PVHCD_USBDEVICE_CONTEXT deviceContext;
    ULONG portNumber;
    ULONGLONG generation;
    NTSTATUS status;
    ULONG slotIndex;
    if (DeviceInfo == NULL || UsbDeviceInit == NULL || DeviceInfo->Size < sizeof(*DeviceInfo) ||
        DeviceInfo->PortPath.Size < FIELD_OFFSET(USB_DEVICE_PORT_PATH, PortPath) + sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;
    /* This HCD has a flat set of virtual root ports and no virtual downstream
     * hub topology. Reading only PortPath[0] from a deeper path would alias a
     * child device onto its root-port parent. */
    if (DeviceInfo->PortPath.PortPathDepth != 1 || DeviceInfo->PortPath.TTHubDepth != 0)
        return STATUS_NOT_SUPPORTED;
    portNumber = DeviceInfo->PortPath.PortPath[0];
    if (DeviceInfo->DeviceSpeed == UsbSuperSpeed &&
        portNumber >= VHCD_USB30_FIRST_PORT_NUMBER &&
        portNumber < VHCD_USB30_FIRST_PORT_NUMBER + VHCD_MAX_USB30_PORTS)
        slotIndex = VHCD_USB30_FIRST_SLOT_INDEX + portNumber - VHCD_USB30_FIRST_PORT_NUMBER;
    else if (portNumber > 0 && portNumber <= VHCD_MAX_PORTS &&
             (DeviceInfo->DeviceSpeed == UsbLowSpeed || DeviceInfo->DeviceSpeed == UsbFullSpeed ||
              DeviceInfo->DeviceSpeed == UsbHighSpeed))
        slotIndex = portNumber - 1;
    else
        return STATUS_NOT_SUPPORTED;
    WdfSpinLockAcquire(gSlotLock);
    if (gStopping || !gSlots[slotIndex].Allocated || !gSlots[slotIndex].Present ||
        gSlots[slotIndex].DeviceSpeed != DeviceInfo->DeviceSpeed ||
        VhcdSlotPowerOff((LONG)slotIndex)) {
        WdfSpinLockRelease(gSlotLock);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    generation = gSlots[slotIndex].Generation;
    ++gSlots[slotIndex].UsbDeviceRefs;
    WdfSpinLockRelease(gSlotLock);
    UCX_USBDEVICE_EVENT_CALLBACKS_INIT(&callbacks, VhcdEvtUsbDeviceEndpointsConfigure, VhcdEvtUsbDeviceEnable, VhcdEvtUsbDeviceDisable, VhcdEvtUsbDeviceReset, VhcdEvtUsbDeviceAddress, VhcdEvtUsbDeviceUpdate, VhcdEvtUsbDeviceHubInfo, VhcdEvtUsbDeviceDefaultEndpointAdd, VhcdEvtUsbDeviceEndpointAdd);
    UcxUsbDeviceInitSetEventCallbacks(UsbDeviceInit, &callbacks);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_USBDEVICE_CONTEXT);
    attributes.EvtCleanupCallback = VhcdUsbDeviceCleanup;
    status = UcxUsbDeviceCreate(Controller, &UsbDeviceInit, &attributes, &usbDevice);
    if (NT_SUCCESS(status)) {
        deviceContext = VhcdGetUsbDeviceContext(usbDevice);
        deviceContext->SlotIndex = slotIndex;
        deviceContext->Generation = generation;
        deviceContext->DeviceSpeed = DeviceInfo->DeviceSpeed;
        WdfSpinLockAcquire(gSlotLock);
        if (gStopping || VhcdSlotPowerOff((LONG)slotIndex) || !VhcdSlotMatches((LONG)slotIndex, generation) ||
            !gSlots[slotIndex].Present) {
            WdfSpinLockRelease(gSlotLock);
            WdfObjectDelete(usbDevice);
            return STATUS_DEVICE_DOES_NOT_EXIST;
        }
        WdfSpinLockRelease(gSlotLock);
    } else {
        WdfSpinLockAcquire(gSlotLock);
        if (VhcdSlotMatches((LONG)slotIndex, generation) && gSlots[slotIndex].UsbDeviceRefs > 0)
            --gSlots[slotIndex].UsbDeviceRefs;
        WdfSpinLockRelease(gSlotLock);
        VhcdFinalizeSlotIfQuiescent((LONG)slotIndex, generation);
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

static NTSTATUS VhcdResolveUsbDeviceContext(_In_ UCXUSBDEVICE UsbDevice,
                                             _Outptr_ PVHCD_USBDEVICE_CONTEXT* DeviceContext)
{
    PVHCD_USBDEVICE_CONTEXT context;
    if (DeviceContext == NULL) return STATUS_INVALID_PARAMETER;
    *DeviceContext = NULL;
    if (UsbDevice == WDF_NO_HANDLE) return STATUS_INVALID_PARAMETER;
    context = VhcdGetUsbDeviceContext(UsbDevice);
    if (context == NULL || context->SlotIndex >= VHCD_MAX_SLOTS || context->Generation == 0)
        return STATUS_INVALID_DEVICE_STATE;
    *DeviceContext = context;
    return STATUS_SUCCESS;
}

static NTSTATUS VhcdValidateDefaultEndpoint(_In_ UCXUSBDEVICE UsbDevice,
                                            _In_ UCXENDPOINT Endpoint,
                                            _In_ PVHCD_USBDEVICE_CONTEXT DeviceContext)
{
    PVHCD_ENDPOINT_CONTEXT endpointContext;
    if (Endpoint == WDF_NO_HANDLE || DeviceContext == NULL) return STATUS_INVALID_PARAMETER;
    endpointContext = VhcdGetEndpointContext(Endpoint);
    if (endpointContext == NULL || endpointContext->EndpointAddress != 0 ||
        endpointContext->SlotIndex != DeviceContext->SlotIndex ||
        endpointContext->Generation != DeviceContext->Generation ||
        VhcdGetUsbDeviceContext(UsbDevice) != DeviceContext)
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}

static NTSTATUS VhcdValidateEndpointList(_In_reads_opt_(Count) UCXENDPOINT* Endpoints,
                                         _In_ ULONG Count,
                                         _In_ PVHCD_USBDEVICE_CONTEXT DeviceContext)
{
    ULONG index;
    if (Count > VHCD_MAX_NONDEFAULT_ENDPOINTS || (Count != 0 && Endpoints == NULL))
        return STATUS_INVALID_PARAMETER;
    for (index = 0; index < Count; ++index) {
        PVHCD_ENDPOINT_CONTEXT endpointContext;
        if (Endpoints[index] == WDF_NO_HANDLE) return STATUS_INVALID_PARAMETER;
        endpointContext = VhcdGetEndpointContext(Endpoints[index]);
        if (endpointContext == NULL || endpointContext->EndpointAddress == 0 ||
            endpointContext->SlotIndex != DeviceContext->SlotIndex ||
            endpointContext->Generation != DeviceContext->Generation)
            return STATUS_INVALID_DEVICE_STATE;
    }
    return STATUS_SUCCESS;
}

VOID VhcdEvtUsbDeviceEndpointsConfigure(UCXCONTROLLER C, WDFREQUEST R)
{
    PENDPOINTS_CONFIGURE request = (PENDPOINTS_CONFIGURE)VhcdRequestArgument1(R);
    PVHCD_USBDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) {
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER);
        return;
    }
    status = VhcdResolveUsbDeviceContext(request->UsbDevice, &deviceContext);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(R, status); return; }
    status = VhcdValidateEndpointList(request->EndpointsToEnable,
        request->EndpointsToEnableCount, deviceContext);
    if (NT_SUCCESS(status)) status = VhcdValidateEndpointList(request->EndpointsToDisable,
        request->EndpointsToDisableCount, deviceContext);
    if (NT_SUCCESS(status)) status = VhcdValidateEndpointList(request->EndpointsEnabledAndUnchanged,
        request->EndpointsEnabledAndUnchangedCount, deviceContext);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(R, status); return; }

    /* This software HCD has no endpoint registers to program. It still owns
     * user-mode transactions submitted on each UCX endpoint, so deprogramming
     * an endpoint must cancel those requests before UCX completes the switch. */
    for (ULONG index = 0; index < request->EndpointsToDisableCount; ++index)
        VhcdCancelEndpointTransfers(request->EndpointsToDisable[index]);
    RtlZeroMemory(&request->FailureFlags, sizeof(request->FailureFlags));
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceEnable(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_ENABLE request = (PUSBDEVICE_ENABLE)VhcdRequestArgument1(R);
    PVHCD_USBDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) {
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER);
        return;
    }
    status = VhcdResolveUsbDeviceContext(request->UsbDevice, &deviceContext);
    if (NT_SUCCESS(status))
        status = VhcdValidateDefaultEndpoint(request->UsbDevice,
            request->DefaultEndpoint, deviceContext);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(R, status); return; }
    RtlZeroMemory(&request->FailureFlags, sizeof(request->FailureFlags));
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceDisable(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_DISABLE request = (PUSBDEVICE_DISABLE)VhcdRequestArgument1(R);
    PVHCD_USBDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) {
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER);
        return;
    }
    status = VhcdResolveUsbDeviceContext(request->UsbDevice, &deviceContext);
    if (NT_SUCCESS(status))
        status = VhcdValidateDefaultEndpoint(request->UsbDevice,
            request->DefaultEndpoint, deviceContext);
    if (NT_SUCCESS(status))
        VhcdCancelSlotTransactions((LONG)deviceContext->SlotIndex,
            deviceContext->Generation, 0, TRUE, FALSE);
    WdfRequestComplete(R, status);
}
VOID VhcdEvtUsbDeviceReset(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_RESET request = (PUSBDEVICE_RESET)VhcdRequestArgument1(R);
    PVHCD_USBDEVICE_CONTEXT deviceContext;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) {
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER);
        return;
    }
    status = VhcdResolveUsbDeviceContext(request->UsbDevice, &deviceContext);
    if (NT_SUCCESS(status))
        status = VhcdValidateDefaultEndpoint(request->UsbDevice,
            request->DefaultEndpoint, deviceContext);
    if (NT_SUCCESS(status))
        status = VhcdValidateEndpointList(request->EndpointsToDisable,
            request->EndpointsToDisableCount, deviceContext);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(R, status); return; }

    /* A reset invalidates active endpoint transactions. Retain any reset
     * notification already queued for this port; the shared wire ABI carries
     * that event to user mode so it can discard partial frame state. */
    VhcdCancelSlotTransactions((LONG)deviceContext->SlotIndex,
        deviceContext->Generation, 0, TRUE, TRUE);
    status = VhcdQueuePortResetNotification(
        (LONG)deviceContext->SlotIndex, deviceContext->Generation, 1, FALSE, FALSE, FALSE);
    WdfRequestComplete(R, status);
}
VOID VhcdEvtUsbDeviceAddress(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_ADDRESS request = (PUSBDEVICE_ADDRESS)VhcdRequestArgument1(R);
    PVHCD_USBDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) {
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER);
        return;
    }
    status = VhcdResolveUsbDeviceContext(request->UsbDevice, &deviceContext);
    if (NT_SUCCESS(status) && (request->Address > 127 || request->Reserved != 0))
        status = STATUS_INVALID_PARAMETER;
    WdfRequestComplete(R, status);
}
VOID VhcdEvtUsbDeviceUpdate(UCXCONTROLLER C, WDFREQUEST R)
{
    PUSBDEVICE_UPDATE request = (PUSBDEVICE_UPDATE)VhcdRequestArgument1(R);
    PVHCD_USBDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(C);
    if (request == NULL || request->Size < sizeof(*request)) {
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER);
        return;
    }
    status = VhcdResolveUsbDeviceContext(request->UsbDevice, &deviceContext);
    if (NT_SUCCESS(status) && request->Flags.UpdateIsHub && request->IsHub)
        status = STATUS_NOT_SUPPORTED;
    if (!NT_SUCCESS(status)) { WdfRequestComplete(R, status); return; }
    RtlZeroMemory(&request->FailureFlags, sizeof(request->FailureFlags));
    WdfRequestComplete(R, STATUS_SUCCESS);
}
VOID VhcdEvtUsbDeviceHubInfo(UCXCONTROLLER C, WDFREQUEST R)
{
    /* Downstream virtual hubs are not modeled; claiming success with zeroed
     * hub properties would make UCX treat a nonfunctional hub as supported. */
    UNREFERENCED_PARAMETER(C);
    WdfRequestComplete(R, STATUS_NOT_SUPPORTED);
}

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
    *Status = 0;
    *Change = 0;
    if (i < 0 || i >= VHCD_MAX_PORTS) return;
    WdfSpinLockAcquire(gSlotLock);
    if (!gPortPowerOff[i]) {
        *Status |= USB_PORT_STATUS_POWER;
        if (gSlots[i].Present) {
            *Status |= USB_PORT_STATUS_CONNECT;
            if (gSlots[i].DeviceSpeed == UsbHighSpeed) *Status |= USB_PORT_STATUS_HIGH_SPEED;
            else if (gSlots[i].DeviceSpeed == UsbLowSpeed) *Status |= USB_PORT_STATUS_LOW_SPEED;
        }
        if (gSlots[i].Enabled) *Status |= USB_PORT_STATUS_ENABLE;
        if (gSlots[i].Suspended) *Status |= USB_PORT_STATUS_SUSPEND;
        if (gSlots[i].ResetInProgress) *Status |= USB_PORT_STATUS_RESET;
    }
    if (gSlots[i].ConnectionChanged) *Change |= 0x0001;
    if (gSlots[i].EnableChanged) *Change |= 0x0002;
    if (gSlots[i].ResetChanged) *Change |= 0x0010;
    if (gSlots[i].SuspendChanged) *Change |= 0x0004;
    WdfSpinLockRelease(gSlotLock);
}

static VOID VhcdReadUsb30Port(_In_ USHORT Port, _Out_ PUSHORT Status, _Out_ PUSHORT Change)
{
    USB_30_PORT_STATUS portStatus;
    USB_30_PORT_CHANGE portChange;
    LONG slotIndex = VhcdUsb30SlotFromPort(Port);

    *Status = 0;
    *Change = 0;
    if (slotIndex < 0) return;

    RtlZeroMemory(&portStatus, sizeof(portStatus));
    RtlZeroMemory(&portChange, sizeof(portChange));
    WdfSpinLockAcquire(gSlotLock);
    {
        PVHCD_SLOT slot = &gSlots[slotIndex];
        portChange.ConnectStatusChange = slot->ConnectionChanged;
        portChange.ResetChange = slot->ResetChanged;
        portChange.BHResetChange = slot->BHResetChanged;
        portChange.PortLinkStateChange = slot->LinkStateChanged;
        portChange.PortConfigErrorChange = slot->ConfigErrorChanged;
    }
    if (!gUsb30PortPowerOff[slotIndex - VHCD_USB30_FIRST_SLOT_INDEX]) {
        PVHCD_SLOT slot = &gSlots[slotIndex];
        portStatus.PortPower = 1;
        portStatus.PortLinkState = slot->Present ? slot->LinkState : PORT_LINK_STATE_RX_DETECT;
        portStatus.CurrentConnectStatus = slot->Present &&
            slot->LinkState != PORT_LINK_STATE_DISABLED &&
            slot->LinkState != PORT_LINK_STATE_RX_DETECT;
        portStatus.PortEnabledDisabled = slot->Enabled;
        portStatus.Reset = slot->ResetInProgress;
        /* USB 3.0 Table 10-10 encodes the 5 Gbit/s SuperSpeed link as zero. */
        portStatus.NegotiatedDeviceSpeed = 0;
    } else {
        portStatus.PortLinkState = PORT_LINK_STATE_DISABLED;
    }
    WdfSpinLockRelease(gSlotLock);
    *Status = portStatus.AsUshort16;
    *Change = portChange.AsUshort16;
}

static VOID VhcdRecordInvalidRootHubRequest(
    _In_opt_ PURB Urb,
    _In_ ULONG Setup,
    _In_ ULONG ValueIndex,
    _In_ USHORT SetupLength,
    _In_ ULONG TransferLength,
    _In_ ULONG UsbStatus,
    _In_ BOOLEAN IsUsb30)
{
    WdfSpinLockAcquire(gSlotLock);
    gUcxState.RootHubLastInvalidUrbFunction = Urb != NULL ? Urb->UrbHeader.Function : MAXULONG;
    gUcxState.RootHubLastInvalidSetup = Setup;
    gUcxState.RootHubLastInvalidValueIndex = ValueIndex;
    gUcxState.RootHubLastInvalidSetupLength = SetupLength;
    gUcxState.RootHubLastInvalidTransferLength = TransferLength;
    gUcxState.RootHubLastInvalidUsbStatus = UsbStatus;
    gUcxState.RootHubLastInvalidIsUsb30 = IsUsb30;
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubInvalidRequestCount);
    WdfSpinLockRelease(gSlotLock);
}

static BOOLEAN VhcdIsUsb30ControlRequest(_In_ PURB Urb)
{
    const UCHAR* setup = Urb->UrbControlTransfer.SetupPacket;
    USHORT port = setup[4];
    /* UCX sends both root-hub protocols through the same callback, and the
     * control request itself does not identify which hub owns an aliased
     * port number. Assign SuperSpeed root ports distinct numbers after
     * the USB 2.0 ports. */
    if ((setup[0] & 0x1f) != 3) return FALSE;
    if (VhcdUsb30SlotFromPort(port) >= 0) return TRUE;
    return FALSE;
}

/* CONTROL_TRANSFER_EX replaces the reserved UrbLink with Timeout/Pad. All
 * fields decoded below have the same documented layout in both URB forms. */
C_ASSERT(sizeof(struct _URB_CONTROL_TRANSFER) == sizeof(struct _URB_CONTROL_TRANSFER_EX));
C_ASSERT(FIELD_OFFSET(URB, UrbControlTransfer.PipeHandle) == FIELD_OFFSET(URB, UrbControlTransferEx.PipeHandle));
C_ASSERT(FIELD_OFFSET(URB, UrbControlTransfer.TransferBufferLength) == FIELD_OFFSET(URB, UrbControlTransferEx.TransferBufferLength));
C_ASSERT(FIELD_OFFSET(URB, UrbControlTransfer.TransferBuffer) == FIELD_OFFSET(URB, UrbControlTransferEx.TransferBuffer));
C_ASSERT(FIELD_OFFSET(URB, UrbControlTransfer.TransferBufferMDL) == FIELD_OFFSET(URB, UrbControlTransferEx.TransferBufferMDL));
C_ASSERT(FIELD_OFFSET(URB, UrbControlTransfer.SetupPacket) == FIELD_OFFSET(URB, UrbControlTransferEx.SetupPacket));
C_ASSERT(sizeof(USB_30_HUB_DESCRIPTOR) == 12);

VOID VhcdEvtRootHubControlUrb(UCXROOTHUB H, WDFREQUEST R)
{
    PURB urb = VhcdRequestUrb(R);
    UCHAR* setup;
    PUCHAR data;
    ULONG length;
    USHORT value, index, rawIndex, statusWord, changeWord, setupLength;
    BOOLEAN isUsb30Control;
    UNREFERENCED_PARAMETER(H);
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubControlCount);
    gUcxState.RootHubLastUrbFunction = urb != NULL ? urb->UrbHeader.Function : MAXULONG;
    if (urb == NULL || (urb->UrbHeader.Function != URB_FUNCTION_CONTROL_TRANSFER &&
        urb->UrbHeader.Function != URB_FUNCTION_CONTROL_TRANSFER_EX)) {
        if (urb != NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
        gUcxState.RootHubLastUsbStatus = (ULONG)USBD_STATUS_INVALID_URB_FUNCTION;
        VhcdRecordInvalidRootHubRequest(urb, 0, 0, 0, 0,
            (ULONG)USBD_STATUS_INVALID_URB_FUNCTION, FALSE);
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    if (urb->UrbHeader.Length < sizeof(urb->UrbControlTransfer)) {
        urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        gUcxState.RootHubLastUsbStatus = (ULONG)USBD_STATUS_INVALID_PARAMETER;
        VhcdRecordInvalidRootHubRequest(urb, 0, 0, 0, 0,
            (ULONG)USBD_STATUS_INVALID_PARAMETER, FALSE);
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    setup = urb->UrbControlTransfer.SetupPacket;
    value = (USHORT)(setup[2] | ((USHORT)setup[3] << 8));
    index = (USHORT)(setup[4] | ((USHORT)setup[5] << 8));
    setupLength = (USHORT)(setup[6] | ((USHORT)setup[7] << 8));
    rawIndex = index;
    gUcxState.RootHubLastSetup = setup[0] | ((ULONG)setup[1] << 8);
    gUcxState.RootHubLastValueIndex = value | ((ULONG)index << 16);
    if ((setup[0] & 0x1f) == 3) index &= 0xff;
    isUsb30Control = VhcdIsUsb30ControlRequest(urb);
    if (setup[1] == USB_REQUEST_GET_DESCRIPTOR && (value >> 8) == USB_20_HUB_DESCRIPTOR_TYPE)
        isUsb30Control = FALSE;
    else if (setup[1] == USB_REQUEST_GET_DESCRIPTOR && (value >> 8) == USB_30_HUB_DESCRIPTOR_TYPE) {
        isUsb30Control = TRUE;
    }
    if (isUsb30Control)
        InterlockedIncrement((volatile LONG*)&gUcxState.RootHubControl30Count);
    else
        InterlockedIncrement((volatile LONG*)&gUcxState.RootHubControl20Count);
    length = urb->UrbControlTransfer.TransferBufferLength;
    data = VhcdTransferBuffer(length, urb->UrbControlTransfer.TransferBuffer, urb->UrbControlTransfer.TransferBufferMDL);
    urb->UrbHeader.Status = USBD_STATUS_SUCCESS;

    if (setup[0] == 0xa0 && setup[1] == USB_REQUEST_GET_DESCRIPTOR &&
        (value >> 8) == USB_20_HUB_DESCRIPTOR_TYPE && (value & 0xff) == 0 && index == 0) {
        UCHAR hub[13] = { 13, USB_20_HUB_DESCRIPTOR_TYPE, VHCD_MAX_PORTS, 0x19, 0x00, 0x01, 0x00,
                          0x00, 0x00, 0x00, 0xfe, 0xff, 0x01 };
        ULONG copyLength;
        /* 16 ports need three bytes per bitmap (hub bit plus 16 port bits). */
        copyLength = min(length, min((ULONG)setupLength, (ULONG)sizeof(hub)));
        if (copyLength != 0 && data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        else if (copyLength != 0) RtlCopyMemory(data, hub, copyLength);
        urb->UrbControlTransfer.TransferBufferLength = copyLength;
    } else if (setup[0] == 0xa0 && setup[1] == USB_REQUEST_GET_DESCRIPTOR &&
        (value >> 8) == USB_30_HUB_DESCRIPTOR_TYPE && (value & 0xff) == 0 && index == 0) {
        USB_30_HUB_DESCRIPTOR hub;
        ULONG copyLength;
        RtlZeroMemory(&hub, sizeof(hub));
        hub.bLength = sizeof(hub);
        hub.bDescriptorType = USB_30_HUB_DESCRIPTOR_TYPE;
        hub.bNumberOfPorts = VHCD_MAX_USB30_PORTS;
        /* Individual port power; no over-current source is modeled. */
        hub.wHubCharacteristics = 0x0011;
        hub.bPowerOnToPowerGood = 1;
        copyLength = min(length, min((ULONG)setupLength, (ULONG)sizeof(hub)));
        if (copyLength != 0 && data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        else if (copyLength != 0) RtlCopyMemory(data, &hub, copyLength);
        urb->UrbControlTransfer.TransferBufferLength = copyLength;
    } else if (setup[1] == USB_REQUEST_GET_STATUS &&
               (setup[0] == 0xa0 || setup[0] == 0xa3)) {
        if (setup[0] == 0xa0) {
            ULONG hubStatus = 0;
            if (value != 0 || rawIndex != 0 || setupLength != sizeof(hubStatus) ||
                length < sizeof(hubStatus) || data == NULL)
                urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
            else { RtlCopyMemory(data, &hubStatus, sizeof(hubStatus)); urb->UrbControlTransfer.TransferBufferLength = sizeof(hubStatus); }
        } else if (VhcdIsUsb30ControlRequest(urb) &&
                   VhcdUsb30SlotFromPort(index) >= 0) {
            if (value == USB_STATUS_EXT_PORT_STATUS) {
                ULONG extendedStatus[2] = { 0, 0 };
                if (rawIndex != index || setupLength != sizeof(extendedStatus) ||
                    length < sizeof(extendedStatus) || data == NULL) {
                    urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
                } else {
                    VhcdReadUsb30Port(index, &statusWord, &changeWord);
                    extendedStatus[0] = statusWord | ((ULONG)changeWord << 16);
                    /* This software root port models baseline 5-Gbit/s USB 3.x.
                     * It has no SuperSpeedPlus sublink details to report. */
                    RtlCopyMemory(data, extendedStatus, sizeof(extendedStatus));
                    urb->UrbControlTransfer.TransferBufferLength = sizeof(extendedStatus);
                }
            } else if (value == USB_STATUS_PORT_STATUS && rawIndex == index &&
                       setupLength == sizeof(ULONG) && length >= sizeof(ULONG) && data != NULL) {
                ULONG portStatus;
                VhcdReadUsb30Port(index, &statusWord, &changeWord);
                portStatus = statusWord | ((ULONG)changeWord << 16);
                RtlCopyMemory(data, &portStatus, sizeof(portStatus));
                urb->UrbControlTransfer.TransferBufferLength = sizeof(portStatus);
            } else {
                urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
            }
        } else if (index != 0 && index <= VHCD_MAX_PORTS) {
            if (value != 0 || rawIndex != index || setupLength != sizeof(ULONG) ||
                length < sizeof(ULONG) || data == NULL) {
                urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
            } else {
                ULONG portStatus;
                VhcdReadPort(index, &statusWord, &changeWord);
                portStatus = statusWord | ((ULONG)changeWord << 16);
                RtlCopyMemory(data, &portStatus, sizeof(portStatus));
                urb->UrbControlTransfer.TransferBufferLength = sizeof(portStatus);
            }
        } else {
            urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        }
    } else if (setup[1] == USB_REQUEST_GET_PORT_ERR_COUNT && setup[0] == 0xa3 &&
               VhcdIsUsb30ControlRequest(urb) && VhcdUsb30SlotFromPort(index) >= 0) {
        if (value != 0 || rawIndex != index || setupLength != sizeof(USHORT) ||
            length < sizeof(USHORT) || data == NULL) {
            urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        } else {
            USHORT errorCount = 0;
            RtlCopyMemory(data, &errorCount, sizeof(errorCount));
            urb->UrbControlTransfer.TransferBufferLength = sizeof(errorCount);
        }
    } else if ((setup[1] == USB_REQUEST_SET_FEATURE || setup[1] == USB_REQUEST_CLEAR_FEATURE) &&
               setup[0] == 0x20 && index == 0 && rawIndex == 0 &&
               setupLength == 0 && length == 0 && value <= 1) {
        /* The virtual root hub has no hub-wide power or over-current state. */
        urb->UrbControlTransfer.TransferBufferLength = 0;
    } else if (setup[1] == USB_REQUEST_SET_HUB_DEPTH && setup[0] == 0x20 &&
               rawIndex == 0 && setupLength == 0 && length == 0 && value == 0) {
        /* The virtual SuperSpeed root hub is at depth zero. */
        urb->UrbControlTransfer.TransferBufferLength = 0;
    } else if ((setup[1] == USB_REQUEST_SET_FEATURE || setup[1] == USB_REQUEST_CLEAR_FEATURE) &&
               setup[0] == 0x23 && setupLength == 0 && length == 0 &&
               VhcdIsUsb30ControlRequest(urb) &&
               VhcdUsb30SlotFromPort(index) >= 0) {
        LONG slot = VhcdUsb30SlotFromPort(index);
        ULONG portIndex = (ULONG)(slot - VHCD_USB30_FIRST_SLOT_INDEX);
        BOOLEAN supported = TRUE;
        BOOLEAN changed = FALSE;
        BOOLEAN resetRequested = FALSE;
        BOOLEAN cancelPortTransactions = FALSE;
        BOOLEAN reattachRequested = FALSE;
        ULONGLONG generation;
        WdfSpinLockAcquire(gSlotLock);
        generation = gSlots[slot].Generation;
        if (value == 8 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) {
                supported = FALSE;
            } else if (!gUsb30PortPowerOff[portIndex]) {
                gUsb30PortPowerOff[portIndex] = TRUE;
                if (gSlots[slot].LinkState != PORT_LINK_STATE_DISABLED)
                    gSlots[slot].LinkStateChanged = TRUE;
                gSlots[slot].LinkState = PORT_LINK_STATE_DISABLED;
                gSlots[slot].Enabled = FALSE;
                gSlots[slot].Suspended = FALSE;
                gSlots[slot].ResetInProgress = FALSE;
                gSlots[slot].U1TimeoutValue = 0;
                gSlots[slot].U2TimeoutValue = 0;
                gSlots[slot].RemoteWakeMask = 0;
                gSlots[slot].ForceLinkPmAccept = FALSE;
                cancelPortTransactions = TRUE;
                changed = TRUE;
                if (gSlots[slot].Allocated && gSlots[slot].Present) {
                    gSlots[slot].ConnectionChanged = TRUE;
                    changed = TRUE;
                    resetRequested = TRUE;
                }
            }
        } else if (value == 8 && setup[1] == USB_REQUEST_SET_FEATURE) {
            if (rawIndex != index) {
                supported = FALSE;
            } else if (gUsb30PortPowerOff[portIndex]) {
                gUsb30PortPowerOff[portIndex] = FALSE;
                gSlots[slot].LinkState = PORT_LINK_STATE_U0;
                gSlots[slot].LinkStateChanged = TRUE;
                gSlots[slot].U1TimeoutValue = 0;
                gSlots[slot].U2TimeoutValue = 0;
                gSlots[slot].RemoteWakeMask = 0;
                gSlots[slot].ForceLinkPmAccept = FALSE;
                changed = TRUE;
                if (gSlots[slot].Allocated && gSlots[slot].Present) {
                    gSlots[slot].ConnectionChanged = TRUE;
                    changed = TRUE;
                }
            }
        } else if (value == 5 && setup[1] == USB_REQUEST_SET_FEATURE) {
            UCHAR requestedLinkState = (UCHAR)(rawIndex >> 8);
            PVHCD_SLOT port = &gSlots[slot];
            BOOLEAN currentStateIsU = port->LinkState <= PORT_LINK_STATE_U3;
            if (requestedLinkState == PORT_LINK_STATE_U3 && !port->Enabled) {
                /* USB 3.2 §10.16.2.10 requires U3 on a non-enabled port to
                 * complete as a functional no-op. */
            } else if (gUsb30PortPowerOff[portIndex] || (requestedLinkState <= PORT_LINK_STATE_U3 &&
                (!port->Enabled || !currentStateIsU ||
                 ((requestedLinkState == PORT_LINK_STATE_U1 || requestedLinkState == PORT_LINK_STATE_U2) &&
                  port->LinkState != PORT_LINK_STATE_U0)))) {
                supported = FALSE;
            } else if (requestedLinkState <= PORT_LINK_STATE_U3) {
                if (port->LinkState != requestedLinkState) {
                    port->LinkState = requestedLinkState;
                    port->Suspended = requestedLinkState == PORT_LINK_STATE_U3;
                    port->LinkStateChanged = TRUE;
                    changed = TRUE;
                }
            } else if (requestedLinkState == PORT_LINK_STATE_DISABLED) {
                if (port->LinkState != PORT_LINK_STATE_DISABLED || port->Enabled) {
                    port->LinkState = PORT_LINK_STATE_DISABLED;
                    port->Enabled = FALSE;
                    port->Suspended = FALSE;
                    port->ResetInProgress = FALSE;
                    port->ConnectionChanged = TRUE;
                    port->LinkStateChanged = TRUE;
                    changed = TRUE;
                }
            } else if (requestedLinkState == PORT_LINK_STATE_RX_DETECT) {
                if (port->LinkState != PORT_LINK_STATE_DISABLED || port->Enabled) {
                    supported = FALSE;
                } else {
                    port->LinkState = PORT_LINK_STATE_RX_DETECT;
                    port->LinkStateChanged = TRUE;
                    port->ConnectionChanged = TRUE;
                    reattachRequested = port->Present;
                    changed = TRUE;
                }
            } else if (requestedLinkState == PORT_LINK_STATE_COMPLIANCE_MODE) {
                /* The software HCD has no PHY compliance transmitter. */
                supported = FALSE;
            } else {
                supported = FALSE;
            }
        } else if ((value == 4 || value == 28) && setup[1] == USB_REQUEST_SET_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else resetRequested = TRUE;
        } else if ((value == 23 || value == 24 || value == 27) &&
                   setup[1] == USB_REQUEST_SET_FEATURE) {
            UCHAR parameter = (UCHAR)(rawIndex >> 8);
            if (gUsb30PortPowerOff[portIndex] ||
                (value == 23 && parameter > 0x7f && parameter != 0xff) ||
                (value == 27 && (parameter & 0xf8) != 0)) {
                supported = FALSE;
            } else if (value == 23) {
                gSlots[slot].U1TimeoutValue = parameter;
            } else if (value == 24) {
                gSlots[slot].U2TimeoutValue = parameter;
            } else {
                gSlots[slot].RemoteWakeMask = parameter;
            }
        } else if (value == 30 && setup[1] == USB_REQUEST_SET_FEATURE) {
            if (rawIndex != index || gUsb30PortPowerOff[portIndex]) supported = FALSE;
            else if (gSlots[slot].Enabled) gSlots[slot].ForceLinkPmAccept = TRUE;
        } else if (value == 16 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else gSlots[slot].ConnectionChanged = FALSE;
        } else if (value == 20 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else gSlots[slot].ResetChanged = FALSE;
        } else if (value == 29 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else gSlots[slot].BHResetChanged = FALSE;
        } else if (value == 25 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else gSlots[slot].LinkStateChanged = FALSE;
        } else if (value == 26 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else gSlots[slot].ConfigErrorChanged = FALSE;
        } else if (value == 30 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (rawIndex != index) supported = FALSE;
            else gSlots[slot].ForceLinkPmAccept = FALSE;
        } else if (value == 17 || value == 18 || value == 19) {
            supported = FALSE;
        } else {
            supported = FALSE;
        }
        WdfSpinLockRelease(gSlotLock);
        if (cancelPortTransactions)
            VhcdCancelSlotTransactions(slot, generation, 0, TRUE, TRUE);
        if (reattachRequested) {
            NTSTATUS reattachStatus = VhcdQueuePortResetNotification(
                slot, generation, 2, FALSE, FALSE, FALSE);
            if (!NT_SUCCESS(reattachStatus)) {
                supported = FALSE;
                urb->UrbHeader.Status = reattachStatus == STATUS_DEVICE_NOT_CONNECTED
                    ? USBD_STATUS_DEVICE_GONE
                    : USBD_STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        if (resetRequested && value != 8) {
            NTSTATUS resetStatus = VhcdQueuePortResetNotification(
                slot, generation, 1, FALSE, TRUE, value == 28);
            if (!NT_SUCCESS(resetStatus)) {
                supported = FALSE;
                urb->UrbHeader.Status = resetStatus == STATUS_DEVICE_NOT_CONNECTED
                    ? USBD_STATUS_DEVICE_GONE
                    : USBD_STATUS_INSUFFICIENT_RESOURCES;
            } else changed = TRUE;
        }
        if (resetRequested && value == 8) {
            NTSTATUS resetStatus = VhcdQueuePortResetNotification(slot, generation, 1, TRUE, FALSE, FALSE);
            if (!NT_SUCCESS(resetStatus)) {
                supported = FALSE;
                urb->UrbHeader.Status = resetStatus == STATUS_DEVICE_NOT_CONNECTED
                    ? USBD_STATUS_DEVICE_GONE
                    : USBD_STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        if (value == 16 && setup[1] == USB_REQUEST_CLEAR_FEATURE)
            VhcdFinalizeSlotIfQuiescent(slot, generation);
        if (changed && gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
        urb->UrbControlTransfer.TransferBufferLength = 0;
        if (!supported && urb->UrbHeader.Status == USBD_STATUS_SUCCESS)
            urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
    } else if ((setup[1] == USB_REQUEST_SET_FEATURE || setup[1] == USB_REQUEST_CLEAR_FEATURE) &&
               setup[0] == 0x23 && index > 0 && index <= VHCD_MAX_PORTS &&
               setupLength == 0 && length == 0 && rawIndex == index) {
        LONG i = (LONG)index - 1;
        BOOLEAN changed = FALSE;
        BOOLEAN supported = TRUE;
        BOOLEAN resetRequested = FALSE;
        ULONGLONG generation;
        WdfSpinLockAcquire(gSlotLock);
        generation = gSlots[i].Generation;
        if (value == 8 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (!gPortPowerOff[i]) {
                gPortPowerOff[i] = TRUE;
                if (gSlots[i].Enabled) gSlots[i].EnableChanged = TRUE;
                gSlots[i].Enabled = FALSE;
                gSlots[i].Suspended = FALSE;
                gSlots[i].SuspendChanged = FALSE;
                gSlots[i].ResetChanged = FALSE;
                gSlots[i].ResetInProgress = FALSE;
                if (gSlots[i].Allocated && gSlots[i].Present) {
                    gSlots[i].ConnectionChanged = TRUE;
                    changed = TRUE;
                    resetRequested = TRUE; /* Cancel requests for a port that just lost power. */
                }
            }
        }
        else if (value == 8 && setup[1] == USB_REQUEST_SET_FEATURE) {
            if (gPortPowerOff[i]) {
                gPortPowerOff[i] = FALSE;
                if (gSlots[i].Allocated && gSlots[i].Present) {
                    gSlots[i].ConnectionChanged = TRUE;
                    changed = TRUE;
                }
            }
        }
        else if (value == 1 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (gSlots[i].Enabled) {
                gSlots[i].Enabled = FALSE;
                gSlots[i].EnableChanged = TRUE;
                changed = TRUE;
            }
            gSlots[i].Suspended = FALSE;
        }
        else if (value == 2 && setup[1] == USB_REQUEST_SET_FEATURE) {
            if (gSlots[i].Present && gSlots[i].Enabled) gSlots[i].Suspended = TRUE;
        }
        else if (value == 2 && setup[1] == USB_REQUEST_CLEAR_FEATURE) {
            if (gSlots[i].Suspended) {
                gSlots[i].Suspended = FALSE;
                gSlots[i].SuspendChanged = TRUE;
                changed = TRUE;
            }
        }
        else if (value == 4 && setup[1] == USB_REQUEST_SET_FEATURE) { resetRequested = TRUE; }
        else if (value == 16 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].ConnectionChanged = FALSE; }
        else if (value == 17 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].EnableChanged = FALSE; }
        else if (value == 18 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].SuspendChanged = FALSE; }
        else if (value == 19 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { /* No over-current source. */ }
        else if (value == 20 && setup[1] == USB_REQUEST_CLEAR_FEATURE) { gSlots[i].ResetChanged = FALSE; }
        else supported = FALSE;
        WdfSpinLockRelease(gSlotLock);
        if (resetRequested && value == 4) {
            NTSTATUS resetStatus = VhcdQueuePortResetNotification(i, generation, 1, FALSE, TRUE, FALSE);
            if (!NT_SUCCESS(resetStatus)) {
                supported = FALSE;
                urb->UrbHeader.Status = resetStatus == STATUS_DEVICE_NOT_CONNECTED
                    ? USBD_STATUS_DEVICE_GONE
                    : USBD_STATUS_INSUFFICIENT_RESOURCES;
            } else changed = TRUE;
        }
        if (value == 16 && setup[1] == USB_REQUEST_CLEAR_FEATURE) VhcdFinalizeSlotIfQuiescent(i, generation);
        if (changed && gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
        urb->UrbControlTransfer.TransferBufferLength = 0;
        if (!supported && urb->UrbHeader.Status == USBD_STATUS_SUCCESS)
            urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        if (resetRequested && value == 8) {
            NTSTATUS notificationStatus;
            VhcdCancelSlotTransactions(i, generation, 0, TRUE, TRUE);
            notificationStatus = VhcdQueuePortResetNotification(i, generation, 1, TRUE, FALSE, FALSE);
            if (!NT_SUCCESS(notificationStatus))
                urb->UrbHeader.Status = notificationStatus == STATUS_DEVICE_NOT_CONNECTED
                    ? USBD_STATUS_DEVICE_GONE
                    : USBD_STATUS_INSUFFICIENT_RESOURCES;
        }
    } else {
        urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
    }
    if (urb->UrbHeader.Status != USBD_STATUS_SUCCESS)
        urb->UrbControlTransfer.TransferBufferLength = 0;
    gUcxState.RootHubLastUsbStatus = urb->UrbHeader.Status;
    if (urb->UrbHeader.Status != USBD_STATUS_SUCCESS) {
        VhcdRecordInvalidRootHubRequest(urb,
            setup[0] | ((ULONG)setup[1] << 8),
            value | ((ULONG)rawIndex << 16),
            setupLength, length, urb->UrbHeader.Status, isUsb30Control);
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
    LONG ssPort;
    UNREFERENCED_PARAMETER(H);
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubInterruptCount);
    if (urb == NULL || urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
        if (urb != NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    if (urb->UrbHeader.Length < sizeof(urb->UrbBulkOrInterruptTransfer)) {
        urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        gUcxState.RootHubLastUsbStatus = (ULONG)USBD_STATUS_INVALID_PARAMETER;
        VhcdRecordInvalidRootHubRequest(urb, 0, 0, 0, 0,
            (ULONG)USBD_STATUS_INVALID_PARAMETER, FALSE);
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    length = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    data = VhcdTransferBuffer(length, urb->UrbBulkOrInterruptTransfer.TransferBuffer, urb->UrbBulkOrInterruptTransfer.TransferBufferMDL);
    bytes = VHCD_USB20_PORT_BITMAP_BYTES;
    if (length < bytes) {
        ULONG superSpeedBitmap = 0;
        InterlockedIncrement((volatile LONG*)&gUcxState.RootHubInterrupt30Count);
        if (length < VHCD_USB30_PORT_BITMAP_BYTES || data == NULL)
            urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        else {
            WdfSpinLockAcquire(gSlotLock);
            for (ssPort = 0; ssPort < VHCD_MAX_USB30_PORTS; ++ssPort) {
                PVHCD_SLOT slot = &gSlots[VHCD_USB30_FIRST_SLOT_INDEX + ssPort];
                if (slot->ConnectionChanged || slot->ResetChanged ||
                    slot->BHResetChanged || slot->LinkStateChanged || slot->ConfigErrorChanged)
                    superSpeedBitmap |= 1u << (ssPort + 1);
            }
            WdfSpinLockRelease(gSlotLock);
            RtlZeroMemory(data, length);
            RtlCopyMemory(data, &superSpeedBitmap, VHCD_USB30_PORT_BITMAP_BYTES);
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            urb->UrbBulkOrInterruptTransfer.TransferBufferLength = VHCD_USB30_PORT_BITMAP_BYTES;
        }
        WdfRequestComplete(R, STATUS_SUCCESS);
        return;
    }
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubInterrupt20Count);
    WdfSpinLockAcquire(gSlotLock);
    for (i = 0; i < VHCD_MAX_PORTS; ++i) {
        if (gSlots[i].ConnectionChanged || gSlots[i].EnableChanged ||
            gSlots[i].ResetChanged || gSlots[i].SuspendChanged)
            bitmap |= 1u << (i + 1);
    }
    /* UCX has one root-hub object per controller and assigns USB 3 ports the
     * disjoint global numbers 17..23. Preserve those bits in the three-byte
     * aggregate bitmap; a one-byte USB 3-local bitmap is handled above. */
    for (ssPort = 0; ssPort < VHCD_MAX_USB30_PORTS; ++ssPort) {
        PVHCD_SLOT slot = &gSlots[VHCD_USB30_FIRST_SLOT_INDEX + ssPort];
        if (slot->ConnectionChanged || slot->ResetChanged ||
            slot->BHResetChanged || slot->LinkStateChanged || slot->ConfigErrorChanged)
            bitmap |= 1u << (VHCD_USB30_FIRST_PORT_NUMBER + ssPort);
    }
    WdfSpinLockRelease(gSlotLock);
    if (length < bytes || data == NULL) urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
    else { RtlZeroMemory(data, bytes); RtlCopyMemory(data, &bitmap, bytes); urb->UrbHeader.Status = USBD_STATUS_SUCCESS; urb->UrbBulkOrInterruptTransfer.TransferBufferLength = bytes; }
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubGetInfo(UCXROOTHUB H, WDFREQUEST R)
{
    WDF_REQUEST_PARAMETERS p;
    PROOTHUB_INFO info;
    UNREFERENCED_PARAMETER(H);
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubGetInfoCount);
    WDF_REQUEST_PARAMETERS_INIT(&p); WdfRequestGetParameters(R, &p);
    info = (PROOTHUB_INFO)p.Parameters.Others.Arg1;
    if (info == NULL || info->Size < sizeof(*info)) {
        gUcxState.RootHubGetInfoStatus = STATUS_INVALID_PARAMETER;
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return;
    }
    info->ControllerType = ControllerTypeSoftXhci;
    info->NumberOf20Ports = VHCD_MAX_PORTS;
    info->NumberOf30Ports = VHCD_MAX_USB30_PORTS;
    info->MaxU1ExitLatency = 0;
    info->MaxU2ExitLatency = 0;
    gUcxState.RootHubGetInfoStatus = STATUS_SUCCESS;
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubGet20PortInfo(UCXROOTHUB H, WDFREQUEST R)
{
    WDF_REQUEST_PARAMETERS p;
    PROOTHUB_20PORTS_INFO info;
    USHORT i;
    UNREFERENCED_PARAMETER(H);
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubGet20PortInfoCount);
    WDF_REQUEST_PARAMETERS_INIT(&p); WdfRequestGetParameters(R, &p);
    info = (PROOTHUB_20PORTS_INFO)p.Parameters.Others.Arg1;
    if (info == NULL || info->PortInfoArray == NULL || info->NumberOfPorts < VHCD_MAX_PORTS || info->PortInfoSize < sizeof(ROOTHUB_20PORT_INFO)) {
        gUcxState.RootHubGet20PortInfoStatus = STATUS_INVALID_PARAMETER;
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return;
    }
    for (i = 0; i < VHCD_MAX_PORTS; ++i) {
        PROOTHUB_20PORT_INFO port = info->PortInfoArray[i];
        if (port == NULL) {
            gUcxState.RootHubGet20PortInfoStatus = STATUS_INVALID_PARAMETER;
            WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return;
        }
        port->PortNumber = i + 1; port->MinorRevision = 0; port->HubDepth = 0;
        port->Removable = TriStateTrue; port->IntegratedHubImplemented = TriStateFalse; port->DebugCapable = TriStateFalse;
        port->ControllerUsb20HardwareLpmFlags.AsUchar = 0;
    }
    gUcxState.RootHubGet20PortInfoStatus = STATUS_SUCCESS;
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID VhcdEvtRootHubGet30PortInfo(UCXROOTHUB H, WDFREQUEST R)
{
    WDF_REQUEST_PARAMETERS p;
    PROOTHUB_30PORTS_INFO info;
    USHORT i;
    UNREFERENCED_PARAMETER(H);
    InterlockedIncrement((volatile LONG*)&gUcxState.RootHubGet30PortInfoCount);
    WDF_REQUEST_PARAMETERS_INIT(&p); WdfRequestGetParameters(R, &p);
    info = (PROOTHUB_30PORTS_INFO)p.Parameters.Others.Arg1;
    if (info == NULL || info->Size < sizeof(*info) || info->PortInfoArray == NULL ||
        info->NumberOfPorts != VHCD_MAX_USB30_PORTS || info->PortInfoSize < sizeof(ROOTHUB_30PORT_INFO)) {
        gUcxState.RootHubGet30PortInfoStatus = STATUS_INVALID_PARAMETER;
        gUcxState.RootHubGet30NumberOfPorts = info != NULL ? info->NumberOfPorts : 0;
        gUcxState.RootHubGet30PortInfoSize = info != NULL ? info->PortInfoSize : 0;
        WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return;
    }
    /* Keep USB 2.0 and USB 3.0 port numbers disjoint for UCX PortPath mapping. */
    for (i = 0; i < VHCD_MAX_USB30_PORTS; ++i) {
        PROOTHUB_30PORT_INFO port = info->PortInfoArray[i];
        if (port == NULL) {
            gUcxState.RootHubGet30PortInfoStatus = STATUS_INVALID_PARAMETER;
            gUcxState.RootHubGet30NumberOfPorts = info->NumberOfPorts;
            gUcxState.RootHubGet30PortInfoSize = info->PortInfoSize;
            WdfRequestComplete(R, STATUS_INVALID_PARAMETER); return;
        }
        port->PortNumber = VHCD_USB30_FIRST_PORT_NUMBER + i;
        port->MinorRevision = 0;
        port->HubDepth = 0;
        port->Removable = TriStateTrue;
        port->DebugCapable = TriStateFalse;
    }
    gUcxState.RootHubGet30NumberOfPorts = info->NumberOfPorts;
    gUcxState.RootHubGet30PortInfoSize = info->PortInfoSize;
    gUcxState.RootHubGet30PortInfoStatus = STATUS_SUCCESS;
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

/* Caller holds gSlotLock. Remember the ABI identity because user mode sends
 * END after processing BEGIN even when the underlying URB was cancelled. */
static VOID VhcdRememberCancelledEndLocked(_Inout_ PVHCD_TRANSACTION Transaction)
{
    PVHCD_CANCELLED_END entry;
    if (!Transaction->Begun || Transaction->EndAcknowledged || Transaction->CancellationRecorded)
        return;
    entry = &gCancelledEnds[gCancelledEndCursor];
    entry->Valid = TRUE;
    entry->SlotIndex = (ULONG)Transaction->SlotIndex;
    entry->Generation = Transaction->Generation;
    entry->Id = Transaction->Id;
    entry->Sequence = Transaction->Sequence;
    gCancelledEndCursor = (gCancelledEndCursor + 1) % RTL_NUMBER_OF(gCancelledEnds);
    Transaction->CancellationRecorded = TRUE;
}

/* Caller holds gSlotLock. */
static BOOLEAN VhcdConsumeCancelledEndLocked(_In_ LONG SlotIndex, _In_ ULONGLONG Generation,
                                             _In_ ULONGLONG Id, _In_ LONG Sequence)
{
    ULONG index;
    for (index = 0; index < RTL_NUMBER_OF(gCancelledEnds); ++index) {
        PVHCD_CANCELLED_END entry = &gCancelledEnds[index];
        if (entry->Valid && entry->SlotIndex == (ULONG)SlotIndex &&
            entry->Generation == Generation && entry->Id == Id && entry->Sequence == Sequence) {
            entry->Valid = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

static VOID VhcdFinishTransfer(PVHCD_TRANSACTION Transaction, NTSTATUS Status, USBD_STATUS UsbStatus)
{
    LONG slot = Transaction->SlotIndex;
    BOOLEAN rootHubChanged = FALSE;
    if (InterlockedExchange(&Transaction->OwnerFinished, 1) != 0) return;
    if (Transaction->Urb != NULL) Transaction->Urb->UrbHeader.Status = UsbStatus;
    if (Transaction->Request != NULL) WdfRequestComplete(Transaction->Request, Status);
    WdfSpinLockAcquire(gSlotLock);
    if (Status == STATUS_CANCELLED)
        VhcdRememberCancelledEndLocked(Transaction);
    if (VhcdSlotMatches(slot, Transaction->Generation) && Transaction->IsPortReset &&
        gSlots[slot].ResetInProgress) {
        PVHCD_SLOT port = &gSlots[slot];
        port->ResetInProgress = FALSE;
        if (port->Present && !VhcdSlotPowerOff(slot)) {
            if (NT_SUCCESS(Status)) {
                if (!port->Enabled && slot < VHCD_MAX_PORTS) port->EnableChanged = TRUE;
                port->Enabled = TRUE;
                port->Suspended = FALSE;
            }
            if (slot >= VHCD_USB30_FIRST_SLOT_INDEX) {
                UCHAR linkState = NT_SUCCESS(Status) ? PORT_LINK_STATE_U0 : PORT_LINK_STATE_INACTIVE;
                if (port->LinkState != linkState) {
                    port->LinkState = linkState;
                    port->LinkStateChanged = TRUE;
                }
                if (Transaction->IsWarmReset) port->BHResetChanged = TRUE;
                else port->ResetChanged = TRUE;
            } else {
                port->ResetChanged = TRUE;
            }
            rootHubChanged = TRUE;
        }
    }
    if (VhcdSlotMatches(slot, Transaction->Generation) && gSlots[slot].TransferRefs > 0)
        --gSlots[slot].TransferRefs;
    if (VhcdSlotMatches(slot, Transaction->Generation) && gSlots[slot].TransactionBytes >= Transaction->AllocationBytes)
        gSlots[slot].TransactionBytes -= Transaction->AllocationBytes;
    if (VhcdSlotMatches(slot, Transaction->Generation) && gSlots[slot].TransactionCount > 0)
        --gSlots[slot].TransactionCount;
    if (gTransactionBytes >= Transaction->AllocationBytes) gTransactionBytes -= Transaction->AllocationBytes;
    if (gTransactionCount > 0) --gTransactionCount;
    if (gTransactionCount == 0) KeSetEvent(&gTransactionsDrained, IO_NO_INCREMENT, FALSE);
    WdfSpinLockRelease(gSlotLock);
    if (rootHubChanged && !gStopping && gRootHub != WDF_NO_HANDLE)
        UcxRootHubPortChanged(gRootHub);
    VhcdFinalizeSlotIfQuiescent(slot, Transaction->Generation);
    VhcdTxRelease(Transaction); /* Release the pending USB request's ownership. */
}

static VOID VhcdTransactionCancelDpc(_In_ PKDPC Dpc, _In_opt_ PVOID DeferredContext,
                                     _In_opt_ PVOID SystemArgument1, _In_opt_ PVOID SystemArgument2)
{
    PVHCD_TRANSACTION transaction;
    if (DeferredContext == NULL) return;
    transaction = (PVHCD_TRANSACTION)DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    VhcdFinishTransfer(transaction, STATUS_CANCELLED, USBD_STATUS_CANCELED);
}

static VOID VhcdInterruptReadyDpc(_In_ PKDPC Dpc, _In_opt_ PVOID DeferredContext,
                                  _In_opt_ PVOID SystemArgument1, _In_opt_ PVOID SystemArgument2)
{
    PVHCD_TRANSACTION transaction = (PVHCD_TRANSACTION)DeferredContext;
    PKSEMAPHORE semaphore = NULL;
    LONG slot;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    if (transaction == NULL) return;

    slot = transaction->SlotIndex;
    WdfSpinLockAcquire(gSlotLock);
    if (!gStopping && VhcdSlotMatches(slot, transaction->Generation) &&
        gSlots[slot].Present && transaction->Linked && !transaction->Ending) {
        transaction->Ready = TRUE;
        semaphore = gSlots[slot].Semaphore;
        if (semaphore != NULL) ObReferenceObject(semaphore);
    }
    WdfSpinLockRelease(gSlotLock);

    if (semaphore != NULL) {
        KeReleaseSemaphore(semaphore, IO_NO_INCREMENT, 1, FALSE);
        ObDereferenceObject(semaphore);
    }
    if (InterlockedExchange(&transaction->ReadyTimerReferenceHeld, 0) == 1)
        VhcdTxRelease(transaction);
}

static VOID VhcdCancelReadyTimer(_Inout_ PVHCD_TRANSACTION Transaction)
{
    BOOLEAN removed;
    if (InterlockedCompareExchange(&Transaction->ReadyTimerReferenceHeld, 0, 0) == 0)
        return;
    /* If cancellation races a running DPC, the DPC releases the timer reference. */
    removed = KeCancelTimer(&Transaction->ReadyTimer);
    if (!removed) removed = KeRemoveQueueDpc(&Transaction->ReadyDpc);
    if (removed && InterlockedExchange(&Transaction->ReadyTimerReferenceHeld, 0) == 1)
        VhcdTxRelease(Transaction);
}

static VOID VhcdQueueTransactionCancellation(PVHCD_TRANSACTION Transaction)
{
    if (InterlockedCompareExchange(&Transaction->CancelCompletionQueued, 1, 0) != 0) return;
    VhcdCancelReadyTimer(Transaction);
    KeInitializeDpc(&Transaction->CancelDpc, VhcdTransactionCancelDpc, Transaction);
    KeInsertQueueDpc(&Transaction->CancelDpc, NULL, NULL);
}

static ULONGLONG VhcdReserveInterruptDeadline(_Inout_ PVHCD_QUEUE_CONTEXT QueueContext)
{
    ULONGLONG now;
    ULONGLONG interval = QueueContext->InterruptInterval100ns;
    LONG64 observed;
    if (interval == 0) return 0;
    /* Keep concurrent requests on one endpoint at least one interval apart. */
    now = KeQueryInterruptTime();
    observed = InterlockedCompareExchange64(&QueueContext->NextInterruptDueTime100ns, 0, 0);
    for (;;) {
        ULONGLONG base = observed > 0 && (ULONGLONG)observed > now
            ? (ULONGLONG)observed : now;
        ULONGLONG due = base + interval;
        LONG64 previous = InterlockedCompareExchange64(
            &QueueContext->NextInterruptDueTime100ns,
            (LONG64)due,
            observed);
        if (previous == observed) return due;
        observed = previous;
    }
}

/* Caller holds gSlotLock. */
static VOID VhcdMarkPortResetStartedLocked(_Inout_ PVHCD_SLOT Port, _In_ LONG SlotIndex)
{
    if (Port->Enabled && SlotIndex < VHCD_MAX_PORTS) Port->EnableChanged = TRUE;
    if (Port->Suspended && SlotIndex < VHCD_MAX_PORTS) Port->SuspendChanged = TRUE;
    Port->Enabled = FALSE;
    Port->Suspended = FALSE;
    Port->ResetInProgress = TRUE;
    if (SlotIndex >= VHCD_USB30_FIRST_SLOT_INDEX) {
        if (Port->LinkState != PORT_LINK_STATE_HOT_RESET)
            Port->LinkStateChanged = TRUE;
        Port->LinkState = PORT_LINK_STATE_HOT_RESET;
    }
}

static NTSTATUS VhcdQueuePortResetNotification(_In_ LONG SlotIndex, _In_ ULONGLONG Generation, _In_ LONG ResetType,
                                               _In_ BOOLEAN AllowPoweredOffPort,
                                               _In_ BOOLEAN ChangesPortState,
                                               _In_ BOOLEAN IsWarmReset)
{
    PVHCD_TRANSACTION transaction;
    PKSEMAPHORE semaphore;
    ULONG allocationBytes = sizeof(*transaction) + sizeof(ioctl_usbtx_header_t);
    ioctl_usbtx_header_t* outer;
    PVHCD_WIRE_REQUEST inner;
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_SLOTS) return STATUS_INVALID_PARAMETER;
    transaction = (PVHCD_TRANSACTION)ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationBytes, VHCD_TX_SIGNATURE);
    if (transaction == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    /* ExAllocatePool2 zeroes this allocation, including the user-visible wire buffer. */
    transaction->Signature = VHCD_TX_SIGNATURE;
    transaction->SlotIndex = SlotIndex;
    transaction->Generation = Generation;
    transaction->EndpointAddress = 0;
    transaction->AllocationBytes = allocationBytes;
    transaction->PayloadCapacity = 0;
    transaction->Ready = TRUE;
    transaction->References = 1; /* Owned until END or port teardown. */
    transaction->IsResetNotification = TRUE;
    transaction->IsPortReset = ChangesPortState;
    transaction->IsWarmReset = IsWarmReset;
    transaction->ResetType = ResetType;
    outer = (ioctl_usbtx_header_t*)transaction->Buffer;
    inner = (PVHCD_WIRE_REQUEST)((PUCHAR)transaction->Buffer + FIELD_OFFSET(ioctl_usbtx_header_t, type));
    outer->data_length = 0;
    outer->result = -1;
    inner->Type = 4;
    inner->Result = -1;
    inner->DataLength = 0;
    inner->Data.Reset.Type = ResetType;

    WdfSpinLockAcquire(gSlotLock);
    if (gStopping || !VhcdSlotMatches(SlotIndex, Generation) || !gSlots[SlotIndex].Present ||
        (VhcdSlotPowerOff(SlotIndex) && !AllowPoweredOffPort)) {
        WdfSpinLockRelease(gSlotLock);
        ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    for (PLIST_ENTRY entry = gSlots[SlotIndex].Transactions.Flink;
         entry != &gSlots[SlotIndex].Transactions; entry = entry->Flink) {
        PVHCD_TRANSACTION existing = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
        if (existing->IsResetNotification && !existing->Ending && existing->ResetType == ResetType) {
            if (ChangesPortState) {
                if (!gSlots[SlotIndex].ResetInProgress)
                    VhcdMarkPortResetStartedLocked(&gSlots[SlotIndex], SlotIndex);
                existing->IsPortReset = TRUE;
                existing->IsWarmReset = IsWarmReset;
            }
            WdfSpinLockRelease(gSlotLock);
            ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
            return STATUS_SUCCESS;
        }
    }
    if (ChangesPortState && gSlots[SlotIndex].ResetInProgress) {
        /* Make repeated SET_FEATURE(PORT_RESET) requests idempotent. */
        WdfSpinLockRelease(gSlotLock);
        ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
        return STATUS_SUCCESS;
    }
    if (gSlots[SlotIndex].TransactionCount >= VHCD_MAX_SLOT_TRANSACTIONS ||
        gSlots[SlotIndex].TransactionBytes > VHCD_MAX_SLOT_TRANSACTION_BYTES ||
        allocationBytes > VHCD_MAX_SLOT_TRANSACTION_BYTES - gSlots[SlotIndex].TransactionBytes ||
        gTransactionCount >= VHCD_MAX_CONTROLLER_TRANSACTIONS ||
        gTransactionBytes > VHCD_MAX_CONTROLLER_TRANSACTION_BYTES ||
        allocationBytes > VHCD_MAX_CONTROLLER_TRANSACTION_BYTES - gTransactionBytes) {
        WdfSpinLockRelease(gSlotLock);
        ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (ChangesPortState) {
        VhcdMarkPortResetStartedLocked(&gSlots[SlotIndex], SlotIndex);
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
    if (gTransactionCount == 0) KeClearEvent(&gTransactionsDrained);
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
    if (!transaction->EndAcknowledged)
        VhcdRememberCancelledEndLocked(transaction);
    if (transaction->Linked) {
        RemoveEntryList(&transaction->Link);
        transaction->Linked = FALSE;
    }
    transaction->Ending = TRUE;
    WdfSpinLockRelease(gSlotLock);
    /* A canceled registration transfers completion ownership to this callback.
     * No Unmark caller may queue completion on STATUS_CANCELLED. */
    VhcdQueueTransactionCancellation(transaction);
}

static VOID VhcdCancelSlotTransactions(_In_ LONG SlotIndex, _In_ ULONGLONG Generation,
                                      _In_ UCHAR EndpointAddress, _In_ BOOLEAN AllEndpoints,
                                      _In_ BOOLEAN PreserveResetNotifications)
{
    if (SlotIndex < 0 || SlotIndex >= VHCD_MAX_SLOTS) return;
    for (;;) {
        PVHCD_TRANSACTION transaction = NULL;
        NTSTATUS cancelStatus = STATUS_SUCCESS;
        WdfSpinLockAcquire(gSlotLock);
        if (VhcdSlotMatches(SlotIndex, Generation)) {
            for (PLIST_ENTRY entry = gSlots[SlotIndex].Transactions.Flink; entry != &gSlots[SlotIndex].Transactions; entry = entry->Flink) {
                PVHCD_TRANSACTION candidate = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
                if (!candidate->Ending &&
                    !(PreserveResetNotifications && candidate->IsResetNotification) &&
                    (AllEndpoints || candidate->EndpointAddress == EndpointAddress)) {
                    VhcdRememberCancelledEndLocked(candidate);
                    candidate->Ending = TRUE;
                    candidate->Linked = FALSE;
                    RemoveEntryList(&candidate->Link);
                    VhcdTxAddRef(candidate);
                    transaction = candidate;
                    /* Serialize selection and Unmark with EvtTransferCanceled.
                     * Otherwise the callback could finish/free the WDF request
                     * between removing this entry and calling Unmark. */
                    if (candidate->Request != NULL)
                        cancelStatus = WdfRequestUnmarkCancelable(candidate->Request);
                    break;
                }
            }
        }
        WdfSpinLockRelease(gSlotLock);
        if (transaction == NULL) break;
        if (cancelStatus != STATUS_CANCELLED) {
            VhcdQueueTransactionCancellation(transaction);
        }
        VhcdTxRelease(transaction);
    }
}

static VOID VhcdCancelEndpointTransfers(_In_ UCXENDPOINT Endpoint)
{
    PVHCD_ENDPOINT_CONTEXT context = VhcdGetEndpointContext(Endpoint);
    if (context != NULL) VhcdCancelSlotTransactions(
        (LONG)context->SlotIndex, context->Generation, context->EndpointAddress, FALSE, TRUE);
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

static NTSTATUS VhcdMapUrb(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ PURB Urb)
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
    PVHCD_TRANSFER_CONTEXT requestContext = VhcdGetTransferContext(Request);
    PKSEMAPHORE signalSemaphore = NULL;
    NTSTATUS cancelStatus;
    if (requestContext == NULL) return STATUS_INVALID_DEVICE_STATE;
    requestContext->Transaction = NULL;
    if (slot < 0 || slot >= VHCD_MAX_SLOTS) return STATUS_INVALID_DEVICE_STATE;

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
        if (Urb->UrbOSFeatureDescriptorRequest.Recipient > 2 ||
            (Urb->UrbOSFeatureDescriptorRequest.Recipient == 0 &&
             Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber != 0) ||
            Urb->UrbOSFeatureDescriptorRequest.MS_PageIndex != 0)
            return STATUS_INVALID_PARAMETER;
        transferLength = Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength;
        if (transferLength < sizeof(UCHAR)) return STATUS_BUFFER_TOO_SMALL;
        if (transferLength > VHCD_MAX_MS_FEATURE_DESCRIPTOR_BYTES) return STATUS_INVALID_PARAMETER;
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
        if ((function == URB_FUNCTION_SET_FEATURE_TO_DEVICE ||
            function == URB_FUNCTION_SET_FEATURE_TO_OTHER ||
            function == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE ||
            function == URB_FUNCTION_CLEAR_FEATURE_TO_OTHER) &&
            Urb->UrbControlFeatureRequest.Index != 0) return STATUS_INVALID_PARAMETER;
        break;
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbPipeRequest) || queueContext->PipeType == UsbdPipeTypeControl) return STATUS_INVALID_PARAMETER;
        VhcdCancelSlotTransactions(
            slot, queueContext->Generation, queueContext->EndpointAddress, FALSE, TRUE);
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
    if (function == URB_FUNCTION_CONTROL_TRANSFER || function == URB_FUNCTION_CONTROL_TRANSFER_EX) {
        PUCHAR setup = function == URB_FUNCTION_CONTROL_TRANSFER
            ? Urb->UrbControlTransfer.SetupPacket : Urb->UrbControlTransferEx.SetupPacket;
        const BOOLEAN setupIsRead = (setup[0] & 0x80) != 0;
        const ULONG setupLength = (ULONG)setup[6] | ((ULONG)setup[7] << 8);
        if (setupIsRead != isRead || (!isRead && transferLength != setupLength))
            return STATUS_INVALID_PARAMETER;
        /* wLength bounds the USB data stage even when the caller allocated a
         * larger IN buffer. Report only the bytes the setup packet requested. */
        if (isRead && transferLength > setupLength) transferLength = setupLength;
    }
    if (function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
        (((queueContext->EndpointAddress & 0x80) != 0) != (isRead != FALSE)))
        return STATUS_INVALID_PARAMETER;
    if (isIso) {
        ULONG index;
        if (queueContext->MaxIsoPacketBytes == 0) return STATUS_INVALID_PARAMETER;
        for (index = 0; index < Urb->UrbIsochronousTransfer.NumberOfPackets; ++index) {
            const USBD_ISO_PACKET_DESCRIPTOR* packet = &Urb->UrbIsochronousTransfer.IsoPacket[index];
            if (packet->Offset > transferLength ||
                (index != 0 && packet->Offset < Urb->UrbIsochronousTransfer.IsoPacket[index - 1].Offset))
                return STATUS_INVALID_PARAMETER;
        }
    }
    if (transferLength > VHCD_MAX_TRANSACTION_BYTES || metadataLength > MAXULONG - transferLength) return STATUS_INVALID_PARAMETER;
    payloadLength = transferLength + metadataLength;
    if (payloadLength > VHCD_MAX_TRANSACTION_BYTES) return STATUS_INVALID_PARAMETER;
    allocationBytes = sizeof(*transaction) + sizeof(ioctl_usbtx_header_t) + payloadLength;
    transaction = (PVHCD_TRANSACTION)ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationBytes, VHCD_TX_SIGNATURE);
    if (transaction == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    /* ExAllocatePool2 zeroes this allocation, including the user-visible wire buffer. */
    transaction->Signature = VHCD_TX_SIGNATURE;
    transaction->SlotIndex = slot;
    transaction->Generation = queueContext->Generation;
    transaction->Request = Request;
    transaction->Urb = Urb;
    transaction->TransferLength = transferLength;
    transaction->MaxIsoPacketBytes = isIso ? queueContext->MaxIsoPacketBytes : 0;
    transaction->MetadataLength = metadataLength;
    transaction->PayloadCapacity = payloadLength;
    transaction->AllocationBytes = allocationBytes;
    transaction->EndpointAddress = queueContext->EndpointAddress;
    transaction->TransferBuffer = VhcdTransferBuffer(transferLength, transferBuffer, transferMdl);
    transaction->IsRead = isRead;
    transaction->Isochronous = isIso;
    transaction->References = 1;
    transaction->Ready = TRUE;
    KeInitializeTimer(&transaction->ReadyTimer);
    KeInitializeDpc(&transaction->ReadyDpc, VhcdInterruptReadyDpc, transaction);
    if (function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
        queueContext->PipeType == UsbdPipeTypeInterrupt)
        transaction->ReadyTime100ns = VhcdReserveInterruptDeadline(queueContext);
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
                ULONG offset = Urb->UrbIsochronousTransfer.IsoPacket[i].Offset;
                ULONG nextOffset = i + 1 < iso->number_packets
                    ? Urb->UrbIsochronousTransfer.IsoPacket[i + 1].Offset
                    : transferLength;
                iso->packets[i].offset = Urb->UrbIsochronousTransfer.IsoPacket[i].Offset;
                iso->packets[i].length = min(nextOffset - offset, queueContext->MaxIsoPacketBytes);
                iso->packets[i].status = 0;
            }
            break;
        }
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
            inner->Type = 3;
            inner->Data.Transfer.Type = 2;
            inner->Data.Transfer.EpAddress = queueContext->EndpointAddress;
            inner->Data.Transfer.IsRead = isRead ? 1 : 0;
            /* The frozen ABI reserves this word for non-iso transfers. Carry
             * the endpoint type so a responder can distinguish bulk/interrupt. */
            inner->Data.Transfer.Reserved0 = (LONG)queueContext->PipeType;
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
            /* Preserve the existing low 16-bit ABI and carry MS_PageIndex above it. */
            inner->Data.Descriptor.Index =
                ((LONG)Urb->UrbOSFeatureDescriptorRequest.Recipient << 8) |
                Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber |
                ((LONG)Urb->UrbOSFeatureDescriptorRequest.MS_PageIndex << 16);
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
                __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
                    GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
                    ExFreePoolWithTag(transaction, VHCD_TX_SIGNATURE);
                    return GetExceptionCode();
                }
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
    requestContext->Transaction = transaction;
    WdfSpinLockAcquire(gSlotLock);
    if (gStopping || !VhcdSlotMatches(slot, queueContext->Generation) || !gSlots[slot].Present || VhcdSlotPowerOff(slot)) {
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
    if (gTransactionCount == 0) KeClearEvent(&gTransactionsDrained);
    ++gTransactionCount;
    signalSemaphore = gSlots[slot].Semaphore;
    ObReferenceObject(signalSemaphore);
    Urb->UrbHeader.Status = USBD_STATUS_PENDING;
    /* MarkCancelableEx never invokes EvtTransferCanceled synchronously. Hold
     * the slot lock until registration is resolved so END/teardown cannot see
     * a linked transaction whose cancellation ownership is still undecided. */
    cancelStatus = WdfRequestMarkCancelableEx(Request, VhcdEvtTransferCanceled);
    if (!NT_SUCCESS(cancelStatus)) {
        RemoveEntryList(&transaction->Link);
        transaction->Linked = FALSE;
        transaction->Ending = TRUE;
    } else {
        ULONGLONG now = KeQueryInterruptTime();
        if (transaction->ReadyTime100ns != 0 &&
            transaction->ReadyTime100ns > now) {
            LARGE_INTEGER dueTime;
            ULONGLONG delay = transaction->ReadyTime100ns - now;
            transaction->Ready = FALSE;
            VhcdTxAddRef(transaction);
            InterlockedExchange(&transaction->ReadyTimerReferenceHeld, 1);
            dueTime.QuadPart = -(LONGLONG)delay;
            KeSetTimer(&transaction->ReadyTimer, dueTime, &transaction->ReadyDpc);
        } else {
            transaction->Ready = TRUE;
            KeReleaseSemaphore(signalSemaphore, IO_NO_INCREMENT, 1, FALSE);
        }
    }
    WdfSpinLockRelease(gSlotLock);
    /* A failed MarkCancelableEx registration has no cancellation callback; the
     * registering path retains completion ownership, including STATUS_CANCELLED. */
    if (!NT_SUCCESS(cancelStatus)) VhcdQueueTransactionCancellation(transaction);
    ObDereferenceObject(signalSemaphore);
    VhcdTxRelease(transaction);
    if (!NT_SUCCESS(cancelStatus)) return STATUS_PENDING;
    return STATUS_SUCCESS;
}

static VOID VhcdCompleteRequestDpc(_In_ PKDPC Dpc, _In_opt_ PVOID DeferredContext,
                                  _In_opt_ PVOID SystemArgument1, _In_opt_ PVOID SystemArgument2)
{
    PVHCD_TRANSFER_CONTEXT context;
    WDFREQUEST request;
    PURB urb;
    NTSTATUS requestStatus;
    USBD_STATUS usbStatus;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    if (DeferredContext == NULL) return;
    context = (PVHCD_TRANSFER_CONTEXT)DeferredContext;
    request = context->CompletionRequest;
    urb = context->CompletionUrb;
    requestStatus = context->CompletionStatus;
    usbStatus = context->CompletionUsbStatus;
    context->CompletionRequest = NULL;
    context->CompletionUrb = NULL;
    if (urb != NULL) urb->UrbHeader.Status = usbStatus;
    WdfRequestComplete(request, requestStatus);
}

static NTSTATUS VhcdEnsureTransferContext(_In_ WDFREQUEST Request,
                                         _Outptr_ PVHCD_TRANSFER_CONTEXT* Context)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    if (Request == WDF_NO_HANDLE || Context == NULL) return STATUS_INVALID_PARAMETER;
    *Context = VhcdGetTransferContext(Request);
    if (*Context != NULL) return STATUS_SUCCESS;

    /* UCX-delivered queue requests may not carry the device-default context. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VHCD_TRANSFER_CONTEXT);
    status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)Context);
    if (status == STATUS_OBJECT_NAME_EXISTS) {
        *Context = VhcdGetTransferContext(Request);
        return *Context != NULL ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
    }
    if (!NT_SUCCESS(status)) return status;
    return *Context != NULL ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
}

static VOID VhcdCompleteRequestDeferred(_In_ WDFREQUEST Request, _In_opt_ PURB Urb,
                                        _In_ NTSTATUS RequestStatus, _In_ USBD_STATUS UsbStatus)
{
    PVHCD_TRANSFER_CONTEXT context = NULL;
    NTSTATUS status = VhcdEnsureTransferContext(Request, &context);
    if (!NT_SUCCESS(status)) {
        /* A contextless request cannot hold a DPC; preserve status and fail safely. */
        if (Urb != NULL) Urb->UrbHeader.Status = UsbStatus;
        WdfRequestComplete(Request, status);
        return;
    }
    context->CompletionRequest = Request;
    context->CompletionUrb = Urb;
    context->CompletionStatus = RequestStatus;
    context->CompletionUsbStatus = UsbStatus;
    KeInitializeDpc(&context->CompletionDpc, VhcdCompleteRequestDpc, context);
    KeInsertQueueDpc(&context->CompletionDpc, NULL, NULL);
}

VOID VhcdEvtEndpointIoInternalDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG IoControlCode)
{
    WDF_REQUEST_PARAMETERS parameters;
    PURB urb = NULL;
    PVHCD_TRANSFER_CONTEXT requestContext = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    if (IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WDF_REQUEST_PARAMETERS_INIT(&parameters);
        WdfRequestGetParameters(Request, &parameters);
        urb = (PURB)parameters.Parameters.Others.Arg1;
    }
    if (urb == NULL) {
        VhcdCompleteRequestDeferred(Request, NULL, STATUS_INVALID_DEVICE_REQUEST,
                                    USBD_STATUS_INVALID_URB_FUNCTION);
        return;
    }
    status = VhcdEnsureTransferContext(Request, &requestContext);
    if (!NT_SUCCESS(status)) {
        urb->UrbHeader.Status = status == STATUS_INSUFFICIENT_RESOURCES
            ? USBD_STATUS_INSUFFICIENT_RESOURCES : USBD_STATUS_INVALID_PARAMETER;
        WdfRequestComplete(Request, status);
        return;
    }
    {
        status = VhcdMapUrb(Queue, Request, urb);
        USBD_STATUS usbStatus;
        if (NT_SUCCESS(status)) return;
        usbStatus = status == STATUS_NOT_SUPPORTED ? USBD_STATUS_NOT_SUPPORTED :
            (status == STATUS_DEVICE_NOT_CONNECTED ? USBD_STATUS_DEVICE_GONE :
             (status == STATUS_BUFFER_TOO_SMALL ? USBD_STATUS_BUFFER_TOO_SMALL : USBD_STATUS_INVALID_PARAMETER));
        VhcdCompleteRequestDeferred(Request, urb, STATUS_SUCCESS, usbStatus);
    }
}

VOID VhcdEvtIoCanceledOnQueue(WDFQUEUE Queue, WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS parameters;
    PIO_STACK_LOCATION stack;
    PURB urb = NULL;
    UNREFERENCED_PARAMETER(Queue);
    /* The request context is preallocated by WDF, so cancellation needs no pool allocation. */
    stack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request));
    if (stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL &&
        stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WDF_REQUEST_PARAMETERS_INIT(&parameters);
        WdfRequestGetParameters(Request, &parameters);
        urb = (PURB)parameters.Parameters.Others.Arg1;
    }
    VhcdCompleteRequestDeferred(Request, urb, STATUS_CANCELLED, USBD_STATUS_CANCELED);
}

VOID VhcdEvtIoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS parameters;
    WDF_OBJECT_ATTRIBUTES attributes;
    PVHCD_USER_IO_CONTEXT context;
    PVOID userBuffer = NULL;
    size_t bufferLength = 0;
    NTSTATUS status;
    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(Request, &parameters);
    /* UCX routes kernel USB requests to its own queues. Only the private
     * user protocol requires caller-context handle/buffer capture here. Both
     * legacy and SuperSpeed ADD requests carry user handles that must be
     * referenced in the originating process context before queue dispatch. */
    if (parameters.Type != WdfRequestTypeDeviceControl ||
        (parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_PDO_ADD &&
         parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_PDO_ADD_SUPERSPEED &&
         parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_PDO_ADD_WITH_USB_SPEED &&
         parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_PDO_REMOVE &&
         parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_BEGIN_TRANS_USB_DATA &&
         parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_END_TRANS_USB_DATA)) {
        status = WdfDeviceEnqueueRequest(Device, Request);
        if (!NT_SUCCESS(status)) WdfRequestComplete(Request, status);
        return;
    }
    if (WdfRequestGetRequestorMode(Request) != UserMode) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return; }
    if (parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD ||
        parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD_SUPERSPEED ||
        parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED) {
        ioctl_pdo_create_t* create;
        PVOID inputBuffer;
        size_t requiredLength = parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED
            ? sizeof(ioctl_pdo_create_with_speed_t) : sizeof(ioctl_pdo_create_t);
        PKSEMAPHORE semaphore = NULL;
        PKEVENT removeEvent = NULL;
        status = WdfRequestRetrieveInputBuffer(Request, requiredLength, &inputBuffer, &bufferLength);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        create = parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED
            ? &((ioctl_pdo_create_with_speed_t*)inputBuffer)->Create
            : (ioctl_pdo_create_t*)inputBuffer;
        if (parameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED) {
            ULONG requestedSpeed = ((ioctl_pdo_create_with_speed_t*)inputBuffer)->UsbSpeed;
            if (requestedSpeed != XUSB_USB_SPEED_LOW && requestedSpeed != XUSB_USB_SPEED_FULL) {
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
        }
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
        /* This request has not been delivered by a queue yet. Return it to
         * KMDF with EnqueueRequest rather than trying to requeue it. */
        status = WdfDeviceEnqueueRequest(Device, Request);
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
        status = WdfDeviceEnqueueRequest(Device, Request);
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
        if (NT_SUCCESS(status) && userBuffer == NULL) status = STATUS_INVALID_USER_BUFFER;
        if (NT_SUCCESS(status)) status = WdfRequestProbeAndLockUserBufferForWrite(Request, userBuffer, bufferLength, &context->OutputMemory);
    } else {
        status = WdfRequestRetrieveUnsafeUserInputBuffer(Request, sizeof(ioctl_usbtx_header_t), &userBuffer, &bufferLength);
        if (NT_SUCCESS(status) && userBuffer == NULL) status = STATUS_INVALID_USER_BUFFER;
        if (NT_SUCCESS(status)) status = WdfRequestProbeAndLockUserBufferForRead(Request, userBuffer, bufferLength, &context->InputMemory);
    }
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    status = WdfDeviceEnqueueRequest(Device, Request);
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
        response->number_packets != packetCount || response->error_count > packetCount ||
        Transaction->MaxIsoPacketBytes == 0 ||
        Transaction->MetadataLength != headerBytes + packetCount * sizeof(iso_packet_t))
        return STATUS_INVALID_PARAMETER;
    responseBytes = (ULONG)Inner->DataLength;
    dataBytes = responseBytes - Transaction->MetadataLength;
    if (dataBytes > Transaction->TransferLength) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < packetCount; ++i) {
        ULONG offset = response->packets[i].offset;
        ULONG length = response->packets[i].length;
        USBD_ISO_PACKET_DESCRIPTOR* original = &urb->UrbIsochronousTransfer.IsoPacket[i];
        ULONG nextOffset = i + 1 < packetCount
            ? urb->UrbIsochronousTransfer.IsoPacket[i + 1].Offset
            : Transaction->TransferLength;
        ULONG packetCapacity;
        if (original->Offset > Transaction->TransferLength ||
            nextOffset < original->Offset || nextOffset > Transaction->TransferLength)
            return STATUS_INVALID_PARAMETER;
        packetCapacity = min(nextOffset - original->Offset, Transaction->MaxIsoPacketBytes);
        if (offset != original->Offset ||
            !xusb_iso_packet_fits_reply(offset, length, packetCapacity,
                dataBytes, Transaction->TransferLength))
            return STATUS_INVALID_PARAMETER;
        if (length != 0 && offset + length > used) used = offset + length;
        original->Length = length;
        original->Status = response->packets[i].status == 0 ? USBD_STATUS_SUCCESS : (USBD_STATUS)0x80000000;
    }
    if (Transaction->IsRead) {
        for (i = 0; i < packetCount; ++i) {
            ULONG offset = response->packets[i].offset;
            ULONG length = response->packets[i].length;
            if (length != 0) {
                RtlCopyMemory(Transaction->TransferBuffer + offset,
                    (PUCHAR)response + Transaction->MetadataLength + offset, length);
            }
        }
    }
    urb->UrbIsochronousTransfer.StartFrame = response->start_frame;
    urb->UrbIsochronousTransfer.ErrorCount = response->error_count;
    urb->UrbIsochronousTransfer.TransferBufferLength = used;
    return STATUS_SUCCESS;
}

static NTSTATUS VhcdApplyResponse(_Inout_ PVHCD_TRANSACTION Transaction, _In_ PVHCD_WIRE_REQUEST Inner, _In_ ULONG ResponseBytes)
{
    ULONG completedLength;
    if (Inner->DataLength < 0 || (ULONG)Inner->DataLength > Transaction->PayloadCapacity ||
        ResponseBytes < sizeof(ioctl_usbtx_header_t) + (ULONG)Inner->DataLength)
        return STATUS_INVALID_PARAMETER;
    if (Inner->Result != 0) return STATUS_UNSUCCESSFUL;
    if (Transaction->Urb == NULL)
        return Inner->DataLength == 0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    if (Transaction->Isochronous) return VhcdCompleteIsoResponse(Transaction, Inner);
    if ((ULONG)Inner->DataLength > Transaction->TransferLength) return STATUS_INVALID_PARAMETER;
    /* END carries response bytes for IN transfers; successful OUT requests already sent their full payload. */
    completedLength = Transaction->IsRead ? (ULONG)Inner->DataLength : Transaction->TransferLength;
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
    case URB_FUNCTION_CONTROL_TRANSFER: Transaction->Urb->UrbControlTransfer.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_CONTROL_TRANSFER_EX: Transaction->Urb->UrbControlTransferEx.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER: Transaction->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE: Transaction->Urb->UrbControlDescriptorRequest.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER: Transaction->Urb->UrbControlVendorClassRequest.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER: Transaction->Urb->UrbControlGetStatusRequest.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_GET_CONFIGURATION: Transaction->Urb->UrbControlGetConfigurationRequest.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_GET_INTERFACE: Transaction->Urb->UrbControlGetInterfaceRequest.TransferBufferLength = completedLength; break;
    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR: Transaction->Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength = completedLength; break;
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
    if (!gStopping && VhcdSlotMatches(fileContext->SlotIndex, fileContext->Generation)) {
        for (PLIST_ENTRY entry = gSlots[fileContext->SlotIndex].Transactions.Flink; entry != &gSlots[fileContext->SlotIndex].Transactions; entry = entry->Flink) {
            PVHCD_TRANSACTION candidate = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
            if (candidate->Ready && !candidate->Ending &&
                (!candidate->Begun || candidate->NeedsRetry)) {
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
    ULONG responseBytes = 0;
    BOOLEAN malformedResponse = FALSE;
    BOOLEAN cancelledEndAcknowledged = FALSE;
    NTSTATUS status = STATUS_NOT_FOUND;
    LONG slot;
    if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE || fileContext->SlotIndex < 0 || ioContext->InputMemory == NULL || InputLength < sizeof(header)) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
    input = (PUCHAR)WdfMemoryGetBuffer(ioContext->InputMemory, &bufferSize);
    if (input == NULL || bufferSize < sizeof(header)) { WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return; }
    RtlCopyMemory(&header, input, sizeof(header));
    WdfSpinLockAcquire(gSlotLock);
    slot = fileContext->SlotIndex;
    if (!gStopping && VhcdSlotMatches(slot, fileContext->Generation)) {
        for (PLIST_ENTRY entry = gSlots[slot].Transactions.Flink; entry != &gSlots[slot].Transactions; entry = entry->Flink) {
            PVHCD_TRANSACTION candidate = CONTAINING_RECORD(entry, VHCD_TRANSACTION, Link);
            if (candidate->Id == header.inter_handle && candidate->Sequence == header.inter_seqno && candidate->Begun && !candidate->Ending) {
                candidate->Ending = TRUE;
                candidate->EndAcknowledged = TRUE;
                VhcdTxAddRef(candidate);
                RemoveEntryList(&candidate->Link);
                candidate->Linked = FALSE;
                transaction = candidate;
                /* The cancel callback takes the same lock before unlinking or
                 * scheduling completion. Keep Unmark in this critical section
                 * so it can never be called after that completion. */
                status = candidate->Request != NULL
                    ? WdfRequestUnmarkCancelable(candidate->Request)
                    : STATUS_SUCCESS;
                break;
            }
        }
        if (transaction == NULL) {
            cancelledEndAcknowledged = VhcdConsumeCancelledEndLocked(
                slot, fileContext->Generation, header.inter_handle, header.inter_seqno);
        }
    }
    WdfSpinLockRelease(gSlotLock);
    if (transaction == NULL) {
        if (!cancelledEndAcknowledged) {
            WdfRequestComplete(Request, STATUS_NOT_FOUND);
            return;
        }
        if (bufferSize < InputLength || header.data_length < 0 ||
            (ULONG)header.data_length > VHCD_MAX_TRANSACTION_BYTES ||
            (ULONG)header.data_length > InputLength - sizeof(header) ||
            InputLength != sizeof(header) + (ULONG)header.data_length) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        {
            PVHCD_WIRE_REQUEST copiedInner = (PVHCD_WIRE_REQUEST)(
                (PUCHAR)&header + FIELD_OFFSET(ioctl_usbtx_header_t, type));
            if (header.result != copiedInner->Result ||
                header.data_length != copiedInner->DataLength || copiedInner->DataLength < 0) {
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
        }
        /* The cancelled URB is already completed; acknowledge its END so the
         * serialized user-mode transport can continue with the next request. */
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }
    if (status == STATUS_CANCELLED) {
        /* Cancellation owns the USB request; END still consumes its wire ID. */
        VhcdTxRelease(transaction);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }
    if (!NT_SUCCESS(status)) {
        /* No cancel callback can run for other Unmark results. Do not strand
         * an unlinked transaction or the PnP teardown drain will never finish. */
        VhcdFinishTransfer(transaction, status, USBD_STATUS_INVALID_PARAMETER);
        VhcdTxRelease(transaction);
        WdfRequestComplete(Request, status);
        return;
    }

    /*
     * Claim by the readable transaction identity before validating the rest
     * of END. A malformed length or inconsistent duplicate field must not
     * strand this exact request in the slot's pending list. Use the copied
     * fixed header for duplicate-field checks; the body is only passed to the
     * response handler after all framing bounds have been checked.
     */
    if (bufferSize < InputLength || header.data_length < 0 ||
        (ULONG)header.data_length > VHCD_MAX_TRANSACTION_BYTES ||
        (ULONG)header.data_length > InputLength - sizeof(header) ||
        InputLength != sizeof(header) + (ULONG)header.data_length) {
        malformedResponse = TRUE;
    } else {
        PVHCD_WIRE_REQUEST copiedInner = (PVHCD_WIRE_REQUEST)(
            (PUCHAR)&header + FIELD_OFFSET(ioctl_usbtx_header_t, type));
        responseBytes = sizeof(header) + (ULONG)header.data_length;
        if (header.result != copiedInner->Result ||
            header.data_length != copiedInner->DataLength ||
            copiedInner->DataLength < 0) {
            malformedResponse = TRUE;
        }
    }
    if (malformedResponse) {
        VhcdFinishTransfer(transaction, STATUS_INVALID_PARAMETER, USBD_STATUS_INVALID_PARAMETER);
        VhcdTxRelease(transaction);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    inner = (PVHCD_WIRE_REQUEST)(input + FIELD_OFFSET(ioctl_usbtx_header_t, type));
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
    if (IoControlCode == IOCTL_PDO_ADD || IoControlCode == IOCTL_PDO_ADD_SUPERSPEED ||
        IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED) {
        ioctl_pdo_create_t* create;
        PVOID inputBuffer;
        PVHCD_FILE_CONTEXT fileContext;
        PVHCD_USER_IO_CONTEXT ioContext = VhcdGetUserIoContext(Request);
        PKSEMAPHORE semaphore;
        PKEVENT removeEvent;
        LONG address;
        LONG suggested;
        LONG slot = -1;
        LONG i;
        BOOLEAN superSpeed = IoControlCode == IOCTL_PDO_ADD_SUPERSPEED;
        USB_DEVICE_SPEED deviceSpeed = superSpeed ? UsbSuperSpeed : UsbHighSpeed;
        size_t requiredLength = IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED
            ? sizeof(ioctl_pdo_create_with_speed_t) : sizeof(*create);
        status = WdfRequestRetrieveInputBuffer(Request, requiredLength, &inputBuffer, &length);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        create = IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED
            ? &((ioctl_pdo_create_with_speed_t*)inputBuffer)->Create
            : (ioctl_pdo_create_t*)inputBuffer;
        if (IoControlCode == IOCTL_PDO_ADD_WITH_USB_SPEED) {
            ULONG requestedSpeed = ((ioctl_pdo_create_with_speed_t*)inputBuffer)->UsbSpeed;
            if (requestedSpeed == XUSB_USB_SPEED_LOW) deviceSpeed = UsbLowSpeed;
            else if (requestedSpeed == XUSB_USB_SPEED_FULL) deviceSpeed = UsbFullSpeed;
            else { WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return; }
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(address), &buffer, &length);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        if (length < sizeof(address)) { WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return; }
        fileContext = VhcdGetFileContext(WdfRequestGetFileObject(Request));
        if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE || fileContext->SlotIndex >= 0 || ioContext == NULL || ioContext->AddSemaphore == NULL) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
        semaphore = ioContext->AddSemaphore;
        removeEvent = ioContext->AddRemoveEvent;
        WdfSpinLockAcquire(gSlotLock);
        if (gStopping) {
            WdfSpinLockRelease(gSlotLock);
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
            return;
        }
        /* A parallel ADD on the same file must not allocate a second slot. */
        if (fileContext->SlotIndex >= 0) {
            WdfSpinLockRelease(gSlotLock);
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            return;
        }
        if (superSpeed) {
            for (i = 0; i < VHCD_MAX_USB30_PORTS; ++i) {
                LONG candidate = VHCD_USB30_FIRST_SLOT_INDEX + i;
                if (!gUsb30PortPowerOff[i] && !gSlots[candidate].Allocated) {
                    slot = candidate;
                    break;
                }
            }
        } else {
            /* USB 2.0 and USB 3.0 root hubs have separate, non-aliasing port ranges. */
            for (i = 0; i < VHCD_MAX_PORTS; ++i) if (!gSlots[i].Allocated) { slot = i; break; }
        }
        suggested = create->BusAddress;
        if (suggested < 1 || suggested > 127 || gAddressInUse[suggested]) {
            suggested = 1;
            while (suggested <= 127 && gAddressInUse[suggested]) ++suggested;
        }
        if (slot >= 0 && suggested <= 127) {
            if (++gNextSlotGeneration == 0) ++gNextSlotGeneration;
            gSlots[slot].Generation = gNextSlotGeneration;
            gSlots[slot].Allocated = TRUE;
            gSlots[slot].Present = TRUE;
            gSlots[slot].Enabled = FALSE;
            gSlots[slot].EnableChanged = FALSE;
            gSlots[slot].DeviceSpeed = deviceSpeed;
            gSlots[slot].LinkState = PORT_LINK_STATE_U0;
            gSlots[slot].ConnectionChanged = TRUE;
            gSlots[slot].BusAddress = suggested;
            gSlots[slot].Semaphore = semaphore;
            gSlots[slot].RemoveEvent = removeEvent;
            gAddressInUse[suggested] = TRUE;
            InitializeListHead(&gSlots[slot].Transactions);
            fileContext->SlotIndex = slot;
            fileContext->Generation = gSlots[slot].Generation;
            ioContext->AddSemaphore = NULL;
            ioContext->AddRemoveEvent = NULL;
        } else {
            slot = -1;
        }
        WdfSpinLockRelease(gSlotLock);
        if (slot < 0) { if (removeEvent != NULL) ObDereferenceObject(removeEvent); ObDereferenceObject(semaphore); ioContext->AddSemaphore = NULL; ioContext->AddRemoveEvent = NULL; WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES); return; }
        ioContext->AddSemaphore = NULL;
        ioContext->AddRemoveEvent = NULL;
        address = (LONG)suggested;
        RtlCopyMemory(buffer, &address, sizeof(address));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(address));
        if (gRootHub != WDF_NO_HANDLE) UcxRootHubPortChanged(gRootHub);
        return;
    }
    if (IoControlCode == IOCTL_PDO_REMOVE) {
        PVHCD_FILE_CONTEXT fileContext = VhcdGetFileContext(WdfRequestGetFileObject(Request));
        LONG slot;
        ULONGLONG generation;
        if (fileContext == NULL || fileContext->Signature != VHCD_FILE_SIGNATURE) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE); return; }
        WdfSpinLockAcquire(gSlotLock);
        slot = fileContext->SlotIndex;
        generation = fileContext->Generation;
        fileContext->SlotIndex = -1;
        fileContext->Generation = 0;
        WdfSpinLockRelease(gSlotLock);
        status = VhcdReleaseSlot(slot, generation) ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
        WdfRequestComplete(Request, status);
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
    if (IoControlCode == IOCTL_XUSB_QUERY_UCX_STATE) {
        xusb_ucx_state_t* info;
        const size_t baseSize = FIELD_OFFSET(xusb_ucx_state_t, RootHubGet20PortInfoCount);
        const size_t previousExtendedSize = FIELD_OFFSET(xusb_ucx_state_t, RootHubGet30PortInfoStatus);
        const size_t portInfoSize = FIELD_OFFSET(xusb_ucx_state_t, ReleaseHardwareCount);
        const size_t lifecycleSize = FIELD_OFFSET(xusb_ucx_state_t, RootHubInvalidRequestCount);
        size_t responseSize = OutputLength >= sizeof(*info) ? sizeof(*info) :
            (OutputLength >= lifecycleSize ? lifecycleSize :
            (OutputLength >= portInfoSize ? portInfoSize :
            (OutputLength >= previousExtendedSize ? previousExtendedSize : baseSize)));
        status = WdfRequestRetrieveOutputBuffer(Request, responseSize, (PVOID*)&info, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        WdfSpinLockAcquire(gSlotLock);
        gUcxState.Stopping = gStopping;
        gUcxState.PendingTransactions = gTransactionCount;
        gUcxState.PendingRemovalWorkItems = (ULONG)gPendingRemovalWorkItems;
        RtlCopyMemory(info, &gUcxState, responseSize);
        WdfSpinLockRelease(gSlotLock);
        if (responseSize >= FIELD_OFFSET(xusb_ucx_state_t, UserQueueState) + sizeof(info->UserQueueState))
            info->UserQueueState = (ULONG)WdfIoQueueGetState(Queue, NULL, NULL);
        info->StructureSize = (ULONG)responseSize;
        info->ControllerHandlePresent = gController != WDF_NO_HANDLE;
        info->RootHubHandlePresent = gRootHub != WDF_NO_HANDLE;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, responseSize);
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
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    UCX_CONTROLLER_CONFIG controllerConfig;
    UCX_ROOTHUB_CONFIG rootHubConfig;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    status = UcxInitializeDeviceInit(DeviceInit);
    gUcxState.DeviceInitStatus = status;
    if (!NT_SUCCESS(status)) return status;
    gUcxState.StageFlags |= XUSB_UCX_STAGE_DEVICE_INIT;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, VhcdEvtFileCreate, VhcdEvtFileClose, VhcdEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, VHCD_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = VhcdEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = VhcdEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = VhcdEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = VhcdEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.EvtCleanupCallback = VhcdEvtDeviceCleanup;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, VHCD_TRANSFER_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &requestAttributes);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, VhcdEvtIoInCallerContext);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    gUcxState.WdfDeviceStatus = status;
    if (!NT_SUCCESS(status)) return status;
    gUcxState.StageFlags |= XUSB_UCX_STAGE_WDF_DEVICE;
    gDevice = device;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_XUSB_VIRT_INTERFACE, NULL);
    gUcxState.VirtInterfaceStatus = status;
    if (!NT_SUCCESS(status)) return status;
    gUcxState.StageFlags |= XUSB_UCX_STAGE_VIRT_INTERFACE;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL);
    gUcxState.HcdInterfaceStatus = status;
    if (!NT_SUCCESS(status)) return status;
    gUcxState.StageFlags |= XUSB_UCX_STAGE_HCD_INTERFACE;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = VhcdEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &gIoQueue);
    gUcxState.IoQueueStatus = status;
    if (!NT_SUCCESS(status)) return status;
    gUcxState.StageFlags |= XUSB_UCX_STAGE_IO_QUEUE;
    UCX_CONTROLLER_CONFIG_INIT(&controllerConfig, "Virtual Device Camera UCX HCD");
    controllerConfig.EvtControllerUsbDeviceAdd = VhcdEvtControllerUsbDeviceAdd;
    controllerConfig.EvtControllerQueryUsbCapability = VhcdEvtControllerQueryUsbCapability;
    controllerConfig.EvtControllerGetCurrentFrameNumber = VhcdEvtControllerGetCurrentFrameNumber;
    controllerConfig.EvtControllerReset = VhcdEvtControllerReset;
    status = UcxControllerCreate(device, &controllerConfig, WDF_NO_OBJECT_ATTRIBUTES, &gController);
    gUcxState.ControllerStatus = status;
    if (!NT_SUCCESS(status)) return status;
    gUcxState.StageFlags |= XUSB_UCX_STAGE_CONTROLLER;
    /* Use the request-specific callback form for the standard root-hub
     * controls. Do not set the generic control-URB handler at the same time. */
    UCX_ROOTHUB_CONFIG_INIT(&rootHubConfig,
        VhcdEvtRootHubControlUrb, VhcdEvtRootHubControlUrb,
        VhcdEvtRootHubControlUrb, VhcdEvtRootHubControlUrb,
        VhcdEvtRootHubControlUrb, VhcdEvtRootHubControlUrb,
        VhcdEvtRootHubControlUrb, VhcdEvtRootHubInterruptTx,
        VhcdEvtRootHubGetInfo, VhcdEvtRootHubGet20PortInfo, VhcdEvtRootHubGet30PortInfo);
    status = UcxRootHubCreate(gController, &rootHubConfig, WDF_NO_OBJECT_ATTRIBUTES, &gRootHub);
    gUcxState.RootHubStatus = status;
    if (NT_SUCCESS(status)) gUcxState.StageFlags |= XUSB_UCX_STAGE_ROOT_HUB;
    return status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    NTSTATUS status;
    RtlZeroMemory(&gUcxState, sizeof(gUcxState));
    gUcxState.Signature = XUSB_UCX_STATE_SIGNATURE;
    gUcxState.StructureSize = sizeof(gUcxState);
    gUcxState.AbiVersion = XUSB_UCX_STATE_ABI_VERSION;
    gUcxState.DeviceInitStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.WdfDeviceStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.VirtInterfaceStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.HcdInterfaceStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.IoQueueStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.ControllerStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.RootHubStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.PrepareHardwareStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.RootHubGetInfoStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.RootHubGet20PortInfoStatus = STATUS_DEVICE_NOT_READY;
    gUcxState.RootHubGet30PortInfoStatus = STATUS_DEVICE_NOT_READY;
    KeInitializeEvent(&gTransactionsDrained, NotificationEvent, TRUE);
    KeInitializeEvent(&gRemovalWorkItemsDrained, NotificationEvent, TRUE);
    WDF_DRIVER_CONFIG_INIT(&config, VhcdEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &gDriver);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = gDriver;
    status = WdfSpinLockCreate(&lockAttributes, &gSlotLock);
    if (!NT_SUCCESS(status)) return status;
    return STATUS_SUCCESS;
}

NTSTATUS VhcdEvtDeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    InterlockedIncrement((volatile LONG*)&gUcxState.D0EntryCount);
    gUcxState.CurrentPowerState = WdfPowerDeviceD0;
    return STATUS_SUCCESS;
}

NTSTATUS VhcdEvtDeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    InterlockedIncrement((volatile LONG*)&gUcxState.D0ExitCount);
    gUcxState.CurrentPowerState = (ULONG)TargetState;
    return STATUS_SUCCESS;
}

NTSTATUS VhcdEvtDevicePrepareHardware(_In_ WDFDEVICE Device,
                                      _In_ WDFCMRESLIST ResourcesRaw,
                                      _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    WdfSpinLockAcquire(gSlotLock);
    gStopping = FALSE;
    WdfSpinLockRelease(gSlotLock);
    /* ReleaseHardware purges this queue. A resource rebalance can prepare the
     * same WDF device again, so undo that explicit purge before accepting I/O. */
    if (gIoQueue != WDF_NO_HANDLE) WdfIoQueueStart(gIoQueue);
    InterlockedIncrement((volatile LONG*)&gUcxState.PrepareHardwareCount);
    gUcxState.PrepareHardwareStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS VhcdEvtDeviceReleaseHardware(_In_ WDFDEVICE Device,
                                      _In_ WDFCMRESLIST ResourcesTranslated)
{
    PKSEMAPHORE semaphores[VHCD_MAX_SLOTS] = { 0 };
    PKEVENT removeEvents[VHCD_MAX_SLOTS] = { 0 };
    LONG slot;
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    InterlockedIncrement((volatile LONG*)&gUcxState.ReleaseHardwareCount);
    WdfSpinLockAcquire(gSlotLock);
    gStopping = TRUE;
    for (slot = 0; slot < VHCD_MAX_SLOTS; ++slot) {
        gSlots[slot].Present = FALSE;
        gSlots[slot].Enabled = FALSE;
        gSlots[slot].Suspended = FALSE;
        gSlots[slot].SuspendChanged = FALSE;
        gSlots[slot].ConnectionChanged = FALSE;
        gSlots[slot].ResetChanged = FALSE;
        gSlots[slot].ResetInProgress = FALSE;
        gSlots[slot].BHResetChanged = FALSE;
        gSlots[slot].LinkStateChanged = FALSE;
        if (gSlots[slot].Allocated) gSlots[slot].RemoveRequested = TRUE;
        if (slot < VHCD_MAX_PORTS) gPortPowerOff[slot] = FALSE;
    }
    RtlZeroMemory(gUsb30PortPowerOff, sizeof(gUsb30PortPowerOff));
    WdfSpinLockRelease(gSlotLock);

    /* Stop client requests first; then drain all URB and synthetic reset transactions. */
    if (gIoQueue != WDF_NO_HANDLE) WdfIoQueuePurgeSynchronously(gIoQueue);
    for (slot = 0; slot < VHCD_MAX_SLOTS; ++slot) {
        ULONGLONG generation;
        WdfSpinLockAcquire(gSlotLock);
        generation = gSlots[slot].Generation;
        WdfSpinLockRelease(gSlotLock);
        VhcdCancelSlotTransactions(slot, generation, 0, TRUE, FALSE);
    }
    (VOID)KeWaitForSingleObject(&gTransactionsDrained, Executive, KernelMode, FALSE, NULL);
    /* The final transaction can signal the event from its completion DPC. */
    KeFlushQueuedDpcs();
    /* Do not let a driver-owned work-item callback outlive PnP teardown. */
    (VOID)KeWaitForSingleObject(&gRemovalWorkItemsDrained, Executive, KernelMode, FALSE, NULL);

    /*
     * Notify clients while PnP is still in ReleaseHardware at PASSIVE_LEVEL.
     * EvtCleanupCallback can run at DISPATCH_LEVEL; keep user-event signaling
     * out of that callback and transfer each slot's references under the lock.
     */
    for (slot = 0; slot < VHCD_MAX_SLOTS; ++slot) {
        WdfSpinLockAcquire(gSlotLock);
        if (gSlots[slot].Allocated) {
            semaphores[slot] = gSlots[slot].Semaphore;
            removeEvents[slot] = gSlots[slot].RemoveEvent;
            if (gSlots[slot].BusAddress > 0 && gSlots[slot].BusAddress <= 127)
                gAddressInUse[gSlots[slot].BusAddress] = FALSE;
            RtlZeroMemory(&gSlots[slot], sizeof(gSlots[slot]));
        }
        WdfSpinLockRelease(gSlotLock);
        if (removeEvents[slot] != NULL) {
            KeSetEvent(removeEvents[slot], IO_NO_INCREMENT, FALSE);
            ObDereferenceObject(removeEvents[slot]);
        }
        if (semaphores[slot] != NULL) ObDereferenceObject(semaphores[slot]);
    }
    InterlockedIncrement((volatile LONG*)&gUcxState.ReleaseHardwareCompletedCount);
    return STATUS_SUCCESS;
}

VOID VhcdEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    PKSEMAPHORE semaphores[VHCD_MAX_SLOTS] = { 0 };
    PKEVENT removeEvents[VHCD_MAX_SLOTS] = { 0 };
    LONG slot;
    UNREFERENCED_PARAMETER(Object);

    /* ReleaseHardware drains slots and notifies clients before device cleanup. */
    WdfSpinLockAcquire(gSlotLock);
    for (slot = 0; slot < VHCD_MAX_SLOTS; ++slot) {
        semaphores[slot] = gSlots[slot].Semaphore;
        removeEvents[slot] = gSlots[slot].RemoveEvent;
        RtlZeroMemory(&gSlots[slot], sizeof(gSlots[slot]));
        if (slot < VHCD_MAX_PORTS) gPortPowerOff[slot] = FALSE;
    }
    RtlZeroMemory(gUsb30PortPowerOff, sizeof(gUsb30PortPowerOff));
    RtlZeroMemory(gAddressInUse, sizeof(gAddressInUse));
    gDevice = WDF_NO_HANDLE;
    gIoQueue = WDF_NO_HANDLE;
    gController = WDF_NO_HANDLE;
    gRootHub = WDF_NO_HANDLE;
    gStopping = TRUE;
    WdfSpinLockRelease(gSlotLock);

    for (slot = 0; slot < VHCD_MAX_SLOTS; ++slot) {
        /* PnP normally detached and notified these slots in ReleaseHardware. */
        if (removeEvents[slot] != NULL) ObDereferenceObject(removeEvents[slot]);
        if (semaphores[slot] != NULL) ObDereferenceObject(semaphores[slot]);
    }
}

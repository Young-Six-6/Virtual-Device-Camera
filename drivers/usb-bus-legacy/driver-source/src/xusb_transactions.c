#include <ntddk.h>
#include <usb.h>
#include <usbioctl.h>
#include "xusb_driver.h"

#define XUSB_MAX_TRANSACTION_BYTES (32u * 1024u * 1024u)
#define XUSB_MAX_QUEUED_TRANSACTIONS 256
#define XUSB_MAX_QUEUED_BYTES (64ll * 1024ll * 1024ll)
#define XUSB_MAX_PARENT_QUEUED_TRANSACTIONS 1024
#define XUSB_MAX_PARENT_QUEUED_BYTES (256ll * 1024ll * 1024ll)
#define XUSB_TRANSACTION_TAG 'tUXV'

typedef struct _XUSB_TRANSACTION {
    volatile LONG References;
    IO_CSQ_IRP_CONTEXT CsqContext;
    PXUSB_CHILD_EXTENSION Child;
    PIRP Irp;
    PVOID SavedDriverContext0;
    PKSEMAPHORE Semaphore;
    PURB Urb;
    ULONGLONG Id;
    LONG Sequence;
    ULONG PayloadCapacity;
    ULONG TransferLength;
    ULONG IsoMetadataLength;
    ULONG PacketCount;
    USHORT UrbFunction;
    ULONG PortIoControlCode;
    PVOID TransferBuffer;
    PMDL TransferMdl;
    BOOLEAN IsRead;
    BOOLEAN IsIsochronous;
    ULONG BufferBytes;
    LONG64 ReservedBytes;
    PUCHAR Buffer;
} XUSB_TRANSACTION, *PXUSB_TRANSACTION;

#pragma pack(push, 1)
typedef struct _XUSB_USER_TRANSFER {
    LONG Type;
    LONG EndpointAddress;
    LONG IsRead;
    union {
        struct { LONG NumberPackets, Reserved0; CHAR IsSplit, Reserved[3]; };
        struct { UCHAR SetupPacket[8]; ULONG Timeout; };
    };
} XUSB_USER_TRANSFER, *PXUSB_USER_TRANSFER;

typedef struct _XUSB_USER_TRANSACTION {
    LONG Type;
    LONG Result;
    LONG DataLength;
    LONG Reserved;
    union {
        UCHAR RequestFields[24];
        XUSB_USER_TRANSFER Transfer;
        struct { LONG Type, Subtype, IsRead, Request, Index, Value; } Vendor;
        struct { LONG Type, Index; } Status;
        struct { LONG Type, Subtype, Index, Value; } Feature;
        struct { LONG Type, EndpointAddress; } Reset;
        struct { LONG Type, Subtype, IsRead, Index, Value; } Descriptor;
    };
    UCHAR Data[ANYSIZE_ARRAY];
} XUSB_USER_TRANSACTION, *PXUSB_USER_TRANSACTION;
#pragma pack(pop)

C_ASSERT(FIELD_OFFSET(XUSB_USER_TRANSACTION, Data) == 40);
C_ASSERT(sizeof(XUSB_USER_TRANSFER) == 24);

typedef struct _XUSB_END_MATCH {
    ULONGLONG Id;
    LONG Sequence;
} XUSB_END_MATCH, *PXUSB_END_MATCH;

static PXUSB_TRANSACTION XusbGetTransaction(_In_ PIRP Irp)
{
    return (PXUSB_TRANSACTION)Irp->Tail.Overlay.DriverContext[0];
}

static NTSTATUS XusbReserveTransactionResources(
    _Inout_ PXUSB_CHILD_EXTENSION Child,
    _In_ LONG64 Reservation)
{
    PXUSB_PARENT_EXTENSION parent =
        (PXUSB_PARENT_EXTENSION)Child->ParentDevice->DeviceExtension;
    LONG childCount = InterlockedCompareExchange(&Child->TransactionCount, 0, 0);
    LONG64 childBytes;
    LONG parentCount;
    LONG64 parentBytes;

    for (;;) {
        LONG previous;
        if (childCount >= XUSB_MAX_QUEUED_TRANSACTIONS) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        previous = InterlockedCompareExchange(
            &Child->TransactionCount,
            childCount + 1,
            childCount);
        if (previous == childCount) {
            break;
        }
        childCount = previous;
    }

    childBytes = InterlockedCompareExchange64(&Child->TransactionBytes, 0, 0);
    for (;;) {
        LONG64 previous;
        if (childBytes > XUSB_MAX_QUEUED_BYTES - Reservation) {
            InterlockedDecrement(&Child->TransactionCount);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        previous = InterlockedCompareExchange64(
            &Child->TransactionBytes,
            childBytes + Reservation,
            childBytes);
        if (previous == childBytes) {
            break;
        }
        childBytes = previous;
    }

    parentCount = InterlockedCompareExchange(&parent->TransactionCount, 0, 0);
    for (;;) {
        LONG previous;
        if (parentCount >= XUSB_MAX_PARENT_QUEUED_TRANSACTIONS) {
            InterlockedAdd64(&Child->TransactionBytes, -Reservation);
            InterlockedDecrement(&Child->TransactionCount);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        previous = InterlockedCompareExchange(
            &parent->TransactionCount,
            parentCount + 1,
            parentCount);
        if (previous == parentCount) {
            break;
        }
        parentCount = previous;
    }

    parentBytes = InterlockedCompareExchange64(&parent->TransactionBytes, 0, 0);
    for (;;) {
        LONG64 previous;
        if (parentBytes > XUSB_MAX_PARENT_QUEUED_BYTES - Reservation) {
            InterlockedDecrement(&parent->TransactionCount);
            InterlockedAdd64(&Child->TransactionBytes, -Reservation);
            InterlockedDecrement(&Child->TransactionCount);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        previous = InterlockedCompareExchange64(
            &parent->TransactionBytes,
            parentBytes + Reservation,
            parentBytes);
        if (previous == parentBytes) {
            break;
        }
        parentBytes = previous;
    }
    return STATUS_SUCCESS;
}

static VOID XusbReleaseTransactionResources(
    _Inout_ PXUSB_CHILD_EXTENSION Child,
    _In_ LONG64 Reservation)
{
    PXUSB_PARENT_EXTENSION parent =
        (PXUSB_PARENT_EXTENSION)Child->ParentDevice->DeviceExtension;
    InterlockedAdd64(&Child->TransactionBytes, -Reservation);
    InterlockedDecrement(&Child->TransactionCount);
    InterlockedAdd64(&parent->TransactionBytes, -Reservation);
    InterlockedDecrement(&parent->TransactionCount);
}

static VOID XusbCompleteUsbIrp(
    _In_ PXUSB_TRANSACTION Transaction,
    _In_ NTSTATUS Status)
{
    PIRP irp = Transaction->Irp;
    PXUSB_CHILD_EXTENSION child = Transaction->Child;
    irp->Tail.Overlay.DriverContext[0] = Transaction->SavedDriverContext0;
    IoReleaseRemoveLock(&child->RemoveLock, irp);
    irp->IoStatus.Status = Status;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static VOID XusbReleaseTransaction(_In_opt_ PXUSB_TRANSACTION Transaction)
{
    if (Transaction != NULL && InterlockedDecrement(&Transaction->References) == 0) {
        if (Transaction->Buffer != NULL) {
            ExFreePoolWithTag(Transaction->Buffer, XUSB_TRANSACTION_TAG);
        }
        if (Transaction->Semaphore != NULL) {
            ObDereferenceObject(Transaction->Semaphore);
        }
        if (Transaction->Child != NULL) {
            XusbReleaseTransactionResources(
                Transaction->Child,
                Transaction->ReservedBytes);
            ObDereferenceObject(Transaction->Child->Self);
        }
        ExFreePoolWithTag(Transaction, XUSB_TRANSACTION_TAG);
    }
}

static VOID XusbCsqInsertIrp(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(Csq, XUSB_CHILD_EXTENSION, TransactionCsq);
    InsertTailList(&child->TransactionQueue, &Irp->Tail.Overlay.ListEntry);
}

static VOID XusbCsqRemoveIrp(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    InitializeListHead(&Irp->Tail.Overlay.ListEntry);
}

static PIRP XusbCsqPeekNextIrp(
    _In_ PIO_CSQ Csq,
    _In_opt_ PIRP Irp,
    _In_opt_ PVOID PeekContext)
{
    PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(Csq, XUSB_CHILD_EXTENSION, TransactionCsq);
    PLIST_ENTRY entry;
    PXUSB_END_MATCH match = (PXUSB_END_MATCH)PeekContext;

    entry = (Irp == NULL) ? child->TransactionQueue.Flink : Irp->Tail.Overlay.ListEntry.Flink;
    while (entry != &child->TransactionQueue) {
        PIRP candidate = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        PXUSB_TRANSACTION transaction = XusbGetTransaction(candidate);
        if (match == NULL || (transaction != NULL && transaction->Id == match->Id &&
            transaction->Sequence == match->Sequence)) {
            return candidate;
        }
        entry = entry->Flink;
    }
    return NULL;
}

_IRQL_raises_(DISPATCH_LEVEL)
static VOID XusbCsqAcquireLock(_In_ PIO_CSQ Csq, _Out_ PKIRQL Irql)
{
    PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(Csq, XUSB_CHILD_EXTENSION, TransactionCsq);
    KeAcquireSpinLock(&child->TransactionLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_restores_
#pragma warning(push)
#pragma warning(disable: 28157) // IoCsq pairs this callback with the matching acquire routine and saved IRQL.
static VOID XusbCsqReleaseLock(_In_ PIO_CSQ Csq, _In_ KIRQL Irql)
{
    PXUSB_CHILD_EXTENSION child = CONTAINING_RECORD(Csq, XUSB_CHILD_EXTENSION, TransactionCsq);
    KeReleaseSpinLock(&child->TransactionLock, Irql);
}
#pragma warning(pop)

static VOID XusbCsqCompleteCancelledIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    PXUSB_TRANSACTION transaction = XusbGetTransaction(Irp);
    UNREFERENCED_PARAMETER(Csq);
    if (transaction != NULL && transaction->Urb != NULL) {
        transaction->Urb->UrbHeader.Status = USBD_STATUS_CANCELED;
    }
    if (transaction != NULL) {
        XusbCompleteUsbIrp(transaction, STATUS_CANCELLED);
    } else {
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    XusbReleaseTransaction(transaction);
}

static NTSTATUS XusbPopulateInterface(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR Configuration,
    _In_ ULONG ConfigurationLength,
    _Inout_ PUSBD_INTERFACE_INFORMATION Interface,
    _In_ ULONG InterfaceBytes,
    _In_ ULONG HandleGeneration,
    _Inout_updates_(XUSB_MAX_PIPES) XUSB_PIPE_ENTRY* Pipes,
    _Inout_ PULONG PipeCount)
{
    ULONG requiredBytes;
    ULONG offset = 0;
    PUSB_INTERFACE_DESCRIPTOR descriptor = NULL;
    ULONG foundPipes = 0;
    PUCHAR cursor;

    if (Interface->Length < FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) ||
        Interface->Length > InterfaceBytes ||
        Interface->NumberOfPipes > XUSB_MAX_PIPES ||
        *PipeCount > XUSB_MAX_PIPES - Interface->NumberOfPipes) {
        return STATUS_INVALID_PARAMETER;
    }
    requiredBytes = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) +
        (Interface->NumberOfPipes * sizeof(USBD_PIPE_INFORMATION));
    if (Interface->Length < requiredBytes) {
        return STATUS_INVALID_PARAMETER;
    }

    while (offset + sizeof(USB_COMMON_DESCRIPTOR) <= ConfigurationLength) {
        PUSB_COMMON_DESCRIPTOR current = (PUSB_COMMON_DESCRIPTOR)((PUCHAR)Configuration + offset);
        if (current->bLength < sizeof(USB_COMMON_DESCRIPTOR) ||
            current->bLength > ConfigurationLength - offset) {
            return STATUS_INVALID_PARAMETER;
        }
        if (current->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE) {
            if (current->bLength < sizeof(USB_INTERFACE_DESCRIPTOR)) {
                return STATUS_INVALID_PARAMETER;
            }
            PUSB_INTERFACE_DESCRIPTOR candidate = (PUSB_INTERFACE_DESCRIPTOR)current;
            if (candidate->bInterfaceNumber == Interface->InterfaceNumber &&
                candidate->bAlternateSetting == Interface->AlternateSetting) {
                descriptor = candidate;
                break;
            }
        }
        offset += current->bLength;
    }
    if (descriptor == NULL || descriptor->bNumEndpoints != Interface->NumberOfPipes) {
        return STATUS_NO_SUCH_DEVICE;
    }

    cursor = (PUCHAR)descriptor + descriptor->bLength;
    while (cursor < (PUCHAR)Configuration + ConfigurationLength &&
        foundPipes < descriptor->bNumEndpoints) {
        PUSB_COMMON_DESCRIPTOR current = (PUSB_COMMON_DESCRIPTOR)cursor;
        ULONG descriptorOffset = (ULONG)(cursor - (PUCHAR)Configuration);
        if (current->bLength < sizeof(USB_COMMON_DESCRIPTOR) ||
            current->bLength > ConfigurationLength - descriptorOffset ||
            current->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE) {
            return STATUS_INVALID_PARAMETER;
        }
        if (current->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE) {
            if (current->bLength < sizeof(USB_ENDPOINT_DESCRIPTOR)) {
                return STATUS_INVALID_PARAMETER;
            }
            PUSB_ENDPOINT_DESCRIPTOR endpoint = (PUSB_ENDPOINT_DESCRIPTOR)current;
            PUSBD_PIPE_INFORMATION pipe = &Interface->Pipes[foundPipes];
            ULONG_PTR handle;
            USHORT maximumPacket = endpoint->wMaxPacketSize;
            UCHAR pipeType = endpoint->bmAttributes & 3;

            if (pipeType == UsbdPipeTypeIsochronous) {
                maximumPacket = (USHORT)((((maximumPacket >> 11) & 3) + 1) *
                    (maximumPacket & 0x7ff));
            }
            handle = ((ULONG_PTR)HandleGeneration << 24) |
                ((ULONG_PTR)endpoint->bInterval << 16) |
                ((ULONG_PTR)pipeType << 8) | endpoint->bEndpointAddress;
            if (handle == 0) {
                return STATUS_INVALID_PARAMETER;
            }
            pipe->MaximumPacketSize = maximumPacket;
            pipe->EndpointAddress = endpoint->bEndpointAddress;
            pipe->Interval = endpoint->bInterval;
            pipe->PipeType = (USBD_PIPE_TYPE)pipeType;
            pipe->PipeHandle = (USBD_PIPE_HANDLE)handle;
            Pipes[*PipeCount].Handle = handle;
            Pipes[*PipeCount].InterfaceNumber = Interface->InterfaceNumber;
            Pipes[*PipeCount].EndpointAddress = endpoint->bEndpointAddress;
            Pipes[*PipeCount].Interval = endpoint->bInterval;
            Pipes[*PipeCount].PipeType = pipeType;
            Pipes[*PipeCount].MaximumPacketSize = maximumPacket;
            Pipes[*PipeCount].Reserved = 0;
            ++*PipeCount;
            ++foundPipes;
        }
        cursor += current->bLength;
    }
    if (foundPipes != descriptor->bNumEndpoints) {
        return STATUS_INVALID_PARAMETER;
    }
    Interface->Class = descriptor->bInterfaceClass;
    Interface->SubClass = descriptor->bInterfaceSubClass;
    Interface->Protocol = descriptor->bInterfaceProtocol;
    Interface->Reserved = 0;
    Interface->InterfaceHandle = (USBD_INTERFACE_HANDLE)(
        ((ULONG_PTR)HandleGeneration << 8) | ((ULONG_PTR)Interface->InterfaceNumber + 1));
    return STATUS_SUCCESS;
}

static ULONG XusbNextHandleGeneration(_Inout_ PXUSB_CHILD_EXTENSION Child)
{
    ++Child->HandleGeneration;
    if (Child->HandleGeneration == 0) {
        ++Child->HandleGeneration;
    }
    return Child->HandleGeneration;
}

static NTSTATUS XusbSelectConfiguration(
    _Inout_ PXUSB_CHILD_EXTENSION Child,
    _Inout_ PURB Urb)
{
    PUSB_CONFIGURATION_DESCRIPTOR source = Urb->UrbSelectConfiguration.ConfigurationDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR configuration = NULL;
    PUSB_CONFIGURATION_DESCRIPTOR oldConfiguration;
    ULONG configurationLength = 0;
    ULONG_PTR configurationHandle = 0;
    PXUSB_PIPE_ENTRY pipes;
    ULONG pipeCount = 0;
    ULONG configurationValue = 0;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL oldIrql;

    pipes = ExAllocatePool2(POOL_FLAG_PAGED, XUSB_MAX_PIPES * sizeof(*pipes), 'pUXV');
    if (pipes == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(pipes, XUSB_MAX_PIPES * sizeof(*pipes));
    ExAcquireFastMutex(&Child->ConfigurationMutex);
    if (source != NULL) {
        USB_CONFIGURATION_DESCRIPTOR header;
        __try {
            RtlCopyMemory(&header, source, sizeof(header));
        }
        __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
            GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
            status = GetExceptionCode();
            goto Exit;
        }
        configurationLength = header.wTotalLength;
        if (header.bLength < sizeof(header) ||
            header.bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE ||
            configurationLength < header.bLength || configurationLength > 65535) {
            status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }
        configuration = ExAllocatePool2(POOL_FLAG_PAGED, configurationLength, 'cUXV');
        if (configuration == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        __try {
            RtlCopyMemory(configuration, source, configurationLength);
        }
        __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
            GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
            status = GetExceptionCode();
            goto Exit;
        }
        if (configuration->bLength != header.bLength ||
            configuration->wTotalLength != header.wTotalLength ||
            configuration->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE) {
            status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }
        configurationValue = configuration->bConfigurationValue;
        configurationHandle = ((ULONG_PTR)XusbNextHandleGeneration(Child) << 16) |
            configurationValue;

        {
            ULONG interfaceOffset = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
            ULONG cursor = interfaceOffset;
            while (cursor < Urb->UrbHeader.Length) {
                PUSBD_INTERFACE_INFORMATION interfaceInfo;
                ULONG remaining = Urb->UrbHeader.Length - cursor;
                if (remaining < (ULONG)FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes)) {
                    status = STATUS_INVALID_PARAMETER;
                    goto Exit;
                }
                interfaceInfo = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)Urb + cursor);
                if (interfaceInfo->Length == 0 || interfaceInfo->Length > remaining) {
                    status = STATUS_INVALID_PARAMETER;
                    goto Exit;
                }
                status = XusbPopulateInterface(
                    configuration,
                    configurationLength,
                    interfaceInfo,
                    remaining,
                    XusbNextHandleGeneration(Child),
                    pipes,
                    &pipeCount);
                if (!NT_SUCCESS(status)) {
                    goto Exit;
                }
                cursor += interfaceInfo->Length;
            }
            if (cursor != Urb->UrbHeader.Length) {
                status = STATUS_INVALID_PARAMETER;
                goto Exit;
            }
        }
        Urb->UrbSelectConfiguration.ConfigurationHandle =
            (USBD_CONFIGURATION_HANDLE)configurationHandle;
    } else {
        Urb->UrbSelectConfiguration.ConfigurationHandle = NULL;
    }

    KeAcquireSpinLock(&Child->TransactionLock, &oldIrql);
    oldConfiguration = (PUSB_CONFIGURATION_DESCRIPTOR)Child->ConfigurationDescriptor;
    Child->ConfigurationDescriptor = configuration;
    Child->ConfigurationLength = configurationLength;
    Child->ConfigurationHandle = configurationHandle;
    Child->PipeCount = pipeCount;
    RtlCopyMemory(Child->Pipes, pipes, pipeCount * sizeof(pipes[0]));
    KeReleaseSpinLock(&Child->TransactionLock, oldIrql);
    configuration = NULL;
    if (oldConfiguration != NULL) {
        ExFreePoolWithTag(oldConfiguration, 'cUXV');
    }

Exit:
    ExReleaseFastMutex(&Child->ConfigurationMutex);
    if (configuration != NULL) {
        ExFreePoolWithTag(configuration, 'cUXV');
    }
    ExFreePoolWithTag(pipes, 'pUXV');
    return status;
}

static NTSTATUS XusbSelectInterface(
    _Inout_ PXUSB_CHILD_EXTENSION Child,
    _Inout_ PURB Urb)
{
    PUSB_CONFIGURATION_DESCRIPTOR configuration;
    PUSBD_INTERFACE_INFORMATION interfaceInfo = &Urb->UrbSelectInterface.Interface;
    PXUSB_PIPE_ENTRY pipes;
    ULONG pipeCount = 0;
    ULONG index;
    ULONG writeIndex = 0;
    KIRQL oldIrql;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    pipes = ExAllocatePool2(POOL_FLAG_PAGED, XUSB_MAX_PIPES * sizeof(*pipes), 'pUXV');
    if (pipes == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExAcquireFastMutex(&Child->ConfigurationMutex);
    configuration = (PUSB_CONFIGURATION_DESCRIPTOR)Child->ConfigurationDescriptor;
    if (configuration == NULL || Child->ConfigurationLength < sizeof(*configuration) ||
        (ULONG_PTR)Urb->UrbSelectInterface.ConfigurationHandle != Child->ConfigurationHandle ||
        Child->ConfigurationHandle == 0) {
        ExReleaseFastMutex(&Child->ConfigurationMutex);
        ExFreePoolWithTag(pipes, 'pUXV');
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    KeAcquireSpinLock(&Child->TransactionLock, &oldIrql);
    for (index = 0; index < Child->PipeCount; ++index) {
        if (Child->Pipes[index].InterfaceNumber != interfaceInfo->InterfaceNumber) {
            pipes[writeIndex++] = Child->Pipes[index];
        }
    }
    KeReleaseSpinLock(&Child->TransactionLock, oldIrql);
    pipeCount = writeIndex;
    status = XusbPopulateInterface(
        configuration,
        Child->ConfigurationLength,
        interfaceInfo,
        Urb->UrbHeader.Length - FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface),
        XusbNextHandleGeneration(Child),
        pipes,
        &pipeCount);
    if (NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Child->TransactionLock, &oldIrql);
        Child->PipeCount = pipeCount;
        RtlCopyMemory(Child->Pipes, pipes, pipeCount * sizeof(pipes[0]));
        KeReleaseSpinLock(&Child->TransactionLock, oldIrql);
    }
    ExReleaseFastMutex(&Child->ConfigurationMutex);
    ExFreePoolWithTag(pipes, 'pUXV');
    return status;
}

static NTSTATUS XusbResolvePipe(
    _In_ PXUSB_CHILD_EXTENSION Child,
    _In_ USBD_PIPE_HANDLE Handle,
    _Out_ PXUSB_PIPE_ENTRY Pipe)
{
    ULONG index;
    ULONG_PTR key = (ULONG_PTR)Handle;
    KIRQL oldIrql;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&Child->TransactionLock, &oldIrql);
    for (index = 0; index < Child->PipeCount; ++index) {
        if (Child->Pipes[index].Handle == key) {
            *Pipe = Child->Pipes[index];
            status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&Child->TransactionLock, oldIrql);
    return status;
}

VOID XusbReleaseChildConfiguration(_Inout_ PXUSB_CHILD_EXTENSION Child)
{
    PUSB_CONFIGURATION_DESCRIPTOR configuration;
    KIRQL oldIrql;

    ExAcquireFastMutex(&Child->ConfigurationMutex);
    KeAcquireSpinLock(&Child->TransactionLock, &oldIrql);
    configuration = (PUSB_CONFIGURATION_DESCRIPTOR)Child->ConfigurationDescriptor;
    Child->ConfigurationDescriptor = NULL;
    Child->ConfigurationLength = 0;
    Child->ConfigurationHandle = 0;
    Child->PipeCount = 0;
    RtlZeroMemory(Child->Pipes, sizeof(Child->Pipes));
    KeReleaseSpinLock(&Child->TransactionLock, oldIrql);
    ExReleaseFastMutex(&Child->ConfigurationMutex);
    if (configuration != NULL) {
        ExFreePoolWithTag(configuration, 'cUXV');
    }
}

NTSTATUS XusbInitializeTransactions(_Inout_ PXUSB_CHILD_EXTENSION Child)
{
    KeInitializeSpinLock(&Child->TransactionLock);
    InitializeListHead(&Child->TransactionQueue);
    Child->NextTransactionId = 0;
    Child->NextSequence = 0;
    Child->TransactionStopping = 0;
    Child->TransactionCount = 0;
    Child->TransactionBytes = 0;
    return IoCsqInitialize(
        &Child->TransactionCsq,
        XusbCsqInsertIrp,
        XusbCsqRemoveIrp,
        XusbCsqPeekNextIrp,
        XusbCsqAcquireLock,
        XusbCsqReleaseLock,
        XusbCsqCompleteCancelledIrp);
}

#pragma warning(push)
#pragma warning(disable: 6014) // Result transfers ownership to the caller, which releases it on every terminal path.
static NTSTATUS XusbBuildTransaction(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ PURB Urb,
    _Outptr_ PXUSB_TRANSACTION* Result)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    USHORT function;
    ULONG length = 0;
    ULONG payloadLength = 0;
    ULONG packetCount = 0;
    ULONG isoMetadataLength = 0;
    LONG64 reservation;
    PXUSB_TRANSACTION transaction;
    PVOID transferBuffer = NULL;
    PMDL transferMdl = NULL;
    BOOLEAN isRead = FALSE;
    XUSB_PIPE_ENTRY pipe;
    NTSTATUS status;

    RtlZeroMemory(&pipe, sizeof(pipe));

    function = Urb->UrbHeader.Function;
    switch (function) {
    case URB_FUNCTION_CONTROL_TRANSFER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlTransfer)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlTransfer.TransferBufferLength;
        transferBuffer = Urb->UrbControlTransfer.TransferBuffer;
        transferMdl = Urb->UrbControlTransfer.TransferBufferMDL;
        isRead = (Urb->UrbControlTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        break;

    case URB_FUNCTION_CONTROL_TRANSFER_EX:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlTransferEx)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlTransferEx.TransferBufferLength;
        transferBuffer = Urb->UrbControlTransferEx.TransferBuffer;
        transferMdl = Urb->UrbControlTransferEx.TransferBufferMDL;
        isRead = (Urb->UrbControlTransferEx.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        break;

    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlDescriptorRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlDescriptorRequest.TransferBufferLength;
        transferBuffer = Urb->UrbControlDescriptorRequest.TransferBuffer;
        transferMdl = Urb->UrbControlDescriptorRequest.TransferBufferMDL;
        isRead = function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ||
            function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT ||
            function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE;
        break;

    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlVendorClassRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlVendorClassRequest.TransferBufferLength;
        transferBuffer = Urb->UrbControlVendorClassRequest.TransferBuffer;
        transferMdl = Urb->UrbControlVendorClassRequest.TransferBufferMDL;
        isRead = (Urb->UrbControlVendorClassRequest.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        break;

    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlGetStatusRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlGetStatusRequest.TransferBufferLength;
        if (length < sizeof(USHORT)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        transferBuffer = Urb->UrbControlGetStatusRequest.TransferBuffer;
        transferMdl = Urb->UrbControlGetStatusRequest.TransferBufferMDL;
        isRead = TRUE;
        break;

    case URB_FUNCTION_GET_CONFIGURATION:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlGetConfigurationRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlGetConfigurationRequest.TransferBufferLength;
        if (length < sizeof(UCHAR)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        transferBuffer = Urb->UrbControlGetConfigurationRequest.TransferBuffer;
        transferMdl = Urb->UrbControlGetConfigurationRequest.TransferBufferMDL;
        isRead = TRUE;
        break;

    case URB_FUNCTION_GET_INTERFACE:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlGetInterfaceRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbControlGetInterfaceRequest.TransferBufferLength;
        if (length < sizeof(UCHAR)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        transferBuffer = Urb->UrbControlGetInterfaceRequest.TransferBuffer;
        transferMdl = Urb->UrbControlGetInterfaceRequest.TransferBufferMDL;
        isRead = TRUE;
        break;

    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbOSFeatureDescriptorRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength;
        if (length < sizeof(UCHAR)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        transferBuffer = Urb->UrbOSFeatureDescriptorRequest.TransferBuffer;
        transferMdl = Urb->UrbOSFeatureDescriptorRequest.TransferBufferMDL;
        isRead = TRUE;
        break;

    case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbGetCurrentFrameNumber)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = sizeof(Urb->UrbGetCurrentFrameNumber.FrameNumber);
        transferBuffer = &Urb->UrbGetCurrentFrameNumber.FrameNumber;
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
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbControlFeatureRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        break;

    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbPipeRequest)) {
            return STATUS_INVALID_PARAMETER;
        }
        status = XusbResolvePipe(child, Urb->UrbPipeRequest.PipeHandle, &pipe);
        if (!NT_SUCCESS(status)) {
            return STATUS_INVALID_PARAMETER;
        }
        break;

    case URB_FUNCTION_SELECT_CONFIGURATION:
        if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface)) {
            return STATUS_INVALID_PARAMETER;
        }
        break;

    case URB_FUNCTION_SELECT_INTERFACE:
        if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) +
            FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes)) {
            return STATUS_INVALID_PARAMETER;
        }
        break;

    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        if (Urb->UrbHeader.Length < sizeof(Urb->UrbBulkOrInterruptTransfer)) {
            return STATUS_INVALID_PARAMETER;
        }
        status = XusbResolvePipe(child, Urb->UrbBulkOrInterruptTransfer.PipeHandle, &pipe);
        if (!NT_SUCCESS(status) || (pipe.PipeType != UsbdPipeTypeBulk &&
            pipe.PipeType != UsbdPipeTypeInterrupt)) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        transferBuffer = Urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        transferMdl = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;
        isRead = (Urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        break;

    case URB_FUNCTION_ISOCH_TRANSFER:
        if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket)) {
            return STATUS_INVALID_PARAMETER;
        }
        status = XusbResolvePipe(child, Urb->UrbIsochronousTransfer.PipeHandle, &pipe);
        if (!NT_SUCCESS(status) || pipe.PipeType != UsbdPipeTypeIsochronous) {
            return STATUS_INVALID_PARAMETER;
        }
        length = Urb->UrbIsochronousTransfer.TransferBufferLength;
        transferBuffer = Urb->UrbIsochronousTransfer.TransferBuffer;
        transferMdl = Urb->UrbIsochronousTransfer.TransferBufferMDL;
        isRead = (Urb->UrbIsochronousTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        packetCount = Urb->UrbIsochronousTransfer.NumberOfPackets;
        if (packetCount > (MAXULONG - ISO_PACKET_HDR_SIZE) / sizeof(iso_packet_t) ||
            Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
                packetCount * sizeof(USBD_ISO_PACKET_DESCRIPTOR)) {
            return STATUS_INVALID_PARAMETER;
        }
        isoMetadataLength = ISO_PACKET_HDR_SIZE + packetCount * sizeof(iso_packet_t);
        if (length > XUSB_MAX_TRANSACTION_BYTES - isoMetadataLength) {
            return STATUS_INVALID_PARAMETER;
        }
        break;

    default:
        return STATUS_NOT_SUPPORTED;
    }

    if (length > XUSB_MAX_TRANSACTION_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }
    payloadLength = length + isoMetadataLength;
    if (transferMdl != NULL) {
        if (MmGetMdlByteCount(transferMdl) < length) {
            return STATUS_INVALID_USER_BUFFER;
        }
        transferBuffer = MmGetSystemAddressForMdlSafe(
            transferMdl,
            NormalPagePriority | MdlMappingNoExecute);
        if (transferBuffer == NULL && length != 0) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    } else if (length != 0 && (transferBuffer == NULL || Irp->RequestorMode != KernelMode ||
        KeGetCurrentIrql() > APC_LEVEL)) {
        return STATUS_INVALID_USER_BUFFER;
    }

    reservation = (LONG64)sizeof(*transaction) + sizeof(ioctl_usbtx_header_t) + payloadLength;
    status = XusbReserveTransactionResources(child, reservation);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    transaction = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*transaction), XUSB_TRANSACTION_TAG);
    if (transaction == NULL) {
        XusbReleaseTransactionResources(child, reservation);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(transaction, sizeof(*transaction));
    transaction->References = 1;
    transaction->Child = child;
    transaction->ReservedBytes = reservation;
    ObReferenceObject(DeviceObject);
    transaction->Buffer = ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(ioctl_usbtx_header_t) + payloadLength,
        XUSB_TRANSACTION_TAG);
    if (transaction->Buffer == NULL) {
        XusbReleaseTransaction(transaction);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    transaction->Irp = Irp;
    transaction->SavedDriverContext0 = Irp->Tail.Overlay.DriverContext[0];
    transaction->Urb = Urb;
    transaction->UrbFunction = function;
    transaction->Id = (ULONGLONG)InterlockedIncrement64(&child->NextTransactionId);
    transaction->Sequence = InterlockedIncrement(&child->NextSequence);
    transaction->PayloadCapacity = payloadLength;
    transaction->TransferLength = length;
    transaction->IsoMetadataLength = isoMetadataLength;
    transaction->PacketCount = packetCount;
    transaction->TransferBuffer = transferBuffer;
    transaction->TransferMdl = transferMdl;
    transaction->IsRead = isRead;
    transaction->IsIsochronous = function == URB_FUNCTION_ISOCH_TRANSFER;
    transaction->BufferBytes = sizeof(ioctl_usbtx_header_t) + payloadLength;

    {
        ioctl_usbtx_header_t* outer = (ioctl_usbtx_header_t*)transaction->Buffer;
        PXUSB_USER_TRANSACTION request = (PXUSB_USER_TRANSACTION)(transaction->Buffer + FIELD_OFFSET(ioctl_usbtx_header_t, type));
        RtlZeroMemory(transaction->Buffer, transaction->BufferBytes);
        outer->inter_handle = transaction->Id;
        outer->inter_seqno = transaction->Sequence;
        outer->data_length = (LONG)payloadLength;
        outer->result = -1;
        request->Result = -1;
        request->DataLength = (LONG)payloadLength;

        if (function == URB_FUNCTION_SELECT_CONFIGURATION) {
            PUSB_CONFIGURATION_DESCRIPTOR descriptor = Urb->UrbSelectConfiguration.ConfigurationDescriptor;
            LONG value = -1;
            if (descriptor != NULL) {
                __try {
                    value = descriptor->bConfigurationValue;
                }
                __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
                    GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
                    XusbReleaseTransaction(transaction);
                    return GetExceptionCode();
                }
            }
            request->Type = 1;
            request->Descriptor.Type = 2;
            request->Descriptor.Subtype = 1;
            request->Descriptor.Index = -1;
            request->Descriptor.Value = value;
        } else if (function == URB_FUNCTION_SELECT_INTERFACE) {
            request->Type = 1;
            request->Descriptor.Type = 2;
            request->Descriptor.Subtype = 2;
            request->Descriptor.Index = Urb->UrbSelectInterface.Interface.InterfaceNumber;
            request->Descriptor.Value = Urb->UrbSelectInterface.Interface.AlternateSetting;
        } else if (function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ||
            function == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE ||
            function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT ||
            function == URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT ||
            function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ||
            function == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE) {
            request->Type = 1;
            request->Descriptor.Type =
                (function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ||
                    function == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE) ? 1 :
                ((function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ||
                    function == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE) ? 3 : 4);
            request->Descriptor.Subtype = Urb->UrbControlDescriptorRequest.DescriptorType;
            request->Descriptor.IsRead = transaction->IsRead ? 1 : 0;
            request->Descriptor.Index = Urb->UrbControlDescriptorRequest.Index;
            request->Descriptor.Value = Urb->UrbControlDescriptorRequest.LanguageId;
        } else if (function == URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR) {
            request->Type = 1;
            request->Descriptor.Type = 5;
            request->Descriptor.Subtype = 0;
            request->Descriptor.IsRead = 1;
            request->Descriptor.Index =
                ((LONG)Urb->UrbOSFeatureDescriptorRequest.Recipient << 8) |
                Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber;
            request->Descriptor.Value = Urb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex;
        } else if (function == URB_FUNCTION_VENDOR_DEVICE ||
            function == URB_FUNCTION_VENDOR_INTERFACE ||
            function == URB_FUNCTION_VENDOR_ENDPOINT ||
            function == URB_FUNCTION_VENDOR_OTHER ||
            function == URB_FUNCTION_CLASS_DEVICE ||
            function == URB_FUNCTION_CLASS_INTERFACE ||
            function == URB_FUNCTION_CLASS_ENDPOINT ||
            function == URB_FUNCTION_CLASS_OTHER) {
            request->Type = 2;
            request->Vendor.Type = function == URB_FUNCTION_CLASS_DEVICE ||
                function == URB_FUNCTION_CLASS_INTERFACE ||
                function == URB_FUNCTION_CLASS_ENDPOINT ||
                function == URB_FUNCTION_CLASS_OTHER ? 1 : 2;
            request->Vendor.Subtype = function == URB_FUNCTION_VENDOR_DEVICE ||
                function == URB_FUNCTION_CLASS_DEVICE ? 1 :
                (function == URB_FUNCTION_VENDOR_INTERFACE ||
                    function == URB_FUNCTION_CLASS_INTERFACE ? 2 :
                    (function == URB_FUNCTION_VENDOR_ENDPOINT ||
                        function == URB_FUNCTION_CLASS_ENDPOINT ? 3 : 4));
            request->Vendor.IsRead = transaction->IsRead ? 1 : 0;
            request->Vendor.Request = Urb->UrbControlVendorClassRequest.Request;
            request->Vendor.Index = Urb->UrbControlVendorClassRequest.Index;
            request->Vendor.Value = Urb->UrbControlVendorClassRequest.Value;
        } else if (function == URB_FUNCTION_GET_STATUS_FROM_DEVICE ||
            function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ||
            function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ||
            function == URB_FUNCTION_GET_STATUS_FROM_OTHER ||
            function == URB_FUNCTION_GET_CONFIGURATION ||
            function == URB_FUNCTION_GET_INTERFACE ||
            function == URB_FUNCTION_GET_CURRENT_FRAME_NUMBER) {
            request->Type = 5;
            request->Status.Type = function == URB_FUNCTION_GET_STATUS_FROM_DEVICE ? 1 :
                (function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ? 2 :
                    (function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ? 3 :
                        (function == URB_FUNCTION_GET_STATUS_FROM_OTHER ? 4 :
                            (function == URB_FUNCTION_GET_CONFIGURATION ? 5 :
                                (function == URB_FUNCTION_GET_INTERFACE ? 6 : 7)))));
            request->Status.Index = function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ||
                function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ||
                function == URB_FUNCTION_GET_STATUS_FROM_OTHER
                ? Urb->UrbControlGetStatusRequest.Index
                    : (function == URB_FUNCTION_GET_INTERFACE
                        ? Urb->UrbControlGetInterfaceRequest.Interface
                        : 0);
        } else if (function == URB_FUNCTION_SET_FEATURE_TO_DEVICE ||
            function == URB_FUNCTION_SET_FEATURE_TO_INTERFACE ||
            function == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT ||
            function == URB_FUNCTION_SET_FEATURE_TO_OTHER ||
            function == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE ||
            function == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE ||
            function == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT ||
            function == URB_FUNCTION_CLEAR_FEATURE_TO_OTHER) {
            request->Type = 6;
            request->Feature.Type = function == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE ||
                function == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE ||
                function == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT ||
                function == URB_FUNCTION_CLEAR_FEATURE_TO_OTHER ? 2 : 1;
            request->Feature.Subtype = function == URB_FUNCTION_SET_FEATURE_TO_DEVICE ||
                function == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE ? 1 :
                (function == URB_FUNCTION_SET_FEATURE_TO_INTERFACE ||
                    function == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE ? 2 :
                    (function == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT ||
                        function == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT ? 3 : 4));
            request->Feature.Index = Urb->UrbControlFeatureRequest.Index;
            request->Feature.Value = Urb->UrbControlFeatureRequest.FeatureSelector;
        } else if (function == URB_FUNCTION_ABORT_PIPE ||
            function == URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL) {
            request->Type = 4;
            request->Reset.Type = function == URB_FUNCTION_ABORT_PIPE ? 4 : 3;
            request->Reset.EndpointAddress = pipe.EndpointAddress;
        } else {
            request->Type = 3;
            request->Transfer.Type = function == URB_FUNCTION_CONTROL_TRANSFER ||
                function == URB_FUNCTION_CONTROL_TRANSFER_EX ? 1 :
                (function == URB_FUNCTION_ISOCH_TRANSFER ? 3 : 2);
            request->Transfer.EndpointAddress = function == URB_FUNCTION_CONTROL_TRANSFER ||
                function == URB_FUNCTION_CONTROL_TRANSFER_EX
                ? 0
                : pipe.EndpointAddress;
            request->Transfer.IsRead = transaction->IsRead ? 1 : 0;
            if (function == URB_FUNCTION_CONTROL_TRANSFER ||
                function == URB_FUNCTION_CONTROL_TRANSFER_EX) {
                RtlCopyMemory(
                    request->Transfer.SetupPacket,
                    function == URB_FUNCTION_CONTROL_TRANSFER
                        ? Urb->UrbControlTransfer.SetupPacket
                        : Urb->UrbControlTransferEx.SetupPacket,
                    sizeof(Urb->UrbControlTransfer.SetupPacket));
                if (function == URB_FUNCTION_CONTROL_TRANSFER_EX) {
                    request->Transfer.Timeout = Urb->UrbControlTransferEx.Timeout;
                }
            } else if (transaction->IsIsochronous) {
                iso_packet_hdr_t* isoHeader = (iso_packet_hdr_t*)request->Data;
                isoHeader->start_frame = Urb->UrbIsochronousTransfer.StartFrame;
                isoHeader->flags = Urb->UrbIsochronousTransfer.TransferFlags;
                isoHeader->error_count = Urb->UrbIsochronousTransfer.ErrorCount;
                isoHeader->number_packets = packetCount;
                request->Transfer.NumberPackets = (LONG)packetCount;
                for (ULONG index = 0; index < packetCount; ++index) {
                    const USBD_ISO_PACKET_DESCRIPTOR* sourcePacket = &Urb->UrbIsochronousTransfer.IsoPacket[index];
                    iso_packet_t* targetPacket = &isoHeader->packets[index];
                    if (sourcePacket->Offset > length || sourcePacket->Length > length - sourcePacket->Offset) {
                        XusbReleaseTransaction(transaction);
                        return STATUS_INVALID_PARAMETER;
                    }
                    targetPacket->offset = sourcePacket->Offset;
                    targetPacket->length = sourcePacket->Length;
                    targetPacket->status = sourcePacket->Status;
                }
            }
        }
        if (!transaction->IsRead && length != 0) {
            __try {
                if (transferBuffer == NULL) {
                    ExRaiseStatus(STATUS_INVALID_USER_BUFFER);
                }
                RtlCopyMemory(request->Data + isoMetadataLength, transferBuffer, length);
            }
            __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
                GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT ||
                GetExceptionCode() == STATUS_INVALID_USER_BUFFER
                ? EXCEPTION_EXECUTE_HANDLER
                : EXCEPTION_CONTINUE_SEARCH) {
                XusbReleaseTransaction(transaction);
                return GetExceptionCode();
            }
        }
    }
    *Result = transaction;
    return STATUS_SUCCESS;
}
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 6014) // Result transfers transaction ownership to the caller for queueing.
static NTSTATUS XusbBuildPortTransaction(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG IoControlCode,
    _Outptr_ PXUSB_TRANSACTION* Result)
{
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    PXUSB_TRANSACTION transaction;
    const LONG64 reservation = sizeof(*transaction) + sizeof(ioctl_usbtx_header_t);
    ioctl_usbtx_header_t* outer;
    PXUSB_USER_TRANSACTION request;
    NTSTATUS status;

    *Result = NULL;
    if (IoControlCode != IOCTL_INTERNAL_USB_RESET_PORT &&
        IoControlCode != IOCTL_INTERNAL_USB_CYCLE_PORT) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = XusbReserveTransactionResources(child, reservation);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    transaction = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*transaction), XUSB_TRANSACTION_TAG);
    if (transaction == NULL) {
        XusbReleaseTransactionResources(child, reservation);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(transaction, sizeof(*transaction));
    transaction->References = 1;
    transaction->Child = child;
    transaction->ReservedBytes = reservation;
    ObReferenceObject(DeviceObject);
    transaction->Irp = Irp;
    transaction->SavedDriverContext0 = Irp->Tail.Overlay.DriverContext[0];
    transaction->PortIoControlCode = IoControlCode;
    transaction->Id = (ULONGLONG)InterlockedIncrement64(&child->NextTransactionId);
    transaction->Sequence = InterlockedIncrement(&child->NextSequence);
    transaction->BufferBytes = sizeof(ioctl_usbtx_header_t);
    transaction->Buffer = ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        transaction->BufferBytes,
        XUSB_TRANSACTION_TAG);
    if (transaction->Buffer == NULL) {
        XusbReleaseTransaction(transaction);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(transaction->Buffer, transaction->BufferBytes);
    outer = (ioctl_usbtx_header_t*)transaction->Buffer;
    request = (PXUSB_USER_TRANSACTION)(transaction->Buffer +
        FIELD_OFFSET(ioctl_usbtx_header_t, type));
    outer->inter_handle = transaction->Id;
    outer->inter_seqno = transaction->Sequence;
    outer->data_length = 0;
    outer->result = -1;
    request->Type = 4;
    request->Result = -1;
    request->DataLength = 0;
    request->Reset.Type = IoControlCode == IOCTL_INTERNAL_USB_RESET_PORT ? 1 : 2;

    *Result = transaction;
    return STATUS_SUCCESS;
}
#pragma warning(pop)

static NTSTATUS XusbGetVirtualPortStatus(
    _In_ PXUSB_CHILD_EXTENSION Child,
    _Inout_ PIRP Irp)
{
    PXUSB_PARENT_EXTENSION parent =
        (PXUSB_PARENT_EXTENSION)Child->ParentDevice->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PULONG output = (PULONG)stack->Parameters.Others.Argument1;
    ULONG portStatus = 0;
    BOOLEAN connected;

    if (output == NULL || Irp->RequestorMode != KernelMode ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&parent->ChildMutex);
    connected = !Child->Internal && Child->Present && !Child->Removed;
    ExReleaseFastMutex(&parent->ChildMutex);
    if (connected) {
        portStatus = USBD_PORT_CONNECTED | USBD_PORT_ENABLED;
    }

    __try {
        *output = portStatus;
    }
    __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
        GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

NTSTATUS XusbQueueUsbRequest(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG ioControlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    PURB urb = ioControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB
        ? (PURB)stack->Parameters.Others.Argument1
        : NULL;
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    PXUSB_TRANSACTION transaction = NULL;
    KIRQL oldIrql;
    NTSTATUS status;

    status = IoAcquireRemoveLock(&child->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    if (child->Signature != XUSB_CHILD_SIGNATURE) {
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }
    if (ioControlCode == IOCTL_INTERNAL_USB_GET_PORT_STATUS) {
        status = XusbGetVirtualPortStatus(child, Irp);
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    if (ioControlCode == IOCTL_INTERNAL_USB_RESET_PORT ||
        ioControlCode == IOCTL_INTERNAL_USB_CYCLE_PORT) {
        status = XusbBuildPortTransaction(DeviceObject, Irp, ioControlCode, &transaction);
        if (!NT_SUCCESS(status)) {
            IoReleaseRemoveLock(&child->RemoveLock, Irp);
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }
    } else if (ioControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB || urb == NULL) {
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }
    if (urb != NULL) {
    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_CONTROL_TRANSFER:
    case URB_FUNCTION_CONTROL_TRANSFER_EX:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER:
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER:
    case URB_FUNCTION_GET_CONFIGURATION:
    case URB_FUNCTION_GET_INTERFACE:
    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
    case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
    case URB_FUNCTION_SET_FEATURE_TO_OTHER:
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case URB_FUNCTION_SELECT_CONFIGURATION:
    case URB_FUNCTION_SELECT_INTERFACE:
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
    case URB_FUNCTION_ISOCH_TRANSFER:
        break;
    default:
        urb->UrbHeader.Status = USBD_STATUS_INVALID_URB_FUNCTION;
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }
    status = XusbBuildTransaction(DeviceObject, Irp, urb, &transaction);
    if (!NT_SUCCESS(status)) {
        urb->UrbHeader.Status = status == STATUS_BUFFER_TOO_SMALL
            ? USBD_STATUS_BUFFER_TOO_SMALL
            : (status == STATUS_NOT_SUPPORTED
                ? USBD_STATUS_INVALID_URB_FUNCTION
                : USBD_STATUS_INVALID_PARAMETER);
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    }
    KeAcquireSpinLock(&child->TransactionLock, &oldIrql);
    if (child->TransactionStopping != 0 || child->Semaphore == NULL) {
        KeReleaseSpinLock(&child->TransactionLock, oldIrql);
        if (urb != NULL) {
            urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
        }
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        XusbReleaseTransaction(transaction);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    transaction->Semaphore = child->Semaphore;
    ObReferenceObject(transaction->Semaphore);
    KeReleaseSpinLock(&child->TransactionLock, oldIrql);
    Irp->Tail.Overlay.DriverContext[0] = transaction;
    InterlockedIncrement(&transaction->References); // Keep the dispatch path alive across concurrent cancellation.
    if (urb != NULL) {
        urb->UrbHeader.Status = USBD_STATUS_PENDING;
    }
    IoMarkIrpPending(Irp);
    IoCsqInsertIrp(&child->TransactionCsq, Irp, &transaction->CsqContext);
    if (InterlockedCompareExchange(&child->TransactionStopping, 0, 0) != 0) {
        if (IoCsqRemoveIrp(&child->TransactionCsq, &transaction->CsqContext) == Irp) {
            if (urb != NULL) {
                urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
            }
            XusbCompleteUsbIrp(transaction, STATUS_DEVICE_NOT_CONNECTED);
            XusbReleaseTransaction(transaction);
            XusbReleaseTransaction(transaction);
            return STATUS_PENDING;
        }
    } else {
        KeReleaseSemaphore(transaction->Semaphore, IO_NO_INCREMENT, 1, FALSE);
    }
    XusbReleaseTransaction(transaction);
    return STATUS_PENDING;
}

NTSTATUS XusbBeginUsbTransaction(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    PXUSB_TRANSACTION transaction;
    PVOID output = Irp->UserBuffer;
    ULONG outputBytes = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ioctl_usbtx_header_t header;
    NTSTATUS status = STATUS_NO_MORE_ENTRIES;
    PIRP queuedIrp;
    BOOLEAN removeLockHeld = FALSE;

    status = IoAcquireRemoveLock(&child->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        goto Complete;
    }
    removeLockHeld = TRUE;

    if (output == NULL || outputBytes < sizeof(header)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto Complete;
    }
    if (KeGetCurrentIrql() > APC_LEVEL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Complete;
    }
    queuedIrp = IoCsqRemoveNextIrp(&child->TransactionCsq, NULL);
    if (queuedIrp == NULL) {
        status = STATUS_NO_MORE_ENTRIES;
        goto Complete;
    }
    transaction = XusbGetTransaction(queuedIrp);
    if (transaction == NULL) {
        IoCsqInsertIrp(&child->TransactionCsq, queuedIrp, NULL);
        status = STATUS_INVALID_DEVICE_STATE;
        goto Complete;
    }
    InterlockedIncrement(&transaction->References); // The queue owns one reference again after reinsert.
    IoCsqInsertIrp(&child->TransactionCsq, queuedIrp, &transaction->CsqContext);
    if (InterlockedCompareExchange(&child->TransactionStopping, 0, 0) != 0) {
        if (IoCsqRemoveIrp(&child->TransactionCsq, &transaction->CsqContext) == queuedIrp) {
            if (transaction->Urb != NULL) {
                transaction->Urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
            }
            XusbCompleteUsbIrp(transaction, STATUS_DEVICE_NOT_CONNECTED);
            XusbReleaseTransaction(transaction);
        }
        XusbReleaseTransaction(transaction);
        status = STATUS_DEVICE_NOT_CONNECTED;
        goto Complete;
    }
    RtlCopyMemory(&header, transaction->Buffer, sizeof(header));
    __try {
        ProbeForWrite(output, sizeof(header), 1);
        if (outputBytes < transaction->BufferBytes) {
            RtlCopyMemory(output, &header, sizeof(header));
            Irp->IoStatus.Information = sizeof(header);
            status = STATUS_BUFFER_OVERFLOW;
        } else {
            ProbeForWrite(output, transaction->BufferBytes, 1);
            RtlCopyMemory(output, transaction->Buffer, transaction->BufferBytes);
            Irp->IoStatus.Information = transaction->BufferBytes;
            status = STATUS_SUCCESS;
        }
    }
    __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
        GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT ||
        GetExceptionCode() == STATUS_INVALID_USER_BUFFER
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH) {
        status = GetExceptionCode();
        Irp->IoStatus.Information = 0;
    }
    XusbReleaseTransaction(transaction);

Complete:
    if (removeLockHeld) {
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
    }
    Irp->IoStatus.Status = status;
    if (status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW) {
        Irp->IoStatus.Information = 0;
    }
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS XusbEndUsbTransaction(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PXUSB_CHILD_EXTENSION child = (PXUSB_CHILD_EXTENSION)DeviceObject->DeviceExtension;
    ULONG inputBytes = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG inputLength;
    PVOID input = stack->Parameters.DeviceIoControl.Type3InputBuffer;
    ioctl_usbtx_header_t header;
    PUCHAR copy = NULL;
    ULONG dataLength;
    PXUSB_TRANSACTION transaction;
    XUSB_END_MATCH match;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    BOOLEAN removeLockHeld = FALSE;

    status = IoAcquireRemoveLock(&child->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        goto Complete;
    }
    removeLockHeld = TRUE;

    if (input == NULL || inputBytes < sizeof(header) || inputBytes > sizeof(header) + XUSB_MAX_TRANSACTION_BYTES) {
        goto Complete;
    }
    if (KeGetCurrentIrql() > APC_LEVEL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Complete;
    }
    __try {
        ProbeForRead(input, sizeof(header), 1);
        RtlCopyMemory(&header, input, sizeof(header));
    }
    __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
        GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT ||
        GetExceptionCode() == STATUS_INVALID_USER_BUFFER
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH) {
        status = GetExceptionCode();
        goto Complete;
    }
    if (header.data_length < 0 || (ULONG)header.data_length > inputBytes - sizeof(header)) {
        goto Complete;
    }
    dataLength = (ULONG)header.data_length;
    if ((ULONG)header.data_length > XUSB_MAX_TRANSACTION_BYTES) {
        goto Complete;
    }
    match.Id = header.inter_handle;
    match.Sequence = header.inter_seqno;
    {
        PIRP original = IoCsqRemoveNextIrp(&child->TransactionCsq, &match);
        transaction = original != NULL ? XusbGetTransaction(original) : NULL;
    }
    if (transaction == NULL) {
        status = STATUS_NOT_FOUND;
        goto Complete;
    }
    if (dataLength > transaction->PayloadCapacity) {
        status = STATUS_INVALID_PARAMETER;
    } else if (header.result != 0) {
        if (transaction->PortIoControlCode != 0) {
            status = STATUS_ACCESS_DENIED;
        } else {
            transaction->Urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
        }
        goto CompleteTransaction;
    } else {
        ioctl_usbtx_header_t copiedHeader;
        PXUSB_USER_TRANSACTION response;
        ULONG actualLength;
        inputLength = sizeof(header) + dataLength;
        if (inputLength < sizeof(copiedHeader)) {
            status = STATUS_INVALID_PARAMETER;
            goto CompleteTransaction;
        }
        copy = ExAllocatePool2(POOL_FLAG_PAGED, inputLength, XUSB_TRANSACTION_TAG);
        if (copy == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto CompleteTransaction;
        }
        __try {
            ProbeForRead(input, inputLength, 1);
            RtlCopyMemory(copy, input, inputLength);
        }
        __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
            GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT ||
            GetExceptionCode() == STATUS_INVALID_USER_BUFFER
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
            status = GetExceptionCode();
            goto CompleteTransaction;
        }
        RtlCopyMemory(&copiedHeader, copy, sizeof(copiedHeader));
        if (copiedHeader.inter_handle != header.inter_handle ||
            copiedHeader.inter_seqno != header.inter_seqno ||
            copiedHeader.data_length != header.data_length ||
            copiedHeader.result != header.result) {
            status = STATUS_INVALID_PARAMETER;
            goto CompleteTransaction;
        }
        response = (PXUSB_USER_TRANSACTION)(copy + FIELD_OFFSET(ioctl_usbtx_header_t, type));
        actualLength = (ULONG)response->DataLength;
        if (response->DataLength < 0 || actualLength > dataLength) {
            status = STATUS_INVALID_PARAMETER;
            goto CompleteTransaction;
        }
        if (transaction->PortIoControlCode != 0) {
            status = dataLength == 0 && actualLength == 0
                ? STATUS_SUCCESS
                : STATUS_INVALID_PARAMETER;
        } else switch (transaction->UrbFunction) {
        case URB_FUNCTION_SELECT_CONFIGURATION:
            status = actualLength == 0 && dataLength == 0
                ? XusbSelectConfiguration(child, transaction->Urb)
                : STATUS_INVALID_PARAMETER;
            break;

        case URB_FUNCTION_SELECT_INTERFACE:
            status = actualLength == 0 && dataLength == 0
                ? XusbSelectInterface(child, transaction->Urb)
                : STATUS_INVALID_PARAMETER;
            break;

        case URB_FUNCTION_ISOCH_TRANSFER: {
            iso_packet_hdr_t* isoResponse = (iso_packet_hdr_t*)response->Data;
            ULONG expectedMetadata = transaction->IsoMetadataLength;
            ULONG actualPayload;
            ULONG payloadOffset = 0;
            ULONG index;

            if (actualLength < expectedMetadata ||
                isoResponse->number_packets != transaction->PacketCount) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            actualPayload = actualLength - expectedMetadata;
            if (actualPayload > transaction->TransferLength) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            status = STATUS_SUCCESS;
            for (index = 0; index < transaction->PacketCount; ++index) {
                iso_packet_t* packet = &isoResponse->packets[index];
                USBD_ISO_PACKET_DESCRIPTOR* original =
                    &transaction->Urb->UrbIsochronousTransfer.IsoPacket[index];
                if (packet->offset != original->Offset ||
                    packet->offset > transaction->TransferLength ||
                    packet->length > transaction->TransferLength - packet->offset ||
                    packet->length > actualPayload - payloadOffset) {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                payloadOffset += packet->length;
            }
            if (!NT_SUCCESS(status)) {
                break;
            }
            if (transaction->IsRead) {
                __try {
                    for (index = 0; index < transaction->PacketCount; ++index) {
                        iso_packet_t* packet = &isoResponse->packets[index];
                        if (packet->length != 0) {
                            RtlCopyMemory(
                                (PUCHAR)transaction->TransferBuffer + packet->offset,
                                response->Data + expectedMetadata + payloadOffset - packet->length,
                                packet->length);
                        }
                    }
                }
                __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
                    GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT ||
                    GetExceptionCode() == STATUS_INVALID_USER_BUFFER
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
                    status = GetExceptionCode();
                    break;
                }
            }
            transaction->Urb->UrbIsochronousTransfer.StartFrame = isoResponse->start_frame;
            transaction->Urb->UrbIsochronousTransfer.ErrorCount = isoResponse->error_count;
            transaction->Urb->UrbIsochronousTransfer.TransferBufferLength = actualPayload;
            for (index = 0; index < transaction->PacketCount; ++index) {
                iso_packet_t* packet = &isoResponse->packets[index];
                USBD_ISO_PACKET_DESCRIPTOR* original =
                    &transaction->Urb->UrbIsochronousTransfer.IsoPacket[index];
                original->Length = packet->length;
                original->Status = packet->status == 0
                    ? USBD_STATUS_SUCCESS
                    : (USBD_STATUS)0x80000000;
            }
            transaction->Urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            break;
        }
        case URB_FUNCTION_CONTROL_TRANSFER:
        case URB_FUNCTION_CONTROL_TRANSFER_EX:
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        case URB_FUNCTION_VENDOR_OTHER:
        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_ENDPOINT:
        case URB_FUNCTION_CLASS_OTHER:
        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        case URB_FUNCTION_GET_CONFIGURATION:
        case URB_FUNCTION_GET_INTERFACE:
        case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
        case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
            if (actualLength > transaction->TransferLength) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            status = STATUS_SUCCESS;
            if (transaction->IsRead && actualLength != 0) {
                __try {
                    if (transaction->TransferBuffer == NULL) {
                        ExRaiseStatus(STATUS_INVALID_USER_BUFFER);
                    }
                    RtlCopyMemory(transaction->TransferBuffer, response->Data, actualLength);
                }
                __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION ||
                    GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT ||
                    GetExceptionCode() == STATUS_INVALID_USER_BUFFER
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
                    status = GetExceptionCode();
                }
            }
            if (NT_SUCCESS(status)) {
                switch (transaction->UrbFunction) {
                case URB_FUNCTION_CONTROL_TRANSFER:
                    transaction->Urb->UrbControlTransfer.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_CONTROL_TRANSFER_EX:
                    transaction->Urb->UrbControlTransferEx.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
                    transaction->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
                case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
                case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
                case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
                case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
                case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
                    transaction->Urb->UrbControlDescriptorRequest.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_VENDOR_DEVICE:
                case URB_FUNCTION_VENDOR_INTERFACE:
                case URB_FUNCTION_VENDOR_ENDPOINT:
                case URB_FUNCTION_VENDOR_OTHER:
                case URB_FUNCTION_CLASS_DEVICE:
                case URB_FUNCTION_CLASS_INTERFACE:
                case URB_FUNCTION_CLASS_ENDPOINT:
                case URB_FUNCTION_CLASS_OTHER:
                    transaction->Urb->UrbControlVendorClassRequest.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
                case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
                case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
                case URB_FUNCTION_GET_STATUS_FROM_OTHER:
                    transaction->Urb->UrbControlGetStatusRequest.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_GET_CONFIGURATION:
                    transaction->Urb->UrbControlGetConfigurationRequest.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_GET_INTERFACE:
                    transaction->Urb->UrbControlGetInterfaceRequest.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
                    transaction->Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength = actualLength;
                    break;
                case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
                    /* The existing UVC user-mode handler returns a 16-bit frame value. */
                    if (actualLength != sizeof(USHORT) && actualLength != sizeof(ULONG)) {
                        status = STATUS_INVALID_PARAMETER;
                    } else if (actualLength == sizeof(USHORT)) {
                        USHORT frameNumber;
                        RtlCopyMemory(&frameNumber, response->Data, sizeof(frameNumber));
                        transaction->Urb->UrbGetCurrentFrameNumber.FrameNumber = frameNumber;
                    }
                    break;
                }
                if (NT_SUCCESS(status)) {
                    transaction->Urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
                }
            }
            break;

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
            status = (actualLength == 0 && dataLength == 0)
                ? STATUS_SUCCESS
                : STATUS_INVALID_PARAMETER;
            if (NT_SUCCESS(status)) {
                transaction->Urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            }
            break;

        default:
            status = STATUS_NOT_SUPPORTED;
            break;
        }
    }
CompleteTransaction:
    if (!NT_SUCCESS(status) && header.result == 0 && transaction->Urb != NULL) {
        transaction->Urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
    }
    XusbCompleteUsbIrp(transaction, status);
    XusbReleaseTransaction(transaction);

Complete:
    if (copy != NULL) {
        ExFreePoolWithTag(copy, XUSB_TRANSACTION_TAG);
    }
    if (removeLockHeld) {
        IoReleaseRemoveLock(&child->RemoveLock, Irp);
    }
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

VOID XusbStopTransactions(_Inout_ PXUSB_CHILD_EXTENSION Child)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Child->TransactionLock, &oldIrql);
    InterlockedExchange(&Child->TransactionStopping, 1);
    KeReleaseSpinLock(&Child->TransactionLock, oldIrql);
}

VOID XusbCancelTransactions(_Inout_ PXUSB_CHILD_EXTENSION Child)
{
    PIRP irp;
    XusbStopTransactions(Child);
    while ((irp = IoCsqRemoveNextIrp(&Child->TransactionCsq, NULL)) != NULL) {
        PXUSB_TRANSACTION transaction = XusbGetTransaction(irp);
        if (transaction != NULL) {
            if (transaction->Urb != NULL) {
                transaction->Urb->UrbHeader.Status = USBD_STATUS_CANCELED;
            }
            XusbCompleteUsbIrp(transaction, STATUS_DEVICE_NOT_CONNECTED);
            XusbReleaseTransaction(transaction);
        }
    }
}

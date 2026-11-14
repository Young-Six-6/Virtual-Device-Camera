#pragma once

#include <ntddk.h>
#include <wmilib.h>
#include "xusb_abi.h"

#define XUSB_PARENT_SIGNATURE 'PXUX'
#define XUSB_CHILD_SIGNATURE  'CXUX'
#define XUSB_FILE_SIGNATURE   'FXUX'
#define XUSB_POOL_TAG         'sUXV'
#define XUSB_MAX_PIPES        64

typedef enum _XUSB_DEVICE_ROLE {
    XusbRoleUsbOnlyEnumerator = 0,
    XusbRoleRootHub = 1,
    XusbRoleHostController = 2,
    XusbRoleUsbTreeEnumerator = 3,
    XusbRoleUnknown = 4
} XUSB_DEVICE_ROLE;

typedef struct _XUSB_FILE_CONTEXT {
    ULONG Signature;
    PDEVICE_OBJECT ChildPdo;
} XUSB_FILE_CONTEXT, *PXUSB_FILE_CONTEXT;

typedef struct _XUSB_PARENT_EXTENSION {
    ULONG Signature;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT LowerDevice;
    IO_REMOVE_LOCK RemoveLock;
    FAST_MUTEX ChildMutex;
    LIST_ENTRY Children;
    ULONG NextBusAddress;
    XUSB_DEVICE_ROLE Role;
    UNICODE_STRING InterfaceName;
    BOOLEAN InterfaceEnabled;
    UNICODE_STRING ControlInterfaceName;
    BOOLEAN ControlInterfaceEnabled;
    volatile LONG Removing;
    WMILIB_CONTEXT WmiLibInfo;
    volatile LONG WmiValue0;
    volatile LONG WmiValue1;
    BOOLEAN WmiRegistered;
} XUSB_PARENT_EXTENSION, *PXUSB_PARENT_EXTENSION;

typedef struct _XUSB_PIPE_ENTRY {
    ULONG_PTR Handle;
    UCHAR InterfaceNumber;
    UCHAR EndpointAddress;
    UCHAR Interval;
    UCHAR PipeType;
    USHORT MaximumPacketSize;
    USHORT Reserved;
} XUSB_PIPE_ENTRY, *PXUSB_PIPE_ENTRY;

typedef struct _XUSB_CHILD_EXTENSION {
    ULONG Signature;
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT ParentDevice;
    PDEVICE_OBJECT PhysicalDeviceObject;
    LIST_ENTRY Link;
    LIST_ENTRY RemovalLink;
    BOOLEAN Linked;
    BOOLEAN Reported;
    BOOLEAN Internal;
    BOOLEAN RemovalPending;
    LONG BusAddress;
    BOOLEAN Present;
    BOOLEAN Removed;
    DEVICE_POWER_STATE DevicePowerState;
    UNICODE_STRING DeviceId;
    UNICODE_STRING HardwareIds;
    UNICODE_STRING CompatibleIds;
    PKEVENT RemoveEvent;
    PKSEMAPHORE Semaphore;
    IO_REMOVE_LOCK RemoveLock;
    IO_CSQ TransactionCsq;
    KSPIN_LOCK TransactionLock;
    LIST_ENTRY TransactionQueue;
    volatile LONG64 NextTransactionId;
    volatile LONG NextSequence;
    volatile LONG TransactionStopping;
    volatile LONG TransactionCount;
    volatile LONG64 TransactionBytes;
    FAST_MUTEX ConfigurationMutex;
    PVOID ConfigurationDescriptor;
    ULONG ConfigurationLength;
    ULONG_PTR ConfigurationHandle;
    ULONG HandleGeneration;
    ULONG PipeCount;
    XUSB_PIPE_ENTRY Pipes[XUSB_MAX_PIPES];
} XUSB_CHILD_EXTENSION, *PXUSB_CHILD_EXTENSION;

_Dispatch_type_(IRP_MJ_CREATE) DRIVER_DISPATCH XusbCreateFile;
_Dispatch_type_(IRP_MJ_CLEANUP) DRIVER_DISPATCH XusbCleanupFile;
_Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH XusbCloseFile;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH XusbDispatchDeviceControl;
_Dispatch_type_(IRP_MJ_SYSTEM_CONTROL) DRIVER_DISPATCH XusbDispatchSystemControl;
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS XusbEnumerateChildren(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
_IRQL_requires_(PASSIVE_LEVEL)
_Dispatch_type_(IRP_MJ_PNP) DRIVER_DISPATCH XusbDispatchChildPnp;
VOID XusbRemoveAllChildren(_In_ PDEVICE_OBJECT DeviceObject);
NTSTATUS XusbCreateRoleChild(_In_ PDEVICE_OBJECT DeviceObject);
NTSTATUS XusbGetDeviceRole(_In_ PDEVICE_OBJECT PhysicalDeviceObject, _Out_ XUSB_DEVICE_ROLE* Role);
VOID XusbWmiInitialize(_Inout_ PXUSB_PARENT_EXTENSION Parent);
NTSTATUS XusbWmiSetServiceRegistryPath(_In_ PCUNICODE_STRING RegistryPath);
VOID XusbWmiReleaseServiceRegistryPath(VOID);
NTSTATUS XusbWmiStart(_Inout_ PXUSB_PARENT_EXTENSION Parent, _In_ PDEVICE_OBJECT DeviceObject);
VOID XusbWmiStop(_Inout_ PXUSB_PARENT_EXTENSION Parent, _In_ PDEVICE_OBJECT DeviceObject);
NTSTATUS XusbInitializeTransactions(_Inout_ PXUSB_CHILD_EXTENSION Child);
NTSTATUS XusbQueueUsbRequest(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
NTSTATUS XusbBeginUsbTransaction(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
NTSTATUS XusbEndUsbTransaction(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
VOID XusbCancelTransactions(_Inout_ PXUSB_CHILD_EXTENSION Child);
VOID XusbStopTransactions(_Inout_ PXUSB_CHILD_EXTENSION Child);
VOID XusbReleaseChildConfiguration(_Inout_ PXUSB_CHILD_EXTENSION Child);

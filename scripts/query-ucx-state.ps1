# Query initialization milestones reported by the reconstructed UCX HCD.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$interfaceGuid = '{ed3fb623-0c23-48b8-880a-9d1a07263ffd}'
$nativeSource = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class UcxStateQuery
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct State
    {
        public uint Signature;
        public uint StructureSize;
        public uint AbiVersion;
        public uint StageFlags;
        public int DeviceInitStatus;
        public int WdfDeviceStatus;
        public int VirtInterfaceStatus;
        public int HcdInterfaceStatus;
        public int IoQueueStatus;
        public int ControllerStatus;
        public int RootHubStatus;
        public int PrepareHardwareStatus;
        public uint PrepareHardwareCount;
        public uint RootHubGetInfoCount;
        public uint ControllerHandlePresent;
        public uint RootHubHandlePresent;
        public uint RootHubGet20PortInfoCount;
        public uint RootHubGet30PortInfoCount;
        public uint RootHubControlCount;
        public uint RootHubInterruptCount;
        public int RootHubGetInfoStatus;
        public int RootHubGet20PortInfoStatus;
        public uint RootHubLastUrbFunction;
        public uint RootHubLastSetup;
        public uint RootHubLastValueIndex;
        public uint RootHubLastUsbStatus;
        public int RootHubGet30PortInfoStatus;
        public uint RootHubGet30NumberOfPorts;
        public uint RootHubGet30PortInfoSize;
        public uint RootHubControl20Count;
        public uint RootHubControl30Count;
        public uint RootHubInterrupt20Count;
        public uint RootHubInterrupt30Count;
        public uint ReleaseHardwareCount;
        public uint ReleaseHardwareCompletedCount;
        public uint D0EntryCount;
        public uint D0ExitCount;
        public uint CurrentPowerState;
        public uint Stopping;
        public uint PendingTransactions;
        public uint PendingRemovalWorkItems;
        public uint UserQueueState;
        public uint RootHubInvalidRequestCount;
        public uint RootHubLastInvalidUrbFunction;
        public uint RootHubLastInvalidSetup;
        public uint RootHubLastInvalidValueIndex;
        public uint RootHubLastInvalidSetupLength;
        public uint RootHubLastInvalidTransferLength;
        public uint RootHubLastInvalidUsbStatus;
        public uint RootHubLastInvalidIsUsb30;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint creation, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code,
        IntPtr input, uint inputLength, out State output, uint outputLength,
        out uint bytesReturned, IntPtr overlapped);

    public static State Query(string path)
    {
        using (SafeFileHandle device = CreateFile(path, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
        {
            if (device.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            State state;
            uint returned;
            if (!DeviceIoControl(device, 0x22602c, IntPtr.Zero, 0, out state,
                (uint)Marshal.SizeOf(typeof(State)), out returned, IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            if ((returned != 64 && returned != 104 && returned != 132 && returned != 168 && returned != Marshal.SizeOf(typeof(State))) ||
                state.StructureSize != returned || state.Signature != 0x53554358)
                throw new InvalidOperationException("UCX state response has an unexpected signature or size.");
            return state;
        }
    }
}
'@

if (-not ('UcxStateQuery' -as [type])) {
    Add-Type -TypeDefinition $nativeSource
}

$interfaceLines = & (Join-Path $env:WINDIR 'System32\pnputil.exe') /enum-interfaces /class $interfaceGuid
$pattern = '\\\\\?\\[^\s]+#\{ed3fb623-0c23-48b8-880a-9d1a07263ffd\}'
$interfacePath = $null
foreach ($line in $interfaceLines) {
    $match = [regex]::Match($line, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($match.Success) { $interfacePath = $match.Value; break }
}
if (-not $interfacePath) { throw "No present HCD interface $interfaceGuid was found." }

try {
    $state = [UcxStateQuery]::Query($interfacePath)
}
catch [System.ComponentModel.Win32Exception] {
    if ($_.Exception.NativeErrorCode -eq 1) {
        throw 'The installed UCX HCD does not implement IOCTL_XUSB_QUERY_UCX_STATE. Install the current replacement package before querying initialization status.'
    }
    throw
}
$stageNames = @(
    @{ Mask = 0x01; Name = 'UcxInitializeDeviceInit' },
    @{ Mask = 0x02; Name = 'WdfDeviceCreate' },
    @{ Mask = 0x04; Name = 'GUID_XUSB_VIRT_INTERFACE' },
    @{ Mask = 0x08; Name = 'GUID_DEVINTERFACE_USB_HOST_CONTROLLER' },
    @{ Mask = 0x10; Name = 'WdfIoQueueCreate' },
    @{ Mask = 0x20; Name = 'UcxControllerCreate' },
    @{ Mask = 0x40; Name = 'UcxRootHubCreate' }
)
$completed = @($stageNames | Where-Object { ($state.StageFlags -band $_.Mask) -ne 0 } | ForEach-Object Name)

[pscustomobject]@{
    InterfacePath = $interfacePath
    ResponseSize = $state.StructureSize
    AbiVersion = $state.AbiVersion
    StageFlags = ('0x{0:X8}' -f $state.StageFlags)
    CompletedStages = $completed
    DeviceInitStatus = ('0x{0:X8}' -f [uint32]$state.DeviceInitStatus)
    WdfDeviceStatus = ('0x{0:X8}' -f [uint32]$state.WdfDeviceStatus)
    VirtInterfaceStatus = ('0x{0:X8}' -f [uint32]$state.VirtInterfaceStatus)
    HcdInterfaceStatus = ('0x{0:X8}' -f [uint32]$state.HcdInterfaceStatus)
    IoQueueStatus = ('0x{0:X8}' -f [uint32]$state.IoQueueStatus)
    ControllerStatus = ('0x{0:X8}' -f [uint32]$state.ControllerStatus)
    RootHubStatus = ('0x{0:X8}' -f [uint32]$state.RootHubStatus)
    PrepareHardwareStatus = ('0x{0:X8}' -f [uint32]$state.PrepareHardwareStatus)
    PrepareHardwareCount = $state.PrepareHardwareCount
    RootHubGetInfoCount = $state.RootHubGetInfoCount
    ControllerHandlePresent = [bool]$state.ControllerHandlePresent
    RootHubHandlePresent = [bool]$state.RootHubHandlePresent
    RootHubGet20PortInfoCount = $state.RootHubGet20PortInfoCount
    RootHubGet30PortInfoCount = $state.RootHubGet30PortInfoCount
    RootHubControlCount = $state.RootHubControlCount
    RootHubInterruptCount = $state.RootHubInterruptCount
    RootHubGetInfoStatus = ('0x{0:X8}' -f ([int64]$state.RootHubGetInfoStatus -band 0xffffffffL))
    RootHubGet20PortInfoStatus = ('0x{0:X8}' -f ([int64]$state.RootHubGet20PortInfoStatus -band 0xffffffffL))
    RootHubLastUrbFunction = ('0x{0:X4}' -f $state.RootHubLastUrbFunction)
    RootHubLastSetup = ('0x{0:X4}' -f $state.RootHubLastSetup)
    RootHubLastValueIndex = ('0x{0:X8}' -f $state.RootHubLastValueIndex)
    RootHubLastUsbStatus = ('0x{0:X8}' -f $state.RootHubLastUsbStatus)
    RootHubGet30PortInfoStatus = ('0x{0:X8}' -f ([int64]$state.RootHubGet30PortInfoStatus -band 0xffffffffL))
    RootHubGet30NumberOfPorts = $state.RootHubGet30NumberOfPorts
    RootHubGet30PortInfoSize = $state.RootHubGet30PortInfoSize
    RootHubControl20Count = $state.RootHubControl20Count
    RootHubControl30Count = $state.RootHubControl30Count
    RootHubInterrupt20Count = $state.RootHubInterrupt20Count
    RootHubInterrupt30Count = $state.RootHubInterrupt30Count
    LifecycleDiagnosticsAvailable = $state.StructureSize -ge 168
    ReleaseHardwareCount = if ($state.StructureSize -ge 168) { $state.ReleaseHardwareCount } else { $null }
    ReleaseHardwareCompletedCount = if ($state.StructureSize -ge 168) { $state.ReleaseHardwareCompletedCount } else { $null }
    D0EntryCount = if ($state.StructureSize -ge 168) { $state.D0EntryCount } else { $null }
    D0ExitCount = if ($state.StructureSize -ge 168) { $state.D0ExitCount } else { $null }
    CurrentPowerState = if ($state.StructureSize -ge 168) { $state.CurrentPowerState } else { $null }
    Stopping = if ($state.StructureSize -ge 168) { [bool]$state.Stopping } else { $null }
    PendingTransactions = if ($state.StructureSize -ge 168) { $state.PendingTransactions } else { $null }
    PendingRemovalWorkItems = if ($state.StructureSize -ge 168) { $state.PendingRemovalWorkItems } else { $null }
    UserQueueState = if ($state.StructureSize -ge 168) { '0x{0:X8}' -f $state.UserQueueState } else { $null }
    RootHubInvalidRequestCount = if ($state.StructureSize -ge 200) { $state.RootHubInvalidRequestCount } else { $null }
    RootHubLastInvalidUrbFunction = if ($state.StructureSize -ge 200) { '0x{0:X4}' -f $state.RootHubLastInvalidUrbFunction } else { $null }
    RootHubLastInvalidSetup = if ($state.StructureSize -ge 200) { '0x{0:X4}' -f $state.RootHubLastInvalidSetup } else { $null }
    RootHubLastInvalidValueIndex = if ($state.StructureSize -ge 200) { '0x{0:X8}' -f $state.RootHubLastInvalidValueIndex } else { $null }
    RootHubLastInvalidSetupLength = if ($state.StructureSize -ge 200) { $state.RootHubLastInvalidSetupLength } else { $null }
    RootHubLastInvalidTransferLength = if ($state.StructureSize -ge 200) { $state.RootHubLastInvalidTransferLength } else { $null }
    RootHubLastInvalidUsbStatus = if ($state.StructureSize -ge 200) { '0x{0:X8}' -f $state.RootHubLastInvalidUsbStatus } else { $null }
    RootHubLastInvalidIsUsb30 = if ($state.StructureSize -ge 200) { [bool]$state.RootHubLastInvalidIsUsb30 } else { $null }
} | Format-List

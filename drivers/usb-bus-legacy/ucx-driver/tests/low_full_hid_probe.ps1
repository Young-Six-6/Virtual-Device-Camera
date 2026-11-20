[CmdletBinding()]
param(
    [ValidateSet('Low', 'Full', 'Bulk', 'MalformedEnd')]
    [string]$Speed = 'Low',

    [ValidateRange(5, 60)]
    [int]$ObserveSeconds = 12
)

$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'virtual-device-camera' -ErrorAction SilentlyContinue) {
    throw 'Stop virtual-device-camera before running the isolated USB speed probe.'
}

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public sealed class XusbUsbSpeedProbe
{
    private const uint IoctlAddWithSpeed = 0x222034;
    private const uint IoctlAdd = 0x222004;
    private const uint IoctlRemove = 0x222008;
    private const uint IoctlBegin = 0x222023;
    private const uint IoctlEnd = 0x222027;
    private const uint FileDeviceInterface = 0x12;
    private const uint DigcfPresent = 0x2;
    private const uint DigcfDeviceInterface = 0x10;
    private const uint OpenExisting = 3;
    private const uint ShareReadWrite = 3;
    private const int CreateRequestBytes = 1592;
    private const int CreateRequestBaseBytes = 1584;
    private const int TransactionHeaderBytes = 64;
    private const int TransactionBufferBytes = 1024 * 1024;
    private const string BulkInterfaceGuid = "A3B8D2F2-4A39-47B7-889F-2B3A7A93F2C1";

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct UsbInterfaceDescriptor
    {
        public byte Length;
        public byte DescriptorType;
        public byte InterfaceNumber;
        public byte AlternateSetting;
        public byte NumberOfEndpoints;
        public byte InterfaceClass;
        public byte InterfaceSubClass;
        public byte InterfaceProtocol;
        public byte Interface;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WinUsbPipeInformation
    {
        public int PipeType;
        public byte PipeId;
        public ushort MaximumPacketSize;
        public byte Interval;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeOverlapped
    {
        public IntPtr Internal;
        public IntPtr InternalHigh;
        public uint Offset;
        public uint OffsetHigh;
        public IntPtr EventHandle;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInterfaceData
    {
        public int Size;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInterfaces(IntPtr infoSet, IntPtr deviceInfo, ref Guid classGuid, uint index, ref DeviceInterfaceData data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr infoSet, ref DeviceInterfaceData data, IntPtr detail, int detailBytes, out int requiredBytes, IntPtr deviceInfo);
    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr infoSet);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle file, uint code, IntPtr input, int inputBytes, IntPtr output, int outputBytes, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateSemaphore(IntPtr security, int initialCount, int maximumCount, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateEvent(IntPtr security, bool manualReset, bool initialState, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_Initialize(SafeFileHandle device, out IntPtr interfaceHandle);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_Free(IntPtr interfaceHandle);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_QueryInterfaceSettings(IntPtr interfaceHandle, byte alternateSetting, ref UsbInterfaceDescriptor descriptor);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_QueryPipe(IntPtr interfaceHandle, byte alternateSetting, byte pipeIndex, ref WinUsbPipeInformation pipeInformation);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_ReadPipe(IntPtr interfaceHandle, byte pipeId, IntPtr buffer, uint bufferLength, out uint bytesRead, IntPtr overlapped);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_WritePipe(IntPtr interfaceHandle, byte pipeId, IntPtr buffer, uint bufferLength, out uint bytesWritten, IntPtr overlapped);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_GetOverlappedResult(IntPtr interfaceHandle, IntPtr overlapped, out uint bytesTransferred, [MarshalAs(UnmanagedType.Bool)] bool wait);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_AbortPipe(IntPtr interfaceHandle, byte pipeId);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_SetPipePolicy(IntPtr interfaceHandle, byte pipeId, uint policyType, uint valueLength, ref uint value);
    [DllImport("winusb.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinUsb_GetPipePolicy(IntPtr interfaceHandle, byte pipeId, uint policyType, ref uint valueLength, ref uint value);

    private readonly int _speed;
    private readonly bool _bulkMode;
    private readonly bool _malformedEndMode;
    private readonly int _productId;
    private readonly string _productIdString;
    private readonly string _productPathMatch;
    private readonly string _instancePrefix;
    private readonly string _tracePath;
    private readonly byte[] _deviceDescriptor;
    private readonly byte[] _configurationDescriptor;
    private readonly byte[] _reportDescriptor;
    private readonly IntPtr _semaphore;
    private readonly IntPtr _removeEvent;
    private SafeFileHandle _file;
    private SafeFileHandle _bulkDeviceFile;
    private IntPtr _winUsbInterface;
    private Thread _worker;
    private volatile bool _stopping;
    private bool _added;
    private int _address;
    private int _configuration;
    private int _lastIoctlError;
    private readonly object _sync = new object();
    private readonly Dictionary<string, int> _counts = new Dictionary<string, int>();
    private readonly List<string> _errors = new List<string>();
    private int _traceRecords;
    private bool _malformedEndSent;
    private bool _malformedEndPending;
    private volatile bool _malformedEndObserved;
    private int _malformedEndIoctlError;
    private uint _pendingTransactionsAfterRemoval;
    private uint _pendingRemovalWorkItemsAfterRemoval;
    private bool _removalQuiescent;

    public string InstancePrefix { get { return _instancePrefix; } }
    public int Address { get { return _address; } }
    public string TracePath { get { return _tracePath; } }
    public bool RemovalQuiescent { get { return _removalQuiescent; } }
    public string RemovalStateSummary
    {
        get
        {
            return "PendingTransactions=" + _pendingTransactionsAfterRemoval +
                ", PendingRemovalWorkItems=" + _pendingRemovalWorkItemsAfterRemoval;
        }
    }

    public XusbUsbSpeedProbe(string speed)
    {
        _malformedEndMode = String.Equals(speed, "MalformedEnd", StringComparison.OrdinalIgnoreCase);
        _bulkMode = _malformedEndMode || String.Equals(speed, "Bulk", StringComparison.OrdinalIgnoreCase);
        _speed = String.Equals(speed, "Low", StringComparison.OrdinalIgnoreCase) ? 0 : 1;
        _productId = (_bulkMode ? 0x4000 : 0x5000) |
            ((Environment.TickCount ^ System.Diagnostics.Process.GetCurrentProcess().Id) & 0x0fff);
        _productIdString = _productId.ToString("X4");
        _productPathMatch = "vid_ffff&pid_" + _productIdString;
        _tracePath = Path.Combine(Path.GetTempPath(), (_malformedEndMode ? "ucx-malformed-end-probe-" : "ucx-bulk-probe-") + _productIdString + ".log");
        if (_bulkMode) File.WriteAllText(_tracePath, DateTime.UtcNow.ToString("o") + (_malformedEndMode ? " malformed-END probe started" : " bulk probe started") + Environment.NewLine);
        string pid = _productIdString;
        string suffix = _malformedEndMode ? "MALFORMED" : (_bulkMode ? "BULK" : (_speed == 0 ? "LOW" : "FULL"));
        _instancePrefix = "USB\\VID_FFFF&PID_" + pid;
        _deviceDescriptor = _bulkMode
            ? new byte[] { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xff, 0xff, (byte)_productId, (byte)(_productId >> 8), 0, 1, 1, 2, 3, 1 }
            : new byte[] {
                18, 1, 0x00, 0x02,
                0, 0, 0, (byte)(_speed == 0 ? 8 : 64),
                0xff, 0xff, (byte)_productId, (byte)(_productId >> 8),
                0, 1, 1, 2, 3, 1
            };
        _reportDescriptor = new byte[] {
            0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x05,0x07, 0x19,0xe0, 0x29,0xe7,
            0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02, 0x95,0x01,
            0x75,0x08, 0x81,0x01, 0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01,
            0x29,0x05, 0x91,0x02, 0x95,0x01, 0x75,0x03, 0x91,0x01, 0x95,0x06,
            0x75,0x08, 0x15,0x00, 0x25,0x65, 0x05,0x07, 0x19,0x00, 0x29,0x65,
            0x81,0x00, 0xc0
        };
        _configurationDescriptor = _bulkMode
            ? new byte[] {
                9, 2, 32, 0, 1, 1, 0, 0x80, 50,
                9, 4, 0, 0, 2, 0xff, 0, 0, 0,
                7, 5, 0x81, 2, 0x00, 0x02, 0,
                7, 5, 0x01, 2, 0x00, 0x02, 0
            }
            : new byte[] {
                9, 2, 34, 0, 1, 1, 0, 0xa0, 25,
                9, 4, 0, 0, 1, 3, 1, 1, 0,
                9, 0x21, 0x11, 0x01, 0, 1, 0x22, (byte)_reportDescriptor.Length, 0,
                7, 5, 0x81, 3, 8, 0, 10
            };
        _semaphore = CreateSemaphore(IntPtr.Zero, 0, Int32.MaxValue, null);
        _removeEvent = CreateEvent(IntPtr.Zero, true, false, null);
        if (_semaphore == IntPtr.Zero || _removeEvent == IntPtr.Zero)
            throw new InvalidOperationException("Could not create transaction synchronization objects: " + Marshal.GetLastWin32Error());
    }

    public void AddAndServe()
    {
        _file = OpenInterface();
        if (_file == null || _file.IsInvalid) throw new InvalidOperationException("Could not open the virtual USB control interface: " + Marshal.GetLastWin32Error());

        byte[] driverInfo = new byte[16];
        int returned;
        if (!Ioctl(0x226028, null, driverInfo, out returned) || returned < 16 || ReadInt32(driverInfo, 0) != 0x53534f46)
            throw new InvalidOperationException("The selected interface is not the reconstructed FOSS HCD.");
        if (_malformedEndMode) RequireQuiescentCounters("before adding the fixture");

        _stopping = false;
        _worker = new Thread(TransactionWorker);
        _worker.IsBackground = true;
        _worker.Start();

        byte[] request = new byte[_bulkMode ? CreateRequestBaseBytes : CreateRequestBytes];
        WriteInt64(request, 0, _semaphore.ToInt64());
        WriteInt64(request, 8, _removeEvent.ToInt64());
        WriteInt32(request, 16, 0);
        string pid = _productIdString;
        string kind = _bulkMode ? "BULK" : (_speed == 0 ? "LOW" : "FULL");
        WriteString(request, 20, 260, "USB\\VID_FFFF&PID_" + pid + "\\XUSB_" + kind + "_HID");
        WriteMultiString(request, 540, 260, _bulkMode
            ? new string[] { "USB\\VID_FFFF&PID_" + pid }
            : new string[] {
                "USB\\VID_FFFF&PID_" + pid,
                "USB\\Class_03&SubClass_01&Prot_01",
                "USB\\Class_03&SubClass_01",
                "USB\\Class_03"
            });
        WriteMultiString(request, 1060, 260, _bulkMode
            ? new string[] { "USB\\Class_FF" }
            : new string[] {
                "USB\\Class_03&SubClass_01&Prot_01",
                "USB\\Class_03&SubClass_01",
                "USB\\Class_03"
            });
        byte[] result = new byte[4];
        if (_bulkMode)
        {
            if (!Ioctl(IoctlAdd, request, result, out returned))
                throw new InvalidOperationException("High-speed bulk fixture add IOCTL failed: " + _lastIoctlError);
        }
        else
        {
            WriteInt32(request, CreateRequestBaseBytes, _speed);
            if (!Ioctl(IoctlAddWithSpeed, request, result, out returned))
                throw new InvalidOperationException("Low/full-speed add IOCTL failed: " + _lastIoctlError);
        }
        if (returned < 4) throw new InvalidOperationException("USB add IOCTL returned a short address response.");
        _address = ReadInt32(result, 0);
        _added = true;
        TraceBulk("PDO added at USB address " + _address);
    }

    public string Summary()
    {
        lock (_sync)
        {
            List<string> lines = new List<string>();
            foreach (KeyValuePair<string, int> entry in _counts)
                lines.Add(entry.Key + "=" + entry.Value);
            foreach (string error in _errors)
                lines.Add("error: " + error);
            return lines.Count == 0 ? "No USB transactions observed." : String.Join(Environment.NewLine, lines.ToArray());
        }
    }

    private void ReadUcxCounters(out uint pendingTransactions, out uint pendingRemovalWorkItems)
    {
        byte[] state = new byte[168];
        int returned;
        if (!Ioctl(0x22602c, null, state, out returned) || returned < state.Length)
            throw new InvalidOperationException("Could not read full UCX lifecycle diagnostics: " + _lastIoctlError);
        if (ReadInt32(state, 0) != 0x53554358 || ReadInt32(state, 4) != returned)
            throw new InvalidOperationException("UCX lifecycle diagnostics returned an unexpected signature or size.");
        pendingTransactions = BitConverter.ToUInt32(state, 156);
        pendingRemovalWorkItems = BitConverter.ToUInt32(state, 160);
    }

    private void RequireQuiescentCounters(string phase)
    {
        uint pendingTransactions, pendingRemovalWorkItems;
        ReadUcxCounters(out pendingTransactions, out pendingRemovalWorkItems);
        if (pendingTransactions != 0 || pendingRemovalWorkItems != 0)
            throw new InvalidOperationException(phase + " requires an idle HCD; observed PendingTransactions=" +
                pendingTransactions + ", PendingRemovalWorkItems=" + pendingRemovalWorkItems + ".");
    }

    public bool RemoveAndClose()
    {
        bool notified = false;
        try
        {
            CloseBulkInterface();
            if (_added && _file != null && !_file.IsInvalid)
            {
                int returned;
                bool removed = Ioctl(IoctlRemove, null, null, out returned);
                if (!removed) RecordError("remove IOCTL: " + _lastIoctlError);
                notified = WaitForSingleObject(_removeEvent, 30000) == 0;
                if (!notified) RecordError("remove notification timed out after 30 seconds");
            }
            if (_malformedEndMode && notified)
            {
                DateTime deadline = DateTime.UtcNow.AddSeconds(5);
                do
                {
                    try
                    {
                        ReadUcxCounters(out _pendingTransactionsAfterRemoval,
                            out _pendingRemovalWorkItemsAfterRemoval);
                        if (_pendingTransactionsAfterRemoval == 0 &&
                            _pendingRemovalWorkItemsAfterRemoval == 0)
                        {
                            _removalQuiescent = true;
                            break;
                        }
                    }
                    catch (Exception ex) { RecordError("post-removal state query: " + ex.Message); }
                    Thread.Sleep(25);
                }
                while (DateTime.UtcNow < deadline);
                if (!_removalQuiescent)
                    RecordError("removal event arrived but lifecycle counters did not drain: " + RemovalStateSummary);
            }
        }
        finally
        {
            CloseBulkInterface();
            _stopping = true;
            if (_worker != null) _worker.Join(5000);
            if (_file != null) { _file.Dispose(); _file = null; }
            if (_semaphore != IntPtr.Zero) CloseHandle(_semaphore);
            if (_removeEvent != IntPtr.Zero) CloseHandle(_removeEvent);
            _added = false;
        }
        return notified;
    }

    public void VerifyBulk()
    {
        if (!_bulkMode) throw new InvalidOperationException("The bulk probe was not selected.");
        TraceBulk("Waiting for WinUSB interface");
        Guid interfaceGuid = new Guid(BulkInterfaceGuid);
        DateTime deadline = DateTime.UtcNow.AddSeconds(30);
        while (DateTime.UtcNow < deadline)
        {
            SafeFileHandle device = OpenInterface(interfaceGuid, _productPathMatch);
            if (device != null && !device.IsInvalid)
            {
                IntPtr usbInterface;
                if (WinUsb_Initialize(device, out usbInterface))
                {
                    _bulkDeviceFile = device;
                    _winUsbInterface = usbInterface;
                    TraceBulk("WinUsb_Initialize succeeded");
                    break;
                }
                TraceBulk("WinUsb_Initialize failed: " + Marshal.GetLastWin32Error());
                device.Dispose();
            }
            Thread.Sleep(250);
        }
        if (_winUsbInterface == IntPtr.Zero)
            throw new InvalidOperationException("The WinUSB interface did not become available; verify the OS descriptors and PnP status.");

        UsbInterfaceDescriptor interfaceDescriptor = new UsbInterfaceDescriptor();
        if (!WinUsb_QueryInterfaceSettings(_winUsbInterface, 0, ref interfaceDescriptor))
            throw new InvalidOperationException("WinUSB could not read the interface descriptor: " + Marshal.GetLastWin32Error());
        TraceBulk("Interface descriptor reports " + interfaceDescriptor.NumberOfEndpoints + " endpoints");
        byte inputPipe = 0, outputPipe = 0;
        for (byte index = 0; index < interfaceDescriptor.NumberOfEndpoints; ++index)
        {
            WinUsbPipeInformation pipe = new WinUsbPipeInformation();
            if (!WinUsb_QueryPipe(_winUsbInterface, 0, index, ref pipe))
                throw new InvalidOperationException("WinUSB could not query endpoint " + index + ": " + Marshal.GetLastWin32Error());
            if (pipe.PipeType == 2 && pipe.PipeId == 0x81) inputPipe = pipe.PipeId;
            if (pipe.PipeType == 2 && pipe.PipeId == 0x01) outputPipe = pipe.PipeId;
            TraceBulk("Pipe " + index + " type=" + pipe.PipeType + " id=0x" + pipe.PipeId.ToString("X2") + " packet=" + pipe.MaximumPacketSize);
        }
        if (inputPipe == 0 || outputPipe == 0)
            throw new InvalidOperationException("WinUSB did not expose both expected bulk endpoints.");

        uint timeout = 5000;
        if (!WinUsb_SetPipePolicy(_winUsbInterface, inputPipe, 3, sizeof(uint), ref timeout) ||
            !WinUsb_SetPipePolicy(_winUsbInterface, outputPipe, 3, sizeof(uint), ref timeout))
            throw new InvalidOperationException("WinUSB could not set bounded transfer timeouts: " + Marshal.GetLastWin32Error());
        uint policyLength = sizeof(uint), inputTimeout = 0, outputTimeout = 0;
        if (!WinUsb_GetPipePolicy(_winUsbInterface, inputPipe, 3, ref policyLength, ref inputTimeout))
            throw new InvalidOperationException("WinUSB could not read back the bulk-IN transfer timeout: " + Marshal.GetLastWin32Error());
        policyLength = sizeof(uint);
        if (!WinUsb_GetPipePolicy(_winUsbInterface, outputPipe, 3, ref policyLength, ref outputTimeout))
            throw new InvalidOperationException("WinUSB could not read back the bulk-OUT transfer timeout: " + Marshal.GetLastWin32Error());
        TraceBulk("Transfer timeout policy readback: IN=" + inputTimeout + " ms, OUT=" + outputTimeout + " ms");
        if (inputTimeout != timeout || outputTimeout != timeout)
            throw new InvalidOperationException("WinUSB did not retain the expected 5000 ms transfer timeouts.");

        if (_malformedEndMode)
        {
            byte[] malformedInput = new byte[512];
            int transferError;
            System.Diagnostics.Stopwatch transferTimer = System.Diagnostics.Stopwatch.StartNew();
            try
            {
                TransferBulkPipe(inputPipe, malformedInput, true);
                throw new InvalidOperationException("The bulk-IN request unexpectedly succeeded after malformed END.");
            }
            catch (Win32Exception ex) { transferError = ex.NativeErrorCode; }
            transferTimer.Stop();
            if (transferError != 87)
                throw new InvalidOperationException("WinUSB completed the malformed-END request with error " + transferError + "; expected ERROR_INVALID_PARAMETER (87).");
            if (transferTimer.ElapsedMilliseconds >= 10000)
                throw new TimeoutException("The USB request completed only at or after the 10-second transfer deadline.");

            DateTime endDeadline = DateTime.UtcNow.AddSeconds(5);
            while (!_malformedEndObserved && DateTime.UtcNow < endDeadline) Thread.Sleep(10);
            if (!_malformedEndObserved)
                throw new InvalidOperationException("The malformed END IOCTL was not observed after the USB request completed.");
            if (_malformedEndIoctlError != 87)
                throw new InvalidOperationException("The malformed END IOCTL returned error " + _malformedEndIoctlError + "; expected ERROR_INVALID_PARAMETER (87).");

            RequireQuiescentCounters("after the malformed END request completed");
            Count("malformed END");
            TraceBulk("Malformed END completed with ERROR_INVALID_PARAMETER; WinUSB request also completed with ERROR_INVALID_PARAMETER after " + transferTimer.ElapsedMilliseconds + " ms");
            return;
        }

        byte[] input = new byte[512];
        TraceBulk("Starting bounded bulk-IN read");
        uint transferred = TransferBulkPipe(inputPipe, input, true);
        TraceBulk("Bulk-IN read completed with " + transferred + " bytes");
        if (transferred != input.Length)
            throw new InvalidOperationException("WinUSB bulk-IN returned " + transferred + " bytes; expected " + input.Length + ".");
        for (int index = 0; index < input.Length; ++index)
            if (input[index] != (byte)(0x5a ^ index))
                throw new InvalidOperationException("WinUSB bulk-IN payload mismatch at byte " + index + ".");

        byte[] output = new byte[16];
        for (int index = 0; index < output.Length; ++index) output[index] = (byte)(0xa0 + index);
        TraceBulk("Starting bounded bulk-OUT write");
        transferred = TransferBulkPipe(outputPipe, output, false);
        TraceBulk("Bulk-OUT write completed with " + transferred + " bytes");
        if (transferred != output.Length)
            throw new InvalidOperationException("WinUSB bulk-OUT transferred " + transferred + " bytes; expected " + output.Length + ".");
    }

    private uint TransferBulkPipe(byte pipeId, byte[] buffer, bool isRead)
    {
        const uint ErrorIoPending = 997;
        const uint WaitObject0 = 0;
        const uint WaitTimeout = 0x102;
        const uint TransferDeadlineMilliseconds = 10000;
        IntPtr eventHandle = CreateEvent(IntPtr.Zero, true, false, null);
        if (eventHandle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create a bulk-transfer event.");

        IntPtr overlapped = IntPtr.Zero;
        IntPtr nativeBuffer = IntPtr.Zero;
        bool requestPending = false;
        try
        {
            // OVERLAPPED WinUSB I/O outlives the P/Invoke call, so keep payload storage unmanaged until completion.
            nativeBuffer = Marshal.AllocHGlobal(buffer.Length);
            if (!isRead) Marshal.Copy(buffer, 0, nativeBuffer, buffer.Length);
            overlapped = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(NativeOverlapped)));
            NativeOverlapped nativeOverlapped = new NativeOverlapped();
            nativeOverlapped.EventHandle = eventHandle;
            Marshal.StructureToPtr(nativeOverlapped, overlapped, false);

            uint transferred;
            bool started = isRead
                ? WinUsb_ReadPipe(_winUsbInterface, pipeId, nativeBuffer, (uint)buffer.Length, out transferred, overlapped)
                : WinUsb_WritePipe(_winUsbInterface, pipeId, nativeBuffer, (uint)buffer.Length, out transferred, overlapped);
            if (!started)
            {
                int error = Marshal.GetLastWin32Error();
                if ((uint)error != ErrorIoPending)
                    throw new Win32Exception(error, "WinUSB bulk transfer could not be queued.");
                requestPending = true;
                TraceBulk((isRead ? "Bulk-IN" : "Bulk-OUT") + " pending asynchronously");

                uint wait = WaitForSingleObject(eventHandle, TransferDeadlineMilliseconds);
                if (wait == WaitTimeout)
                {
                    TraceBulk((isRead ? "Bulk-IN" : "Bulk-OUT") + " timed out; aborting the pipe");
                    bool aborted = WinUsb_AbortPipe(_winUsbInterface, pipeId);
                    int abortError = aborted ? 0 : Marshal.GetLastWin32Error();
                    TraceBulk("WinUsb_AbortPipe returned " + aborted + (aborted ? "" : " error=" + abortError));
                    wait = WaitForSingleObject(eventHandle, TransferDeadlineMilliseconds);
                    if (wait != WaitObject0)
                    {
                        TraceBulk("Canceled transfer did not signal its event; retaining OVERLAPPED and payload storage until process exit");
                        overlapped = IntPtr.Zero;
                        eventHandle = IntPtr.Zero;
                        throw new TimeoutException("Bulk transfer timed out and did not complete after pipe abort (wait=0x" + wait.ToString("X") + ").");
                    }
                    requestPending = false;
                    bool completedAfterAbort = WinUsb_GetOverlappedResult(_winUsbInterface, overlapped, out transferred, false);
                    int completionError = completedAfterAbort ? 0 : Marshal.GetLastWin32Error();
                    TraceBulk("Canceled bulk transfer completion: success=" + completedAfterAbort + " error=" + completionError);
                    throw new TimeoutException("Bulk transfer exceeded 10 seconds and was aborted.");
                }
                if (wait != WaitObject0)
                {
                    TraceBulk("WaitForSingleObject failed with 0x" + wait.ToString("X") + "; retaining OVERLAPPED and payload storage until process exit");
                    overlapped = IntPtr.Zero;
                    eventHandle = IntPtr.Zero;
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Waiting for the bulk transfer failed.");
                }
                requestPending = false;
            }

            if (!WinUsb_GetOverlappedResult(_winUsbInterface, overlapped, out transferred, false))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WinUSB bulk transfer completion failed.");
            if (transferred > buffer.Length)
                throw new InvalidOperationException("WinUSB reported more bytes than the supplied bulk-transfer buffer.");
            if (isRead && transferred != 0) Marshal.Copy(nativeBuffer, buffer, 0, (int)transferred);
            return transferred;
        }
        finally
        {
            if (!requestPending)
            {
                if (overlapped != IntPtr.Zero) Marshal.FreeHGlobal(overlapped);
                if (eventHandle != IntPtr.Zero) CloseHandle(eventHandle);
                if (nativeBuffer != IntPtr.Zero) Marshal.FreeHGlobal(nativeBuffer);
            }
        }
    }

    private void TraceBulk(string message)
    {
        if (!_bulkMode) return;
        if (Interlocked.Increment(ref _traceRecords) > 512) return;
        lock (_sync)
        {
            File.AppendAllText(_tracePath, DateTime.UtcNow.ToString("o") + " " + message + Environment.NewLine, Encoding.UTF8);
        }
    }

    private void CloseBulkInterface()
    {
        if (_winUsbInterface != IntPtr.Zero)
        {
            WinUsb_Free(_winUsbInterface);
            _winUsbInterface = IntPtr.Zero;
        }
        if (_bulkDeviceFile != null)
        {
            _bulkDeviceFile.Dispose();
            _bulkDeviceFile = null;
        }
    }

    private void TransactionWorker()
    {
        byte[] transaction = new byte[TransactionBufferBytes];
        while (!_stopping)
        {
            uint wait = WaitForSingleObject(_semaphore, 250);
            if (wait == 0x102) continue;
            if (wait != 0) { RecordError("semaphore wait status 0x" + wait.ToString("X")); break; }
            if (_bulkMode) TraceBulk("Transaction semaphore signaled");
            int received;
            if (!Ioctl(IoctlBegin, null, transaction, out received))
            {
                int error = _lastIoctlError;
                if (_bulkMode) TraceBulk("BEGIN IOCTL failed: " + error);
                if (error != 259 && error != 1168)
                {
                    RecordError("begin IOCTL: " + error);
                    _stopping = true;
                    return;
                }
                continue;
            }
            if (received < TransactionHeaderBytes)
            {
                RecordError("short BEGIN response: " + received);
                if (_bulkMode) TraceBulk("short BEGIN response; stopping responder so fixture removal can cancel outstanding work");
                _stopping = true;
                return;
            }
            int type = ReadInt32(transaction, 24);
            int capacity = ReadInt32(transaction, 32);
            if (_bulkMode)
            {
                string detail = type == 3
                    ? " transferType=" + ReadInt32(transaction, 40) + " endpoint=0x" + ReadInt32(transaction, 44).ToString("X2") + " in=" + (ReadInt32(transaction, 48) != 0)
                    : (type == 1 ? " requestType=" + ReadInt32(transaction, 40) + " descriptorType=" + ReadInt32(transaction, 44) + " index=0x" + ReadInt32(transaction, 52).ToString("X") : "");
                TraceBulk("BEGIN type=" + type + detail + " capacity=" + capacity);
            }
            if (capacity < 0 || capacity > transaction.Length - TransactionHeaderBytes)
            {
                RecordError("invalid transaction capacity: " + capacity);
                if (_bulkMode) TraceBulk("invalid transaction capacity; stopping responder so fixture removal can cancel outstanding work");
                _stopping = true;
                return;
            }
            int responseLength;
            int result;
            try { Process(transaction, type, capacity, out responseLength, out result); }
            catch (Exception ex)
            {
                responseLength = 0;
                result = 1;
                RecordError("transaction parser: " + ex.Message);
            }
            bool malformedEndTarget = _malformedEndPending;
            _malformedEndPending = false;
            if (_bulkMode) TraceBulk("BEGIN transaction type=" + type + " capacity=" + capacity + " response=" + responseLength + " result=" + result);
            WriteInt32(transaction, 28, result);
            WriteInt32(transaction, 32, responseLength);
            WriteInt32(transaction, 12, responseLength);
            WriteInt32(transaction, 16, result);
            if (malformedEndTarget)
            {
                if (responseLength <= 0)
                {
                    RecordError("target bulk-IN transaction returned no response bytes");
                    _stopping = true;
                    return;
                }
                /* Keep the outer length and input size valid; corrupt only the duplicated inner length. */
                WriteInt32(transaction, 32, responseLength - 1);
                TraceBulk("Injecting duplicate-length mismatch for transaction id=" +
                    BitConverter.ToUInt64(transaction, 0) + " sequence=" + ReadInt32(transaction, 8));
            }
            byte[] response = new byte[TransactionHeaderBytes + responseLength];
            Buffer.BlockCopy(transaction, 0, response, 0, response.Length);
            int ended;
            if (_bulkMode) TraceBulk("Sending END type=" + type + " responseBytes=" + responseLength + " result=" + result);
            if (!Ioctl(IoctlEnd, response, null, out ended))
            {
                if (_bulkMode) TraceBulk("END IOCTL failed: " + _lastIoctlError + "; stopping responder so fixture removal can cancel outstanding work");
                if (malformedEndTarget)
                {
                    _malformedEndIoctlError = _lastIoctlError;
                    _malformedEndObserved = true;
                    TraceBulk("Malformed END returned Win32 error " + _malformedEndIoctlError);
                    if (_malformedEndIoctlError != 87)
                        RecordError("malformed END returned unexpected error: " + _malformedEndIoctlError);
                }
                else RecordError("end IOCTL: " + _lastIoctlError);
                _stopping = true;
                return;
            }
            else if (_bulkMode) TraceBulk("END IOCTL completed for transaction type=" + type);
        }
    }

    private void Process(byte[] b, int type, int capacity, out int length, out int result)
    {
        byte[] data = null;
        length = 0;
        result = 0;
        if (type == 1)
        {
            int descriptorRequestType = ReadInt32(b, 40);
            int descriptorType = ReadInt32(b, 44);
            bool isRead = ReadInt32(b, 48) != 0;
            int index = ReadInt32(b, 52);
            int value = ReadInt32(b, 56);
            if (descriptorRequestType == 2 && descriptorType == 1)
            {
                _configuration = value == 0 ? 0 : 1;
                Count("select configuration");
            }
            else if (descriptorRequestType == 2 && descriptorType == 2)
            {
                Count("select interface");
            }
            else if (descriptorRequestType == 5 && descriptorType == 0)
            {
                int recipient = (index >> 8) & 0xff;
                int interfaceNumber = index & 0xff;
                int pageIndex = (index >> 16) & 0xff;
                if (isRead && recipient == 0 && interfaceNumber == 0 && pageIndex == 0 && value == 4)
                {
                    data = ExtendedCompatIdDescriptor();
                    Count("Microsoft OS feature descriptor 4");
                }
                else if (isRead && recipient == 1 && interfaceNumber == 0 && pageIndex == 0 && value == 5)
                {
                    data = ExtendedPropertiesDescriptor();
                    Count("Microsoft OS feature descriptor 5");
                }
                else
                {
                    result = 1;
                    Count("unsupported Microsoft OS feature descriptor");
                }
            }
            else if (isRead)
            {
                data = Descriptor(descriptorType, index, value);
                Count("descriptor " + descriptorType);
                if (data == null) result = 1;
            }
            else result = 1;
        }
        else if (type == 2)
        {
            int requestType = ReadInt32(b, 40);
            int request = ReadInt32(b, 52);
            bool isRead = ReadInt32(b, 48) != 0;
            Count("class/vendor request " + request);
            if (requestType == 1 && isRead)
            {
                if (request == 1) data = new byte[8];
                else if (request == 2 || request == 3) data = new byte[] { 0 };
                else result = 1;
            }
            else if (requestType != 1 || (request != 9 && request != 10 && request != 11)) result = 1;
        }
        else if (type == 3)
        {
            int transferType = ReadInt32(b, 40);
            int endpoint = ReadInt32(b, 44);
            bool isRead = ReadInt32(b, 48) != 0;
            if (transferType == 1)
            {
                byte[] setup = new byte[8];
                Buffer.BlockCopy(b, 52, setup, 0, 8);
                data = Control(setup, capacity);
                Count(string.Format("control 0x{0:X2} request 0x{1:X2} value 0x{2:X4} index 0x{3:X4} length {4}",
                    setup[0], setup[1], setup[2] | (setup[3] << 8), setup[4] | (setup[5] << 8),
                    setup[6] | (setup[7] << 8)));
                if (data == null) result = 1;
            }
            else if (transferType == 2 && _bulkMode && endpoint == 0x81 && isRead)
            {
                data = new byte[Math.Min(512, capacity)];
                for (int index = 0; index < data.Length; ++index) data[index] = (byte)(0x5a ^ index);
                Count("bulk IN");
                if (_malformedEndMode && !_malformedEndSent)
                {
                    _malformedEndSent = true;
                    _malformedEndPending = true;
                }
            }
            else if (transferType == 2 && _bulkMode && endpoint == 0x01 && !isRead)
            {
                bool valid = capacity == 16;
                for (int index = 0; valid && index < capacity; ++index)
                    valid = b[TransactionHeaderBytes + index] == (byte)(0xa0 + index);
                if (valid) Count("bulk OUT");
                else { result = 1; RecordError("bulk OUT payload did not match the sent pattern"); }
            }
            else if (transferType == 2 && !_bulkMode && endpoint == 0x81 && isRead)
            {
                data = new byte[Math.Min(8, capacity)];
                Count("interrupt IN");
            }
            else result = 1;
        }
        else if (type == 4)
        {
            Count("pipe reset");
        }
        else if (type == 5)
        {
            int statusType = ReadInt32(b, 40);
            if (statusType == 5) data = new byte[] { (byte)_configuration };
            else if (statusType == 6) data = new byte[] { 0 };
            else data = new byte[] { 0, 0 };
            Count("status request " + statusType);
        }
        else if (type == 6)
        {
            Count("feature request");
        }
        else
        {
            result = 1;
            Count("unsupported wire type " + type);
        }

        if (data != null)
        {
            int copy = Math.Min(capacity, data.Length);
            if (copy != 0) Buffer.BlockCopy(data, 0, b, TransactionHeaderBytes, copy);
            length = copy;
        }
        else if (result != 0) length = 0;
    }

    private byte[] Control(byte[] setup, int capacity)
    {
        int requestType = setup[0];
        int request = setup[1];
        int value = setup[2] | (setup[3] << 8);
        int index = setup[4] | (setup[5] << 8);
        int requested = setup[6] | (setup[7] << 8);
        if ((requestType & 0x60) == 0x40)
        {
            Count("vendor control request " + request);
            if (requestType == 0xc0 && request == 0xa5 && value == 0 && index == 4)
            {
                Count("Microsoft OS feature descriptor 4 via vendor request");
                return Clip(ExtendedCompatIdDescriptor(), Math.Min(capacity, requested));
            }
            if (requestType == 0xc1 && request == 0xa5 && value == 0 && index == 5)
            {
                Count("Microsoft OS feature descriptor 5 via vendor request");
                return Clip(ExtendedPropertiesDescriptor(), Math.Min(capacity, requested));
            }
            return null;
        }
        if ((requestType & 0x60) == 0x20)
        {
            if ((requestType & 0x80) != 0)
            {
                if (request == 1) return Clip(new byte[8], Math.Min(capacity, requested));
                if (request == 2 || request == 3) return Clip(new byte[] { 0 }, Math.Min(capacity, requested));
                return null;
            }
            return new byte[0];
        }
        if (request == 6 && (requestType & 0x80) != 0)
            return Clip(Descriptor((value >> 8) & 0xff, value & 0xff, index), Math.Min(capacity, requested));
        if (request == 9 && (requestType & 0x80) == 0)
        {
            _configuration = value == 0 ? 0 : 1;
            return new byte[0];
        }
        if (request == 8 && (requestType & 0x80) != 0)
            return Clip(new byte[] { (byte)_configuration }, Math.Min(capacity, requested));
        if (request == 10 && (requestType & 0x80) != 0)
            return Clip(new byte[] { 0 }, Math.Min(capacity, requested));
        if (request == 0 && (requestType & 0x80) != 0)
            return Clip(new byte[] { 0, 0 }, Math.Min(capacity, requested));
        if (request == 5 || request == 11 || request == 1 || request == 3)
            return new byte[0];
        return null;
    }

    private byte[] Descriptor(int type, int index, int language)
    {
        if (type == 1) return _deviceDescriptor;
        if (type == 2) return _configurationDescriptor;
        if (type == 3)
        {
            if (index == 0) return new byte[] { 4, 3, 9, 4 };
            if (index == 0xee)
            {
                Count("Microsoft OS string descriptor");
                return new byte[] {
                    18, 3, 0x4d, 0, 0x53, 0, 0x46, 0, 0x54, 0,
                    0x31, 0, 0x30, 0, 0x30, 0, 0xa5, 0
                };
            }
            if (index == 1) return StringDescriptor("Virtual Camera Project");
            if (index == 2) return StringDescriptor(_bulkMode ? "USB Bulk Probe" : "USB Speed Probe HID");
            if (index == 3) return StringDescriptor(_speed == 0 ? "LOW" : "FULL");
            return null;
        }
        if (type == 0x21) return new byte[] { 9, 0x21, 0x11, 0x01, 0, 1, 0x22, (byte)_reportDescriptor.Length, 0 };
        if (type == 0x22) return _reportDescriptor;
        return null;
    }

    private static byte[] StringDescriptor(string value)
    {
        byte[] text = Encoding.Unicode.GetBytes(value);
        byte[] descriptor = new byte[text.Length + 2];
        descriptor[0] = (byte)descriptor.Length;
        descriptor[1] = 3;
        Buffer.BlockCopy(text, 0, descriptor, 2, text.Length);
        return descriptor;
    }

    private byte[] ExtendedCompatIdDescriptor()
    {
        byte[] descriptor = new byte[40];
        WriteInt32(descriptor, 0, descriptor.Length);
        descriptor[4] = 0x00; descriptor[5] = 0x01; // bcdVersion = 1.00
        descriptor[6] = 0x04; descriptor[7] = 0x00; // wIndex = Extended Compat ID
        descriptor[8] = 1; // One function section, with no additional compatible ID.
        descriptor[16] = 0; // First interface number.
        descriptor[17] = 1; // Interface count.
        if (_bulkMode) Buffer.BlockCopy(Encoding.ASCII.GetBytes("WINUSB"), 0, descriptor, 18, 6);
        return descriptor;
    }

    private byte[] ExtendedPropertiesDescriptor()
    {
        if (!_bulkMode)
        {
            byte[] empty = new byte[10];
            WriteInt32(empty, 0, empty.Length);
            empty[4] = 0x00; empty[5] = 0x01;
            empty[6] = 0x05; empty[7] = 0x00;
            return empty;
        }

        byte[] propertyName = Encoding.Unicode.GetBytes("DeviceInterfaceGUID\0");
        byte[] propertyValue = Encoding.Unicode.GetBytes("{" + BulkInterfaceGuid + "}\0");
        const int propertySectionHeaderBytes = 10;
        int propertySectionBytes = propertySectionHeaderBytes + propertyName.Length + 4 + propertyValue.Length;
        byte[] descriptor = new byte[10 + propertySectionBytes];
        WriteInt32(descriptor, 0, descriptor.Length);
        descriptor[4] = 0x00; descriptor[5] = 0x01; // bcdVersion = 1.00
        descriptor[6] = 0x05; descriptor[7] = 0x00; // wIndex = Extended Properties
        descriptor[8] = 1; descriptor[9] = 0; // One DeviceInterfaceGUID property.
        int offset = 10;
        WriteInt32(descriptor, offset, propertySectionBytes);
        WriteInt32(descriptor, offset + 4, 1); // REG_SZ
        descriptor[offset + 8] = (byte)propertyName.Length;
        descriptor[offset + 9] = (byte)(propertyName.Length >> 8);
        Buffer.BlockCopy(propertyName, 0, descriptor, offset + propertySectionHeaderBytes, propertyName.Length);
        int valueLengthOffset = offset + propertySectionHeaderBytes + propertyName.Length;
        WriteInt32(descriptor, valueLengthOffset, propertyValue.Length);
        Buffer.BlockCopy(propertyValue, 0, descriptor, valueLengthOffset + 4, propertyValue.Length);
        return descriptor;
    }

    private static byte[] Clip(byte[] data, int maximum)
    {
        if (data == null) return null;
        int size = Math.Max(0, Math.Min(data.Length, maximum));
        byte[] clipped = new byte[size];
        if (size != 0) Buffer.BlockCopy(data, 0, clipped, 0, size);
        return clipped;
    }

    private static SafeFileHandle OpenInterface()
    {
        Guid guid = new Guid("ED3FB623-0C23-48B8-880A-9D1A07263FFD");
        return OpenInterface(guid, null);
    }

    private static SafeFileHandle OpenInterface(Guid guid, string pathMustContain)
    {
        IntPtr list = SetupDiGetClassDevs(ref guid, IntPtr.Zero, IntPtr.Zero, DigcfPresent | DigcfDeviceInterface);
        if (list == new IntPtr(-1)) return null;
        try
        {
            for (uint i = 0; ; ++i)
            {
                DeviceInterfaceData data = new DeviceInterfaceData();
                data.Size = Marshal.SizeOf(typeof(DeviceInterfaceData));
                if (!SetupDiEnumDeviceInterfaces(list, IntPtr.Zero, ref guid, i, ref data)) break;
                int required;
                SetupDiGetDeviceInterfaceDetail(list, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
                if (required < 8) continue;
                IntPtr detail = Marshal.AllocHGlobal(required);
                try
                {
                    Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(list, ref data, detail, required, out required, IntPtr.Zero)) continue;
                    string path = Marshal.PtrToStringUni(IntPtr.Add(detail, 4));
                    if (!String.IsNullOrEmpty(pathMustContain) &&
                        path.IndexOf(pathMustContain, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    uint flags = String.IsNullOrEmpty(pathMustContain) ? 0u : 0x40000000u; // WinUSB requires overlapped handles.
                    SafeFileHandle file = CreateFile(path, 0xc0000000, ShareReadWrite, IntPtr.Zero, OpenExisting, flags, IntPtr.Zero);
                    if (file != null && !file.IsInvalid) return file;
                    if (file != null) file.Dispose();
                }
                finally { Marshal.FreeHGlobal(detail); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(list); }
        return null;
    }

    private bool Ioctl(uint code, byte[] input, byte[] output, out int returned)
    {
        IntPtr inputBuffer = IntPtr.Zero;
        IntPtr outputBuffer = IntPtr.Zero;
        try
        {
            int inputBytes = input == null ? 0 : input.Length;
            int outputBytes = output == null ? 0 : output.Length;
            if (inputBytes != 0)
            {
                inputBuffer = Marshal.AllocHGlobal(inputBytes);
                Marshal.Copy(input, 0, inputBuffer, inputBytes);
            }
            if (outputBytes != 0) outputBuffer = Marshal.AllocHGlobal(outputBytes);
            bool success = DeviceIoControl(_file, code, inputBuffer, inputBytes, outputBuffer, outputBytes, out returned, IntPtr.Zero);
            _lastIoctlError = success ? 0 : Marshal.GetLastWin32Error();
            if (success && outputBytes != 0) Marshal.Copy(outputBuffer, output, 0, Math.Min(outputBytes, returned));
            return success;
        }
        finally
        {
            if (inputBuffer != IntPtr.Zero) Marshal.FreeHGlobal(inputBuffer);
            if (outputBuffer != IntPtr.Zero) Marshal.FreeHGlobal(outputBuffer);
        }
    }

    private static void WriteString(byte[] target, int offset, int chars, string value)
    {
        byte[] bytes = Encoding.Unicode.GetBytes(value);
        int length = Math.Min(bytes.Length, chars * 2 - 2);
        Buffer.BlockCopy(bytes, 0, target, offset, length);
    }

    private static void WriteMultiString(byte[] target, int offset, int chars, string[] values)
    {
        byte[] bytes = Encoding.Unicode.GetBytes(String.Join("\0", values) + "\0\0");
        int length = Math.Min(bytes.Length, chars * 2);
        Buffer.BlockCopy(bytes, 0, target, offset, length);
    }

    private static int ReadInt32(byte[] data, int offset) { return BitConverter.ToInt32(data, offset); }
    private static void WriteInt32(byte[] data, int offset, int value) { Buffer.BlockCopy(BitConverter.GetBytes(value), 0, data, offset, 4); }
    private static void WriteInt64(byte[] data, int offset, long value) { Buffer.BlockCopy(BitConverter.GetBytes(value), 0, data, offset, 8); }

    private void Count(string key)
    {
        lock (_sync)
        {
            int count;
            _counts.TryGetValue(key, out count);
            _counts[key] = count + 1;
        }
    }

    private void RecordError(string value)
    {
        lock (_sync) if (_errors.Count < 32) _errors.Add(value);
    }
}
'@

$probe = $null
try {
    $probe = New-Object XusbUsbSpeedProbe($Speed)
    $existing = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
        $_.InstanceId -like ($probe.InstancePrefix + '*')
    })
    if ($existing.Count -gt 0) {
        throw "A present test device already uses $($probe.InstancePrefix); remove or inspect it before running this probe."
    }
    $probe.AddAndServe()
    $fixtureName = if ($Speed -eq 'MalformedEnd') { 'WinUSB malformed-END' } elseif ($Speed -eq 'Bulk') { 'WinUSB bulk' } else { "$Speed-speed HID" }
    Write-Host ("Added {0} fixture at USB address {1}; instance prefix {2}" -f $fixtureName, $probe.Address, $probe.InstancePrefix)
    if ($Speed -in @('Bulk', 'MalformedEnd')) { Write-Host ("Bulk transaction trace: {0}" -f $probe.TracePath) }
    if ($Speed -in @('Bulk', 'MalformedEnd')) {
        try { $probe.VerifyBulk() }
        catch {
            Write-Host 'Bulk probe failed; transactions observed before cleanup:'
            Write-Host $probe.Summary()
            Write-Host ("Bulk transaction trace: {0}" -f $probe.TracePath)
            Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object { $_.InstanceId -like ($probe.InstancePrefix + '*') } |
                Select-Object Class,FriendlyName,Status,InstanceId | Format-Table -AutoSize
            throw
        }
    }
    if ($Speed -ne 'MalformedEnd') { Start-Sleep -Seconds $ObserveSeconds }

    Write-Host 'Observed USB transactions:'
    $summary = $probe.Summary()
    Write-Host $summary
    $nodes = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like ($probe.InstancePrefix + '*') })
    if ($nodes.Count -eq 0) { throw 'No matching present USB PnP node was found during the observation window.' }
    $nodes | Select-Object Class,FriendlyName,Status,InstanceId | Format-Table -AutoSize
    if (@($nodes | Where-Object Status -ne 'OK').Count -gt 0) { throw 'The test HID PnP node did not reach status OK.' }
    if ($Speed -eq 'MalformedEnd') {
        if ($summary -notmatch '(?m)^bulk IN=1\r?$' -or $summary -notmatch '(?m)^malformed END=1\r?$') {
            throw 'The probe did not send exactly one malformed END for one bulk-IN transaction.'
        }
        if ($summary -match '(?m)^error: ') { throw 'The malformed-END probe recorded an unexpected responder error.' }
    }
    elseif ($Speed -eq 'Bulk') {
        if ($summary -notmatch '(?m)^bulk IN=1\r?$' -or $summary -notmatch '(?m)^bulk OUT=1\r?$') {
            throw 'Bulk IN and OUT transactions did not both complete through the test responder.'
        }
    }
    else {
        $interruptMatch = [regex]::Match($summary, '(?m)^interrupt IN=(\d+)\r?$')
        if (-not $interruptMatch.Success) { throw 'No interrupt-IN request was observed.' }
        $interruptCount = [int]$interruptMatch.Groups[1].Value
        $minimumInterrupts = $ObserveSeconds * 50
        $maximumInterrupts = $ObserveSeconds * 150
        if ($interruptCount -lt $minimumInterrupts -or $interruptCount -gt $maximumInterrupts) {
            throw "Interrupt-IN rate $interruptCount over $ObserveSeconds seconds is outside the expected interval pacing range ($minimumInterrupts..$maximumInterrupts)."
        }
    }
    if ($summary -notmatch '(?m)^Microsoft OS feature descriptor 4(?: via vendor request)?=\d+\r?$') {
        throw 'Windows did not complete the Microsoft OS Extended Compat ID vendor-control transaction.'
    }
    if ($summary -notmatch '(?m)^Microsoft OS feature descriptor 5(?: via vendor request)?=\d+\r?$') {
        throw 'Windows did not complete the Microsoft OS Extended Properties vendor-control transaction.'
    }
    if ($summary -notmatch '(?m)^Microsoft OS string descriptor=\d+\r?$') {
        throw 'Windows did not retrieve the Microsoft OS string descriptor at index 0xEE.'
    }
}
finally {
    if ($null -ne $probe) {
        $cleanupTimer = [Diagnostics.Stopwatch]::StartNew()
        $notified = $probe.RemoveAndClose()
        $cleanupTimer.Stop()
        if ($notified) { Write-Host ("Driver signaled removal after PnP/transfer cleanup ({0} ms)." -f $cleanupTimer.ElapsedMilliseconds) }
        else { throw 'Removal notification did not arrive within 30 seconds.' }
        if ($Speed -eq 'MalformedEnd') {
            Write-Host ("Post-removal lifecycle state: {0}" -f $probe.RemovalStateSummary)
            if ($cleanupTimer.ElapsedMilliseconds -ge 5000) { throw 'Malformed-END fixture removal did not complete within 5 seconds.' }
            if (-not $probe.RemovalQuiescent) { throw 'Removal event was signaled before pending transactions/removal work items drained.' }
        }
        $remaining = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like ($probe.InstancePrefix + '*') })
        if ($remaining.Count -eq 0) { Write-Host 'No matching test device remains present.' }
        else {
            $remaining | Select-Object Class,FriendlyName,Status,InstanceId | Format-Table -AutoSize
            throw 'The test HID PnP node remained present after removal.'
        }
    }
}

[CmdletBinding()]
param(
    [ValidateRange(5, 90)]
    [int]$DeviceStartTimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'virtual-device-camera' -ErrorAction SilentlyContinue) {
    throw 'Stop virtual-device-camera before running this isolated USB control-path probe.'
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

public sealed class MsFeatureUrbProbe
{
    private const uint IoctlAdd = 0x222004;
    private const uint IoctlRemove = 0x222008;
    private const uint IoctlBegin = 0x222023;
    private const uint IoctlEnd = 0x222027;
    private const uint IoctlQueryDriverInfo = 0x226028;
    private const uint TestIoctl = 0x22e000;
    private const uint DigcfPresent = 0x2;
    private const uint DigcfDeviceInterface = 0x10;
    private const uint OpenExisting = 3;
    private const uint ShareReadWrite = 3;
    private const int CreateRequestBytes = 1584;
    private const int TransactionHeaderBytes = 64;
    private const int TransactionBufferBytes = 1024 * 1024;
    private const int FeatureResultBytes = 84;
    private const string ProductIdString = "7E41";
    private const string DeviceInstanceId = "USB\\VID_FFFF&PID_7E41\\XUSB_MS_FEATURE_URB";
    private const string DeviceInstancePrefix = "USB\\VID_FFFF&PID_7E41";
    private const string HcdInterfaceGuid = "ed3fb623-0c23-48b8-880a-9d1a07263ffd";
    private const string TestInterfaceGuid = "7d85244a-f4db-4b4c-9826-a08d73324c91";

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

    private readonly IntPtr _semaphore;
    private readonly IntPtr _removeEvent;
    private readonly object _sync = new object();
    private readonly Dictionary<string, int> _counts = new Dictionary<string, int>();
    private readonly List<string> _errors = new List<string>();
    private SafeFileHandle _hcdFile;
    private SafeFileHandle _testFile;
    private Thread _worker;
    private volatile bool _stopping;
    private bool _added;
    private int _address;
    private int _lastIoctlError;

    public string InstancePrefix { get { return DeviceInstancePrefix; } }
    public int Address { get { return _address; } }

    public MsFeatureUrbProbe()
    {
        _semaphore = CreateSemaphore(IntPtr.Zero, 0, Int32.MaxValue, null);
        _removeEvent = CreateEvent(IntPtr.Zero, true, false, null);
        if (_semaphore == IntPtr.Zero || _removeEvent == IntPtr.Zero)
            throw new InvalidOperationException("Could not create transaction synchronization objects: " + Marshal.GetLastWin32Error());
    }

    public void AddAndServe()
    {
        Guid guid = new Guid(HcdInterfaceGuid);
        _hcdFile = OpenInterface(guid, null);
        if (_hcdFile == null || _hcdFile.IsInvalid)
            throw new InvalidOperationException("Could not open the virtual USB control interface: " + Marshal.GetLastWin32Error());

        byte[] driverInfo = new byte[16];
        int returned;
        if (!Ioctl(_hcdFile, IoctlQueryDriverInfo, null, driverInfo, out returned) || returned < 16 || ReadInt32(driverInfo, 0) != 0x53534f46)
            throw new InvalidOperationException("The selected USB control interface is not the reconstructed FOSS HCD.");

        _stopping = false;
        _worker = new Thread(TransactionWorker);
        _worker.IsBackground = true;
        _worker.Start();

        byte[] request = new byte[CreateRequestBytes];
        WriteInt64(request, 0, _semaphore.ToInt64());
        WriteInt64(request, 8, _removeEvent.ToInt64());
        WriteInt32(request, 16, 0);
        WriteString(request, 20, 260, DeviceInstanceId);
        WriteMultiString(request, 540, 260, new string[] { "USB\\VID_FFFF&PID_" + ProductIdString });
        WriteMultiString(request, 1060, 260, new string[0]);
        byte[] result = new byte[4];
        if (!Ioctl(_hcdFile, IoctlAdd, request, result, out returned))
            throw new InvalidOperationException("Could not add the fixed MS feature URB fixture: " + _lastIoctlError);
        _added = true;
        if (returned < 4) throw new InvalidOperationException("USB add IOCTL returned a short address response.");
        _address = ReadInt32(result, 0);
    }

    public bool TryOpenTestInterface()
    {
        if (_testFile != null && !_testFile.IsInvalid) return true;
        Guid guid = new Guid(TestInterfaceGuid);
        SafeFileHandle file = OpenInterface(guid, null);
        if (file == null || file.IsInvalid)
        {
            if (file != null) file.Dispose();
            return false;
        }
        _testFile = file;
        return true;
    }

    public int[] Snapshot()
    {
        lock (_sync)
        {
            return new int[] {
                CountValue("URB feature descriptor 4 (device recipient)"),
                CountValue("URB feature descriptor 5 (interface recipient)"),
                CountValue("vendor-control OS feature descriptor 4"),
                CountValue("vendor-control OS feature descriptor 5")
            };
        }
    }

    public string Summary()
    {
        lock (_sync)
        {
            List<string> lines = new List<string>();
            foreach (KeyValuePair<string, int> entry in _counts) lines.Add(entry.Key + "=" + entry.Value);
            foreach (string error in _errors) lines.Add("error: " + error);
            return lines.Count == 0 ? "No USB transactions observed." : String.Join(Environment.NewLine, lines.ToArray());
        }
    }

    public string InvokeAndValidateClientIoctl()
    {
        if (!TryOpenTestInterface()) throw new InvalidOperationException("The KMDF client's test interface is not available.");
        byte[] output = new byte[FeatureResultBytes];
        int returned;
        if (!Ioctl(_testFile, TestIoctl, null, output, out returned))
            throw new InvalidOperationException("The test-client IOCTL failed: " + _lastIoctlError);
        if (returned != FeatureResultBytes)
            throw new InvalidOperationException("The test client returned an unexpected result size: " + returned);

        if (ReadInt32(output, 0) != FeatureResultBytes || ReadInt32(output, 4) != 1)
            throw new InvalidOperationException("The test client returned an invalid result header.");
        if (ReadInt32(output, 8) != 0 || ReadInt32(output, 12) != 0 || ReadInt32(output, 16) != 40)
            throw new InvalidOperationException("Descriptor 4 did not complete successfully with 40 bytes.");
        if (ReadInt32(output, 60) != 0 || ReadInt32(output, 64) != 0 || ReadInt32(output, 68) != 10)
            throw new InvalidOperationException("Descriptor 5 did not complete successfully with 10 bytes.");
        if (ReadInt32(output, 20) != 40 || output[24] != 0 || output[25] != 1 || output[26] != 4 || output[27] != 0 || output[28] != 1)
            throw new InvalidOperationException("Descriptor 4 bytes do not match a valid Extended Compat ID header.");
        if (ReadInt32(output, 72) != 10 || output[76] != 0 || output[77] != 1 || output[78] != 5 || output[79] != 0)
            throw new InvalidOperationException("Descriptor 5 bytes do not match a valid empty Extended Properties header.");
        return String.Format(
            "KMDF test IOCTL passed: index 4 NTSTATUS=0x{0:X8}, USBD_STATUS=0x{1:X8}, length={2}; index 5 NTSTATUS=0x{3:X8}, USBD_STATUS=0x{4:X8}, length={5}.",
            ReadInt32(output, 8), ReadInt32(output, 12), ReadInt32(output, 16),
            ReadInt32(output, 60), ReadInt32(output, 64), ReadInt32(output, 68));
    }

    public bool RemoveAndClose()
    {
        bool notified = false;
        try
        {
            if (_testFile != null) { _testFile.Dispose(); _testFile = null; }
            if (_added && _hcdFile != null && !_hcdFile.IsInvalid)
            {
                int returned;
                if (!Ioctl(_hcdFile, IoctlRemove, null, null, out returned))
                    RecordError("remove IOCTL: " + _lastIoctlError);
                notified = WaitForSingleObject(_removeEvent, 30000) == 0;
                if (!notified) RecordError("remove notification timed out after 30 seconds");
            }
        }
        finally
        {
            _stopping = true;
            if (_worker != null) _worker.Join(5000);
            if (_hcdFile != null) { _hcdFile.Dispose(); _hcdFile = null; }
            if (_semaphore != IntPtr.Zero) CloseHandle(_semaphore);
            if (_removeEvent != IntPtr.Zero) CloseHandle(_removeEvent);
            _added = false;
        }
        return notified;
    }

    private void TransactionWorker()
    {
        byte[] transaction = new byte[TransactionBufferBytes];
        while (!_stopping)
        {
            uint wait = WaitForSingleObject(_semaphore, 250);
            if (wait == 0x102) continue;
            if (wait != 0) { RecordError("semaphore wait status 0x" + wait.ToString("X")); break; }

            int received;
            if (!Ioctl(_hcdFile, IoctlBegin, null, transaction, out received))
            {
                int error = _lastIoctlError;
                if (error != 259 && error != 1168) RecordError("BEGIN IOCTL: " + error);
                if (error != 259 && error != 1168) { _stopping = true; return; }
                continue;
            }
            if (received < TransactionHeaderBytes)
            {
                RecordError("short BEGIN response: " + received);
                _stopping = true;
                return;
            }
            int type = ReadInt32(transaction, 24);
            int capacity = ReadInt32(transaction, 32);
            if (capacity < 0 || capacity > transaction.Length - TransactionHeaderBytes)
            {
                RecordError("invalid transaction capacity: " + capacity);
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
            WriteInt32(transaction, 28, result);
            WriteInt32(transaction, 32, responseLength);
            WriteInt32(transaction, 12, responseLength);
            WriteInt32(transaction, 16, result);
            byte[] response = new byte[TransactionHeaderBytes + responseLength];
            Buffer.BlockCopy(transaction, 0, response, 0, response.Length);
            int ended;
            if (!Ioctl(_hcdFile, IoctlEnd, response, null, out ended))
            {
                RecordError("END IOCTL: " + _lastIoctlError);
                _stopping = true;
                return;
            }
        }
    }

    private void Process(byte[] b, int type, int capacity, out int length, out int result)
    {
        byte[] data = null;
        length = 0;
        result = 0;
        if (type == 1)
        {
            int requestType = ReadInt32(b, 40);
            int descriptorType = ReadInt32(b, 44);
            bool isRead = ReadInt32(b, 48) != 0;
            int index = ReadInt32(b, 52);
            int value = ReadInt32(b, 56);
            if (requestType == 5 && descriptorType == 0)
            {
                int recipient = (index >> 8) & 0xff;
                int interfaceNumber = index & 0xff;
                int pageIndex = (index >> 16) & 0xff;
                if (isRead && recipient == 0 && interfaceNumber == 0 && pageIndex == 0 && value == 4)
                {
                    data = ExtendedCompatIdDescriptor();
                    Count("URB feature descriptor 4 (device recipient)");
                }
                else if (isRead && recipient == 1 && interfaceNumber == 0 && pageIndex == 0 && value == 5)
                {
                    data = ExtendedPropertiesDescriptor();
                    Count("URB feature descriptor 5 (interface recipient)");
                }
                else
                {
                    result = 1;
                    Count("unsupported URB feature descriptor");
                }
            }
            else if (requestType == 2 && descriptorType == 1)
            {
                Count("select configuration");
            }
            else if (requestType == 2 && descriptorType == 2)
            {
                Count("select interface");
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
            int requestKind = ReadInt32(b, 40);
            int subtype = ReadInt32(b, 44);
            bool isRead = ReadInt32(b, 48) != 0;
            int request = ReadInt32(b, 52);
            int index = ReadInt32(b, 56);
            if (requestKind == 2 && isRead && request == 0xa5 && (index == 4 || index == 5))
            {
                data = index == 4 ? ExtendedCompatIdDescriptor() : ExtendedPropertiesDescriptor();
                Count("vendor-control OS feature descriptor " + index);
            }
            else if (requestKind == 1 && isRead)
            {
                if (request == 1) data = new byte[8];
                else if (request == 2 || request == 3) data = new byte[] { 0 };
                else result = 1;
            }
            else if (requestKind != 1 || (request != 9 && request != 10 && request != 11) || subtype < 1 || subtype > 4)
                result = 1;
            Count("class/vendor request " + request);
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
                if (data == null) result = 1;
            }
            else if (transferType == 2 && endpoint == 0x81 && isRead)
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
            if (statusType == 5) data = new byte[] { 1 };
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
            if (requestType == 0xc0 && request == 0xa5 && value == 0 && index == 4)
            {
                Count("vendor-control OS feature descriptor 4");
                return Clip(ExtendedCompatIdDescriptor(), Math.Min(capacity, requested));
            }
            if (requestType == 0xc1 && request == 0xa5 && value == 0 && index == 5)
            {
                Count("vendor-control OS feature descriptor 5");
                return Clip(ExtendedPropertiesDescriptor(), Math.Min(capacity, requested));
            }
            Count("other vendor control request " + request);
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
        if (request == 9 && (requestType & 0x80) == 0) return new byte[0];
        if (request == 8 && (requestType & 0x80) != 0) return Clip(new byte[] { 1 }, Math.Min(capacity, requested));
        if (request == 10 && (requestType & 0x80) != 0) return Clip(new byte[] { 0 }, Math.Min(capacity, requested));
        if (request == 0 && (requestType & 0x80) != 0) return Clip(new byte[] { 0, 0 }, Math.Min(capacity, requested));
        if (request == 5 || request == 11 || request == 1 || request == 3) return new byte[0];
        return null;
    }

    private static byte[] Descriptor(int type, int index, int language)
    {
        if (type == 1)
            return new byte[] { 18, 1, 0, 2, 0, 0, 0, 64, 0xff, 0xff, 0x41, 0x7e, 0, 1, 1, 2, 0, 1 };
        if (type == 2)
            return new byte[] { 9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 0xff, 0, 0, 0 };
        if (type != 3) return null;
        if (index == 0) return new byte[] { 4, 3, 9, 4 };
        if (index == 0xee)
            return new byte[] { 18, 3, 0x4d, 0, 0x53, 0, 0x46, 0, 0x54, 0, 0x31, 0, 0x30, 0, 0x30, 0, 0xa5, 0 };
        if (index == 1) return StringDescriptor("VDC test-only fixture");
        if (index == 2) return StringDescriptor("MS Feature URB Probe");
        return null;
    }

    private static byte[] ExtendedCompatIdDescriptor()
    {
        byte[] descriptor = new byte[40];
        WriteInt32(descriptor, 0, descriptor.Length);
        descriptor[4] = 0; descriptor[5] = 1;
        descriptor[6] = 4; descriptor[7] = 0;
        descriptor[8] = 1;
        descriptor[16] = 0;
        descriptor[17] = 1;
        /* The function section intentionally contains no WINUSB compatible ID. */
        return descriptor;
    }

    private static byte[] ExtendedPropertiesDescriptor()
    {
        byte[] descriptor = new byte[10];
        WriteInt32(descriptor, 0, descriptor.Length);
        descriptor[4] = 0; descriptor[5] = 1;
        descriptor[6] = 5; descriptor[7] = 0;
        return descriptor;
    }

    private static byte[] Clip(byte[] data, int maximum)
    {
        if (data == null) return null;
        int length = Math.Min(data.Length, maximum);
        byte[] clipped = new byte[length];
        if (length != 0) Buffer.BlockCopy(data, 0, clipped, 0, length);
        return clipped;
    }

    private SafeFileHandle OpenInterface(Guid guid, string pathMustContain)
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
                    if (!String.IsNullOrEmpty(pathMustContain) && path.IndexOf(pathMustContain, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    SafeFileHandle file = CreateFile(path, 0xc0000000, ShareReadWrite, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
                    if (file != null && !file.IsInvalid) return file;
                    if (file != null) file.Dispose();
                }
                finally { Marshal.FreeHGlobal(detail); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(list); }
        return null;
    }

    private bool Ioctl(SafeFileHandle file, uint code, byte[] input, byte[] output, out int returned)
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
            bool success = DeviceIoControl(file, code, inputBuffer, inputBytes, outputBuffer, outputBytes, out returned, IntPtr.Zero);
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

    private void Count(string key)
    {
        lock (_sync)
        {
            int count;
            _counts.TryGetValue(key, out count);
            _counts[key] = count + 1;
        }
    }

    private int CountValue(string key)
    {
        int count;
        return _counts.TryGetValue(key, out count) ? count : 0;
    }

    private void RecordError(string value)
    {
        lock (_sync) if (_errors.Count < 32) _errors.Add(value);
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

    private static byte[] StringDescriptor(string value)
    {
        byte[] text = Encoding.Unicode.GetBytes(value);
        byte[] descriptor = new byte[text.Length + 2];
        descriptor[0] = (byte)descriptor.Length;
        descriptor[1] = 3;
        Buffer.BlockCopy(text, 0, descriptor, 2, text.Length);
        return descriptor;
    }

    private static int ReadInt32(byte[] data, int offset) { return BitConverter.ToInt32(data, offset); }
    private static void WriteInt32(byte[] data, int offset, int value) { Buffer.BlockCopy(BitConverter.GetBytes(value), 0, data, offset, 4); }
    private static void WriteInt64(byte[] data, int offset, long value) { Buffer.BlockCopy(BitConverter.GetBytes(value), 0, data, offset, 8); }
}
'@

$probe = $null
$fixturePrefix = 'USB\VID_FFFF&PID_7E41'
try {
    $existing = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -like ($fixturePrefix + '*') })
    if ($existing.Count -gt 0) {
        throw "A present device already uses $fixturePrefix. Inspect/remove it before running this test."
    }

    $probe = New-Object MsFeatureUrbProbe
    $probe.AddAndServe()
    Write-Host ("Added the MS OS feature test fixture at USB address {0}; waiting for its KMDF function driver." -f $probe.Address)

    $deadline = [DateTime]::UtcNow.AddSeconds($DeviceStartTimeoutSeconds)
    $testNode = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $testNode = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -like ($fixturePrefix + '*') } | Select-Object -First 1
        if ($null -ne $testNode -and $testNode.Status -eq 'OK' -and $probe.TryOpenTestInterface()) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($null -eq $testNode -or $testNode.Status -ne 'OK' -or -not $probe.TryOpenTestInterface()) {
        if ($null -ne $testNode) { $testNode | Format-List Class,FriendlyName,Status,Problem,InstanceId }
        throw "The test function driver/interface did not start within $DeviceStartTimeoutSeconds seconds. Stage the test-only INF for VID_FFFF&PID_7E41 first."
    }

    # Let PnP finish its own OS-string/vendor-control probing before taking the baseline.
    Start-Sleep -Milliseconds 500
    $before = $probe.Snapshot()
    Write-Host ("Function driver is active: {0}" -f $testNode.InstanceId)
    $ioctlResult = $probe.InvokeAndValidateClientIoctl()
    Write-Host $ioctlResult
    $after = $probe.Snapshot()
    $summary = $probe.Summary()
    Write-Host 'Observed transactions:'
    Write-Host $summary

    $direct4 = $after[0] - $before[0]
    $direct5 = $after[1] - $before[1]
    $vendor4 = $after[2] - $before[2]
    $vendor5 = $after[3] - $before[3]
    if (($direct4 + $vendor4) -ne 1 -or ($direct5 + $vendor5) -ne 1) {
        throw 'The client IOCTL did not produce exactly one recognized HCD request for each descriptor.'
    }
    Write-Host ("Descriptor 4 transport deltas: direct URB={0}, vendor control={1}." -f $direct4, $vendor4)
    Write-Host ("Descriptor 5 transport deltas: direct URB={0}, vendor control={1}." -f $direct5, $vendor5)
}
finally {
    if ($null -ne $probe) {
        $notified = $probe.RemoveAndClose()
        if (-not $notified) { throw 'The HCD did not signal fixture removal within 30 seconds.' }
        $remaining = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like ($fixturePrefix + '*') })
        if ($remaining.Count -ne 0) { throw 'The test fixture PnP node remained after removal.' }
        Write-Host 'The test fixture was removed and its PnP node disappeared.'
    }
}

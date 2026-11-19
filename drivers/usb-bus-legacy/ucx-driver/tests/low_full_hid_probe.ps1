[CmdletBinding()]
param(
    [ValidateSet('Low', 'Full')]
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
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public sealed class XusbUsbSpeedProbe
{
    private const uint IoctlAddWithSpeed = 0x222034;
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

    private readonly int _speed;
    private readonly string _instancePrefix;
    private readonly byte[] _deviceDescriptor;
    private readonly byte[] _configurationDescriptor;
    private readonly byte[] _reportDescriptor;
    private readonly IntPtr _semaphore;
    private readonly IntPtr _removeEvent;
    private SafeFileHandle _file;
    private Thread _worker;
    private volatile bool _stopping;
    private bool _added;
    private int _address;
    private int _configuration;
    private int _lastIoctlError;
    private readonly object _sync = new object();
    private readonly Dictionary<string, int> _counts = new Dictionary<string, int>();
    private readonly List<string> _errors = new List<string>();

    public string InstancePrefix { get { return _instancePrefix; } }
    public int Address { get { return _address; } }

    public XusbUsbSpeedProbe(string speed)
    {
        _speed = String.Equals(speed, "Low", StringComparison.OrdinalIgnoreCase) ? 0 : 1;
        string pid = _speed == 0 ? "1001" : "1002";
        string suffix = _speed == 0 ? "LOW" : "FULL";
        _instancePrefix = "USB\\VID_FFFF&PID_" + pid;
        _deviceDescriptor = new byte[] {
            18, 1, (byte)(_speed == 0 ? 0x10 : 0x00), (byte)(_speed == 0 ? 0x01 : 0x02),
            0, 0, 0, (byte)(_speed == 0 ? 8 : 64),
            0xff, 0xff, (byte)(_speed == 0 ? 0x01 : 0x02), 0x10,
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
        _configurationDescriptor = new byte[] {
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

        _stopping = false;
        _worker = new Thread(TransactionWorker);
        _worker.IsBackground = true;
        _worker.Start();

        byte[] request = new byte[CreateRequestBytes];
        WriteInt64(request, 0, _semaphore.ToInt64());
        WriteInt64(request, 8, _removeEvent.ToInt64());
        WriteInt32(request, 16, 0);
        string pid = _speed == 0 ? "1001" : "1002";
        string kind = _speed == 0 ? "LOW" : "FULL";
        WriteString(request, 20, 260, "USB\\VID_FFFF&PID_" + pid + "\\XUSB_" + kind + "_HID");
        WriteMultiString(request, 540, 260, new string[] {
            "USB\\VID_FFFF&PID_" + pid,
            "USB\\Class_03&SubClass_01&Prot_01",
            "USB\\Class_03&SubClass_01",
            "USB\\Class_03"
        });
        WriteMultiString(request, 1060, 260, new string[] {
            "USB\\Class_03&SubClass_01&Prot_01",
            "USB\\Class_03&SubClass_01",
            "USB\\Class_03"
        });
        WriteInt32(request, CreateRequestBaseBytes, _speed);
        byte[] result = new byte[4];
        if (!Ioctl(IoctlAddWithSpeed, request, result, out returned) || returned < 4)
            throw new InvalidOperationException("Low/full-speed add IOCTL failed: " + _lastIoctlError);
        _address = ReadInt32(result, 0);
        _added = true;
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

    public bool RemoveAndClose()
    {
        bool notified = false;
        try
        {
            if (_added && _file != null && !_file.IsInvalid)
            {
                int returned;
                bool removed = Ioctl(IoctlRemove, null, null, out returned);
                if (!removed) RecordError("remove IOCTL: " + _lastIoctlError);
                notified = WaitForSingleObject(_removeEvent, 30000) == 0;
                if (!notified) RecordError("remove notification timed out after 30 seconds");
            }
        }
        finally
        {
            _stopping = true;
            if (_worker != null) _worker.Join(5000);
            if (_file != null) { _file.Dispose(); _file = null; }
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
            if (!Ioctl(IoctlBegin, null, transaction, out received))
            {
                int error = _lastIoctlError;
                if (error != 259 && error != 1168) RecordError("begin IOCTL: " + error);
                continue;
            }
            if (received < TransactionHeaderBytes)
            {
                RecordError("short BEGIN response: " + received);
                continue;
            }
            int type = ReadInt32(transaction, 24);
            int capacity = ReadInt32(transaction, 32);
            if (capacity < 0 || capacity > transaction.Length - TransactionHeaderBytes)
            {
                RecordError("invalid transaction capacity: " + capacity);
                continue;
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
            if (!Ioctl(IoctlEnd, response, null, out ended))
                RecordError("end IOCTL: " + Marshal.GetLastWin32Error());
        }
    }

    private void Process(byte[] b, int type, int capacity, out int length, out int result)
    {
        byte[] data = null;
        length = 0;
        result = 0;
        if (type == 1)
        {
            int recipient = ReadInt32(b, 40);
            int descriptorType = ReadInt32(b, 44);
            bool isRead = ReadInt32(b, 48) != 0;
            int index = ReadInt32(b, 52);
            int value = ReadInt32(b, 56);
            if (recipient == 2 && descriptorType == 1)
            {
                _configuration = value == 0 ? 0 : 1;
                Count("select configuration");
            }
            else if (recipient == 2 && descriptorType == 2)
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
                Count("control request " + setup[1]);
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
            if (index == 1) return StringDescriptor("Virtual Camera Project");
            if (index == 2) return StringDescriptor("USB Speed Probe HID");
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
    Write-Host ("Added {0}-speed HID fixture at USB address {1}; instance prefix {2}" -f $Speed, $probe.Address, $probe.InstancePrefix)
    Start-Sleep -Seconds $ObserveSeconds

    $nodes = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like ($probe.InstancePrefix + '*') })
    if ($nodes.Count -eq 0) { throw 'No matching present USB PnP node was found during the observation window.' }
    $nodes | Select-Object Class,FriendlyName,Status,InstanceId | Format-Table -AutoSize
    if (@($nodes | Where-Object Status -ne 'OK').Count -gt 0) { throw 'The test HID PnP node did not reach status OK.' }
    Write-Host 'Observed USB transactions:'
    $summary = $probe.Summary()
    Write-Host $summary
    $interruptMatch = [regex]::Match($summary, '(?m)^interrupt IN=(\d+)$')
    if (-not $interruptMatch.Success) { throw 'No interrupt-IN request was observed.' }
    $interruptCount = [int]$interruptMatch.Groups[1].Value
    $minimumInterrupts = $ObserveSeconds * 50
    $maximumInterrupts = $ObserveSeconds * 150
    if ($interruptCount -lt $minimumInterrupts -or $interruptCount -gt $maximumInterrupts) {
        throw "Interrupt-IN rate $interruptCount over $ObserveSeconds seconds is outside the expected interval pacing range ($minimumInterrupts..$maximumInterrupts)."
    }
}
finally {
    if ($null -ne $probe) {
        $cleanupTimer = [Diagnostics.Stopwatch]::StartNew()
        $notified = $probe.RemoveAndClose()
        $cleanupTimer.Stop()
        if ($notified) { Write-Host ("Driver signaled removal after PnP/transfer cleanup ({0} ms)." -f $cleanupTimer.ElapsedMilliseconds) }
        else { throw 'Removal notification did not arrive within 30 seconds.' }
        $remaining = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like ($probe.InstancePrefix + '*') })
        if ($remaining.Count -eq 0) { Write-Host 'No matching test device remains present.' }
        else {
            $remaining | Select-Object Class,FriendlyName,Status,InstanceId | Format-Table -AutoSize
            throw 'The test HID PnP node remained present after removal.'
        }
    }
}

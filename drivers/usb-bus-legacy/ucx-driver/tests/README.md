# UCX HCD runtime probes

`low_full_hid_probe.ps1` is a Windows-only integration probe for the FOSS HCD's
USB 2.0 low/full-speed add path, HID enumeration, interrupt polling interval, and
device-removal cleanup. It requires the replacement HCD with periodic interrupt
pacing (2.0.0.34 or newer) and a loaded role-3/UCX device tree.

Run one speed at a time from PowerShell while `virtual-device-camera.exe` is
stopped:

```powershell
.\drivers\usb-bus-legacy\ucx-driver\tests\low_full_hid_probe.ps1 `
  -Speed Low -ObserveSeconds 5

.\drivers\usb-bus-legacy\ucx-driver\tests\low_full_hid_probe.ps1 `
  -Speed Full -ObserveSeconds 5
```

The probe first checks the selected control interface's FOSS identity, then adds
a synthetic HID keyboard with a 10 ms interrupt interval. It requires an `OK`
PnP node and an interrupt-IN rate in the expected 50–150 requests/second range.
Reports contain zero bytes; the probe does not emulate real keyboard input. It
removes the fixture in `finally`, requires the driver's removal notification,
and fails if the test PnP node remains present. Run it only on a development
machine where a temporary virtual HID node is expected.

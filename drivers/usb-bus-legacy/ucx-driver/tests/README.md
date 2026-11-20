# UCX HCD runtime probes

`low_full_hid_probe.ps1` is a Windows-only integration probe for the FOSS HCD's
USB 2.0 low/full-speed add path, HID enumeration, Microsoft OS 1.0 descriptors,
interrupt polling, bulk transactions, and device-removal cleanup. The probe requires
the replacement HCD with periodic interrupt pacing (2.0.0.34 or newer) and a loaded
role-3/UCX device tree. It verifies Microsoft OS feature-descriptor vendor-control
transactions with a test responder. `ms_feature_urb_probe.ps1` separately submits
`URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR` from a KMDF client and verifies the returned
descriptors end to end. Each HID/bulk run uses a fresh VID/PID so Windows will not
reuse a prior negative OS-string-descriptor result from the registry.

Runtime status (2026-11-21): role-3 tree 1.0.0.22 and HCD 2.0.0.41 are installed and
running without reboot. HCD `.41` fixes an `.40` isochronous capture regression: the
IN packet descriptor's `Length` is an output field, so writable capacity is derived
from packet offsets and the endpoint interval maximum. Release camera capture passed
at 1280×720 YUY2/30fps for 120 frames in 3.95 seconds; hot updates to 640×480 and back
also passed with 30 captured frames at each size. Camera removal drained pending
transactions and removal work items to zero. Earlier HCD 2.0.0.36 once bugchecked
with `0x3B` in deferred request completion because one UCX request lacked its transfer
context. HCD 2.0.0.37 added on-demand request-context allocation; 2.0.0.38 corrected
bulk-OUT completion length; 2.0.0.39 added USB 2.0 enable-change reporting and
tightened URB validation. The Low and Full HID probes and WinUSB 512-byte bulk IN / 16-byte
bulk OUT fixture were verified on `.39`.

One `0x144/3` bugcheck occurred while testing the specialized OS feature URB. The
minidump stack attributes the corrupt URB submission to the test-only
`UcxMsFeatureTestClient.sys`, not the production HCD: the fixture used a stack URB
despite registering `USBD_CLIENT_CONTRACT_VERSION_602`. The client now allocates URBs
with `WdfUsbTargetDeviceCreateUrb`. A rebuilt and test-signed client then retrieved
descriptor 4 (40 bytes) and descriptor 5 (10 bytes), both with successful NTSTATUS and
USBD status, and removed cleanly. Windows translated each request to one vendor-control
transfer before it reached the HCD; the direct HCD URB representation was not observed.
The fixture was removed from PnP and its test package was deleted from DriverStore.

Run one speed at a time from PowerShell while `virtual-device-camera.exe` is
stopped:

```powershell
.\drivers\usb-bus-legacy\ucx-driver\tests\low_full_hid_probe.ps1 `
  -Speed Low -ObserveSeconds 5

.\drivers\usb-bus-legacy\ucx-driver\tests\low_full_hid_probe.ps1 `
  -Speed Full -ObserveSeconds 5

.\drivers\usb-bus-legacy\ucx-driver\tests\low_full_hid_probe.ps1 `
  -Speed Bulk -ObserveSeconds 5

.\drivers\usb-bus-legacy\ucx-driver\tests\low_full_hid_probe.ps1 `
  -Speed MalformedEnd
```

`Low` and `Full` first check the selected control interface's FOSS identity, then add
a synthetic HID keyboard with a valid OS string descriptor at index `0xEE` and a
40-byte Extended Compat ID descriptor. Windows must retrieve the feature descriptors
through vendor requests. The fixture has an interrupt endpoint with a 10 ms interval;
the probe requires an `OK` PnP node and an interrupt-IN rate in the expected 50–150
requests/second range. Reports contain zero bytes, so it does not emulate keyboard
input.

`Bulk` adds a synthetic WinUSB device with bulk endpoints and Microsoft OS 1.0
descriptors. It performs one 512-byte bulk-IN read and one 16-byte bulk-OUT write,
checks the payloads and the user's BEGIN/END transactions, and requires the test device
to reach `OK`. Transfers are overlapped, have a 10-second deadline, and abort the pipe
when that deadline expires. The overlapped operation owns an unmanaged buffer until it
completes or is canceled. A per-run trace is written under `%TEMP%`.

`MalformedEnd` uses the same temporary WinUSB bulk fixture and responder, then submits
one 512-byte bulk-IN request. For that request only, the responder preserves the
readable END header, transaction ID, sequence, outer length, and input size, but changes
the duplicated inner `DataLength`. The probe requires the END IOCTL and WinUSB request
to complete with `ERROR_INVALID_PARAMETER`, and checks that pending transaction and
removal-work-item counts are zero before and after fixture removal. Removal must finish
within five seconds. This mode requires an idle HCD and a stopped
`virtual-device-camera.exe`; it creates and removes a temporary USB child but does not
install or replace drivers. Its trace records the matching transaction ID and sequence.

## Microsoft OS feature URB

`ms_feature_urb_probe.ps1` requires the separate test-only KMDF client under
`ms-feature-client/`. Build and statically validate it with
`ms-feature-client/build-and-verify.ps1 -Configuration Release`, sign its generated
catalog with a trusted development certificate, and stage its INF with
`pnputil /add-driver <package>\UcxMsFeatureTestClient.inf` (do not use `/install`).
Then run the probe from an elevated PowerShell while the camera app is stopped:

```powershell
.\drivers\usb-bus-legacy\ucx-driver\tests\ms_feature_urb_probe.ps1
```

The probe submits one descriptor-4 device request and one descriptor-5 interface
request through `WdfUsbTargetDeviceSendUrbSynchronously`. Windows may translate each
request into a vendor-control transfer before the HCD sees it, so the responder
accepts exactly one recognized direct URB representation or vendor-control
representation per descriptor. It validates the returned bytes, NTSTATUS and
USBD_STATUS. It also requires the fixture to disappear after removal. Afterward,
remove only the test client's staged `oemXX.inf` package if it is no longer needed;
the probe itself never installs or updates either production driver.

Every mode removes the fixture in `finally`, requires the driver's removal notification,
and fails if the test PnP node remains present. These probes validate the synthetic
responder and HCD transaction bridge; they do not establish that the production camera
program handles bulk/interrupt devices or that every USB class is supported. Run them
only on a development machine where a temporary virtual USB device is expected.

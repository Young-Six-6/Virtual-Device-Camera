# KMDF MS OS feature URB test client

This is an isolated test fixture for the FOSS UCX HCD. It is not part of either
production driver package and must not be installed on physical USB devices.

The function driver matches only the synthetic `USB\VID_FFFF&PID_7E41` ID emitted
by `ms_feature_urb_probe.ps1`. Its admin/SYSTEM-only device interface accepts one
test IOCTL. That IOCTL sends two synchronous KMDF USB-target URBs:

| Descriptor | Recipient | Interface | Page index | Expected bytes |
| --- | --- | ---: | ---: | ---: |
| 4, Extended Compat ID | Device | 0 | 0 | 40 |
| 5, Extended Properties | Interface | 0 | 0 | 10 |

Each request has a two-second timeout. The fixture advertises the Microsoft OS
1.0 string descriptor (`MSFT100`, vendor code `0xA5`), a 40-byte Extended Compat
ID descriptor with no compatible ID, and an empty 10-byte Extended Properties
descriptor. In particular, it does not advertise the `WINUSB` compatible ID.

The client registers `USBD_CLIENT_CONTRACT_VERSION_602`, so it allocates each URB
with `WdfUsbTargetDeviceCreateUrb` and deletes the returned WDF memory object
after the synchronous send completes. A stack-allocated URB is invalid for this
contract and can make the USB core bugcheck before the HCD receives the request;
keep this allocation rule if the fixture changes. See Microsoft's
[URB allocation guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-client-driver-contract-in-windows-8)
and [`WdfUsbTargetDeviceCreateUrb`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfusb/nf-wdfusb-wdfusbtargetdevicecreateurb).

## Build and static package validation

Run from a Developer PowerShell on a machine with the Windows 10 WDK, KMDF 1.31,
InfVerif, and Inf2Cat installed:

```powershell
.\build-and-verify.ps1 -Configuration Release
```

Build output and the generated unsigned catalog are kept under this directory's
ignored `build/` folder. The script builds x64 Release, runs InfVerif and Inf2Cat,
and does not sign, publish the package to the Windows Driver Store, install, or
start the driver.

## Runtime probe prerequisites

Runtime use is deliberately separate. It requires a development machine with the
replacement HCD and role-3 USB tree already active, Windows test signing enabled,
and this exact test client's INF staged and test-signed by the operator. Run
PowerShell as Administrator, stop `virtual-device-camera.exe`, then run:

```powershell
..\ms_feature_urb_probe.ps1
```

The probe adds the single fixed-ID synthetic device, services its USB transactions,
waits for the matching KMDF function driver, issues its test IOCTL, and removes the
fixture in `finally`. It requires an `OK` PnP node and removal notification. A reboot
is not part of the probe flow; stop if PnP reports that one is required.

The responder recognizes vendor-control OS descriptor requests so normal Windows
enumeration can finish. Windows may translate the KMDF client's
`URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR` request into the corresponding USB vendor
control transfer before it reaches the HCD. The probe therefore accepts exactly
one recognized direct URB request or vendor control request for each descriptor.
It also checks the descriptor contents and returned URB/USBD statuses. A passing
run demonstrates the descriptor request end to end through the Windows USB stack
and HCD, while its reported deltas show which representation reached the HCD.

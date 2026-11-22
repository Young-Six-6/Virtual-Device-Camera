# Combined role-3 + UCX role-2 replacement package

Current runtime validation (2026-11-22): role-3 tree 1.0.0.25 and HCD 2.0.0.49 run under the local test-signing certificate. The packages installed and bound without a Windows reboot; controller, tree, and USB 3 root hub report `OK`, UCX initialization statuses are zero, and the hub reports seven ports. DirectShow captured 60 frames of 1280×720 YUY2 at 30fps in 1.99 seconds. After orderly app shutdown, the virtual-camera PDO disappeared, pending transactions and removal work items were zero, and the root hub reported no invalid requests.

Historical ISO fix (`.41`): HCD `.40` regressed camera streaming because it treated `USBD_ISO_PACKET_DESCRIPTOR.Length` as the requested capacity for an IN packet. Windows defines this field as the actual length received from the host controller. HCD `.41` derives writable packet slices from the next packet offset (or transfer-buffer end), caps them by the endpoint service-interval limit, and validates returned lengths. This behavior matches [Microsoft's isochronous-transfer guidance](https://learn.microsoft.com/windows-hardware/drivers/usbcon/transfer-data-to-isochronous-endpoints); the fix remains in the current HCD.

On HCD `.48` / tree `.24`, Windows probes completed one 512-byte bulk IN, one 16-byte bulk OUT, low/full-speed HID interrupt-IN polling, and Microsoft OS 1.0 descriptors through vendor control; malformed END cleanup also removed cleanly. These were short functional probes, not stress or throughput tests, and have not been repeated on `.49/.25`. The direct `URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR` representation at the HCD boundary and every Root Hub power/link transition remain unverified. The default UVC responder does not implement arbitrary bulk/interrupt device classes; `vcam_create_ex` provides an application callback for declared bulk/interrupt endpoints, with unit coverage but no custom-device runtime test. Earlier full-rate camera figures below were measured on HCD `.34` and remain historical.

An earlier `0x144/3` crash was attributed by its minidump stack to the test-only KMDF client, which used a stack URB despite registering `USBD_CLIENT_CONTRACT_VERSION_602`. The client now uses `WdfUsbTargetDeviceCreateUrb`; the corrected descriptor probe removed its test node cleanly. The 2026-11-20 `0x144/3` dump is now analyzed: `!analyze -v` identifies `UcxMsFeatureTestClient.sys` as the bad-URB caller (`+0x1799`), with the synchronous URB send passing through UsbHub3 and UCX Root Hub before the bugcheck. SetupAPI shows HCD `.39` and tree `.20` were loaded then. This evidence points to the test-client path and does not attribute the crash to the production HCD. The dump had no private client symbols, so the exact source line remains unknown. No newer BugCheck 1001 is recorded as of 2026-11-22; see [driver crash notes](../../../docs/driver.md).

Historical `.44/.23` candidate: the build-only HCD added Root Hub reset/link bookkeeping, stricter request checks, reset notification cancellation handling, endpoint cancellation, and downstream-hub rejection. Those changes continued into later versions; current `.49/.25` camera enumeration, capture and teardown evidence is recorded above. This does not establish every Root Hub power state or unrelated USB class as supported.

Historical camera performance validation (2026-11-19): role-3 tree 1.0.0.18 and HCD 2.0.0.34 were installed and bound in the same Windows boot session using the local test-signing certificate. The virtual controller, its USB 3 root hub, and camera nodes reported OK. FFmpeg/DirectShow captured 120 frames of 1280×720 YUY2 from one stream in 3.93 seconds (about 30.5fps). Two concurrent streams each delivered 120 frames in 3.96 seconds (about 30.3fps per stream) with the test client's receive buffer sized to avoid startup drops. After the app stopped, both camera nodes disappeared and HCD pending transactions/removal work items returned to zero. No Windows restart was needed.

Read-only PnP/SCM snapshot from 2026-11-19: tree 1.0.0.18 and HCD 2.0.0.34 were in `OK` state, with both services running. After the capture run, no camera function node or stream remained; pending transactions and removal work items were zero. Later `.20/.39` probe results are recorded in the 2026-11-20 historical notes; current `.25/.49` status is at the top.

Tree 1.0.0.14 and 1.0.0.15 were assigned new versions after earlier image collisions. Tree 1.0.0.16 was then required because its rebuilt image differed from the image already loaded under the `.15` service name; advancing `DriverVer` let the deployment guard install it without reusing that resident image name.

HCD 2.0.0.34 keeps seven SuperSpeed device slots, validates endpoint descriptors, and paces periodic interrupt transactions according to endpoint `bInterval`. Low- and full-speed HID fixtures enumerated as `HIDClass` devices with status `OK`; over five seconds the low-speed fixture generated 498 interrupt-IN requests and the full-speed fixture 495. Removal notifications arrived in 772ms and 780ms respectively, and neither fixture left a PnP node. Repeated runs remained close to 100 requests/second per fixture. These fixtures send zero-filled keyboard reports and do not validate real keyboard input. The camera measurements exercise the software virtual SuperSpeed path, not a physical USB 3 controller, and do not establish complete compatibility with unrelated USB devices or every legacy protocol path.

This is a separate experimental deployment workflow. It combines:

- the reconstructed WDM role-3 enumerator (`root\*xusb_tree`), copied from
  `driver-source/package/experimental/xusb_reconstructed_tree.inf` and
  `driver-source/build/x64/<Configuration>/xusb_driver.sys`;
- the KMDF + UCX 1.1 role-2 host-controller client matching the PCI-style
  hardware ID created by role 3 (`VirtualCameraUcxHcd.sys`). The INF targets
  Windows 10 version 2004 (build 19041) or newer and KMDF 1.31.

The UCX HCD implements the root-hub, USB-device and frozen BEGIN/END transaction
paths used by the tested camera stream. It remains a replacement candidate for
unimplemented protocol paths; successful staging or PnP binding alone does not prove
that other USB devices or URB families work. The deploy script now
also requires the new HCD's own started USB root-hub child and exact running
image hashes before it removes the old control-interface providers. This keeps the
old provider available when the candidate fails to start.

Each tree/HCD DriverVer receives a distinct service and image name during packaging. This allows a new candidate to load while an earlier failed instance remains resident. Preserving upper KMDF BusRelations in the child PDO restored root-hub enumeration; same-version unload/reload still needs verification.

Legacy `IOCTL_INTERNAL_USB_CYCLE_PORT` targets the USB hub PDO. The reconstructed WDM
driver maps it to wire reset type 2, while the UCX HCD exposes root-hub port state and
models port-power off/on as a disconnect/reconnect. This may provide equivalent cycle
semantics, but the exact Windows hub-to-UCX callback sequence has not been verified.

The root hub advertises individual port power switching and tracks `PORT_POWER`
state independently of virtual-device slot allocation. Clearing port power hides
the connection and cancels outstanding transactions. The user-mode camera receives a
reset notification to clear its current frame position; setting power again reports a
fresh connection. This models the root-hub feature request itself, but does not establish
that Windows maps the legacy internal cycle-port IOCTL to this sequence.

## Build and stage

Run from an x64 Windows development machine with Visual Studio C++ tools and a WDK
containing x64 kernel libraries, UCX 1.1 headers/`ucxstub.lib`, and KMDF 1.31. The script
selects the newest compatible installed kit; pass `-WindowsTargetPlatformVersion` to pin
a specific compatible version:

```powershell
./replacement-package.ps1 -Configuration Release
```

The script builds both projects, stages the two SYS files and two INFs under
`build/x64/Release`, validates the INFs with InfVerif against Windows driver
rules for build 19041, then creates both catalogs with Inf2Cat. Both kernel
projects run WDK static analysis, and compiler warnings fail the build. Build
output is unsigned. Sign the package with the intended test/publisher certificate
after catalog creation. The helper takes an explicit certificate thumbprint and checks
the Code Signing EKU/private key. For the standard test-signing flow it signs both
catalogs and verifies each against its matching SYS. Add `-EmbedImages` to embed-sign
both SYS files first, regenerate the catalogs, then sign and verify the catalogs:

```powershell
./replacement-sign.ps1 -CertificateThumbprint <40-hex-thumbprint>
./replacement-sign.ps1 -CertificateThumbprint <40-hex-thumbprint> -EmbedImages
```

Keep the private signing key out of the repository and package. For development
signing, configure and trust a test certificate on a dedicated test machine according
to the applicable Windows driver-signing policy. Authenticode verification does not
prove the active kernel Code Integrity or boot policy will accept a test certificate.

## Install

Close `virtual-device-camera` before installation. `virt_dev.cpp::open_driver()`
prefers the exact reconstructed UCX HCD instance, then a FOSS provider, and only
falls back to another openable `GUID_XUSB_VIRT_INTERFACE` when neither is found.
The deployment script still removes known old USB-only and Root Hub provider
nodes before binding the replacement, then fails if one remains. This avoids a
legacy fallback being selected when the candidate HCD is absent or unavailable;
the old packages stay staged in DriverStore.

Open an elevated PowerShell window. The deployment script requires the x64
DevCon utility from the WDK tools to create the root-enumerated role-3 node.
It rejects any higher-version role-3 package already staged for the same INF, stages
the UCX INF, binds `root\*xusb_tree` to the exact requested service/version/image,
then asks PnPUtil to install the matching UCX HCD package and waits for its versioned
service binding and a started USB root-hub child (ROOT_HUB20 or ROOT_HUB30). It removes
obsolete reconstructed role-3/UCX packages and old control providers only after both
driver services point to the requested package images:

```powershell
./replacement-deploy.ps1 -Action Install
```

The catalogs must validate as trusted before the script proceeds. The package builder
derives a distinct HCD service/image name from `DriverVer`, so a stale previous HCD image
does not block the next debug build from loading. Older project role-3 and UCX packages
are pruned without `/uninstall` or `/force` after a successful bind. The cleanup keeps
the active and newer role-3 versions, checks present PnP INF bindings, and relies on
PnPUtil to reject anything Windows still considers in use. It only matches the project
provider and reconstructed INF names; original/legacy driver packages are left alone.
Each package version uses a distinct service and image name, so a new HCD can be
loaded while an earlier image remains resident. This avoids routine full reboots between
versioned debug builds. If PnPUtil returns `3010`, the deploy script now stops immediately
and identifies the command; it does not continue changing devices or reboot Windows.
Same-version unload and uninstall may still require separate recovery work.
Before any device change, the script compares both requested service names with existing
service image hashes and checks staged role-3 and UCX HCD packages for higher-version or
same-version service/image conflicts. If `Get-WindowsDriver` does not provide a usable FileRepository INF
path, it exports the published package to a temporary directory and hashes the SYS without
changing PnP state. Conflicts stop deployment before updating a device. Different bytes
under the same versioned name require an increased `DriverVer`. DevCon exit code 1 also
stops deployment immediately; PnP readiness polling cannot finish a reboot-pending installation.
The package also includes `query-ucx-state.ps1`; if the UCX controller starts but
its USB root hub does not appear, the deploy script prints the HCD's recorded
initialization statuses before it exits.

## SuperSpeed runtime status and remaining coverage

HCD 2.0.0.34 reserves seven virtual slots on USB 3 root-hub ports 17–23,
uses UCX's SuperSpeed device-speed indication, reports connection and reset state,
and checks the isochronous endpoint companion's burst/interval bounds. The camera
uses an additive FOSS-only add IOCTL, then emits USB 3.0 device/BOS descriptors and
a companion descriptor with `bMaxBurst=7` and `wBytesPerInterval=8,192` bytes per
125-microsecond interval. Existing IOCTLs and transaction wire layout stay unchanged;
older and original drivers reject the additive request and continue through USB 2.0.

The verified 2.0.0.34 path uses an 8,192-byte interval budget, providing about 65.5 MB/s after the two-byte UVC header,
enough for 1280×720 YUY2 at 30fps (about 55.3 MB/s). DirectShow captured 120 frames from one camera in
about 3.97 seconds (about 30fps); two simultaneous camera streams also completed a 120-frame request per stream in about 3.97 seconds. USB 2.0 high-speed isochronous YUY2 remains capped at
3×1,024 bytes per microframe, about 13.3fps at 720p; MJPEG reduces that bandwidth need
and previously reached about 25fps on this machine. These figures describe this tested
camera path, not physical USB 3 throughput or compatibility with every USB device.
The low/full-speed extension's HID enumeration, interrupt-IN, Microsoft OS 1.0 vendor
control requests, and removal cleanup have since passed on HCD 2.0.0.39. This does not
validate useful keyboard input, every endpoint type, or arbitrary HID compatibility.

## Uninstall

```powershell
./replacement-deploy.ps1 -Action Uninstall
```

Uninstall removes matching role-2 host-controller nodes and role-3 tree nodes
(including the root tree subtree), plus any remaining legacy GUID provider
nodes. It leaves old and new packages in the Windows Driver Store. The old
root enumerator is not automatically recreated: for rollback, close the app,
reinstall the preserved original INF, and recreate its root device, for example
with DevCon using the original `xusb_bus.inf` and `root\*xusb_enumbus` ID.
Restore the old Host Controller/Root Hub topology from the original `xusb_ctrl.inf`
only when that was the configuration previously in use. Remove corresponding
`oem*.inf` packages separately only after confirming they are no longer used.

## Clean up older project packages

To prune older reconstructed packages without reinstalling the current build, use
the explicit cleanup action from an elevated PowerShell window while the application
is closed:

```powershell
./replacement-deploy.ps1 -Action Cleanup
```

Cleanup requires the current reconstructed tree and UCX controller to be present and
in `OK` state. It preserves packages at or above the active tree version, keeps any
package referenced by a present PnP device, and asks PnPUtil to delete only older,
inactive project packages without `/uninstall` or `/force`. If PnPUtil reports that a
reboot is required, the script stops and does not reboot. A package that Windows refuses
to remove is retained for later inspection. Before attempting removal, the script exports
each package into the ignored `replacement-package/build/stale-package-backup/` directory;
if export fails, that package is left in DriverStore.

## Compatibility notes

- This workflow targets x64 Windows 10 2004 (19041) and newer.
- The role-3/role-2 hardware IDs intentionally match the current reconstruction.
- The old x86 package remains untouched.
- Do not use this as a replacement path for the legacy root hub, USB-only bus,
  or physical host-controller package.

The initialization query supports the prior 64-, 104- and 132-byte responses and a 168-byte lifecycle response. The latest tail records hardware release start/completion counts, D0 entry/exit counts, power/stopping state, pending transactions/removal work and the user queue state. The query script displays unavailable lifecycle fields as blank for older drivers. Snapshots are approximate, and the normal user queue is purged during removal, so this interface cannot be used to poll a stack that is already stalled inside teardown. Earlier 2.0.0.17/132-byte and 2.0.0.22 observations are historical; current runtime evidence is recorded at the top of this document.

# Combined role-3 + UCX role-2 replacement package

Latest runtime measurement (2026-11-18): tree 1.0.0.16 and HCD 2.0.0.32 were installed and bound in the same Windows boot session using a local test-signing certificate. The controller, USB 3 root hub, and SuperSpeed camera nodes reported OK. DirectShow captured 120 frames of 1280×720 YUY2 in about 3.97 seconds (about 30fps), both for one camera and for two simultaneous camera streams. After the app stopped, both camera nodes disappeared and pending transactions/removal work items returned to zero without restarting Windows. This validates these camera stream cases, not every legacy URB or full production replacement behavior. The earlier tree 1.0.0.11/HCD 2.0.0.28 capture is historical; the earlier USB 2.0 MJPEG result (about 25fps) is also historical.

Latest read-only PnP/SCM query (2026-11-18) reports tree 1.0.0.16 and HCD 2.0.0.32 in `OK` state, with both services running. The camera application had been stopped, so no camera function node or stream remained; pending transactions and removal work items were zero. No system restart was needed for the update.

Tree 1.0.0.14 and 1.0.0.15 were assigned new versions after earlier image collisions. Tree 1.0.0.16 was then required because its rebuilt image differed from the image already loaded under the `.15` service name; advancing `DriverVer` let the deployment guard install it without reusing that resident image name.

HCD 2.0.0.32 is installed and runtime-verified for single- and dual-camera SuperSpeed capture. It keeps seven SuperSpeed device slots, validates endpoint descriptors, and adds a low/full-speed USB 2.0 add IOCTL. A temporary caller verified that low/full add/remove IOCTLs return successfully and signal removal; it had no USB transaction responder, so no PnP device or stream was enumerated. Runtime throughput claims below refer to the tested SuperSpeed camera path. This is not a physical USB 3 controller benchmark and does not establish complete compatibility with unrelated USB devices or every legacy protocol path.

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

Close `virtual-device-camera` before installation. Its unchanged
`virt_dev.cpp::open_driver()` implementation picks the first openable
`GUID_XUSB_VIRT_INTERFACE`; leaving an old USB-only enumerator or Root Hub node
present could make it attach to the legacy device instead of the UCX HCD. The
script checks the process is closed, removes those known old provider device
nodes (it does not delete their DriverStore packages), and fails if one remains.

Open an elevated PowerShell window. The deployment script requires the x64
DevCon utility from the WDK tools to create the root-enumerated role-3 node.
It rejects any higher-version role-3 package already staged for the same INF, stages
the UCX INF, binds `root\*xusb_tree` to the exact requested service/version/image,
then asks PnPUtil to install the matching UCX HCD package and waits for its versioned
service binding and a started USB root-hub child (ROOT_HUB20 or ROOT_HUB30). It removes
obsolete UCX packages and old control providers only after both driver services point
to the requested package images:

```powershell
./replacement-deploy.ps1 -Action Install
```

The catalogs must validate as trusted before the script proceeds. The package builder
derives a distinct HCD service/image name from `DriverVer`, so a stale previous HCD image
does not block the next debug build from loading. Old project UCX packages are pruned
without `/force` after a successful bind; images Windows still considers in use are left
alone. Each package version uses a distinct service and image name, so a new HCD can be
loaded while an earlier image remains resident. This avoids routine full reboots between
versioned debug builds. If PnPUtil returns `3010`, the deploy script now stops immediately
and identifies the command; it does not continue changing devices or reboot Windows.
Same-version unload and uninstall may still require separate recovery work.
Before any device change, the script compares both requested service names with existing
service image hashes and checks staged role-3 packages for higher-version or same-version
service/image conflicts. If `Get-WindowsDriver` does not provide a usable FileRepository INF
path, it exports the published package to a temporary directory and hashes the SYS without
changing PnP state. Conflicts stop deployment before updating a device. Different bytes
under the same versioned name require an increased `DriverVer`. DevCon exit code 1 also
stops deployment immediately; PnP readiness polling cannot finish a reboot-pending installation.
The package also includes `query-ucx-state.ps1`; if the UCX controller starts but
its USB root hub does not appear, the deploy script prints the HCD's recorded
initialization statuses before it exits.

## SuperSpeed runtime status and remaining coverage

The installed 2.0.0.32 HCD reserves seven virtual slots on USB 3 root-hub ports 17–23,
uses UCX's SuperSpeed device-speed indication, reports connection and reset state,
and checks the isochronous endpoint companion's burst/interval bounds. The camera
uses an additive FOSS-only add IOCTL, then emits USB 3.0 device/BOS descriptors and
a companion descriptor with `bMaxBurst=7` and `wBytesPerInterval=8,192` bytes per
125-microsecond interval. Existing IOCTLs and transaction wire layout stay unchanged;
older and original drivers reject the additive request and continue through USB 2.0.

The verified 2.0.0.32 path uses an 8,192-byte interval budget, providing about 65.5 MB/s after the two-byte UVC header,
enough for 1280×720 YUY2 at 30fps (about 55.3 MB/s). DirectShow captured 120 frames from one camera in
about 3.97 seconds (about 30fps); two simultaneous camera streams also completed a 120-frame request per stream in about 3.97 seconds. USB 2.0 high-speed isochronous YUY2 remains capped at
3×1,024 bytes per microframe, about 13.3fps at 720p; MJPEG reduces that bandwidth need
and previously reached about 25fps on this machine. These figures describe this tested
camera path, not physical USB 3 throughput or compatibility with every USB device.
The low/full-speed extension has passed add/remove smoke checks only; enumeration,
endpoint callbacks, and transfer behavior remain unverified.

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

## Compatibility notes

- This workflow targets x64 Windows 10 2004 (19041) and newer.
- The role-3/role-2 hardware IDs intentionally match the current reconstruction.
- The old x86 package remains untouched.
- Do not use this as a replacement path for the legacy root hub, USB-only bus,
  or physical host-controller package.

The initialization query supports the prior 64-, 104- and 132-byte responses and a 168-byte lifecycle response. The latest tail records hardware release start/completion counts, D0 entry/exit counts, power/stopping state, pending transactions/removal work and the user queue state. The query script displays unavailable lifecycle fields as blank for older drivers. Snapshots are approximate, and the normal user queue is purged during removal, so this interface cannot be used to poll a stack that is already stalled inside teardown. Earlier 2.0.0.17/132-byte and 2.0.0.22 observations are historical; current runtime evidence is recorded at the top of this document.

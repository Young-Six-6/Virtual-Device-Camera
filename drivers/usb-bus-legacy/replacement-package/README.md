# Combined role-3 + UCX role-2 replacement package

Current machine evidence (2026-11-17): tree 1.0.0.11 and HCD 2.0.0.22 are bound in the same Windows boot session. The controller, virtual root hub, and active camera node report OK. DirectShow enumerates `MP4 HD Camera 1` with YUY2 and MJPEG modes; 1280×720 MJPEG capture was requested at 30fps and delivered 60 frames at about 25fps. Stopping the application removes its camera node and drains pending HCD transactions without restarting Windows. This validates the tested camera path, not every legacy URB or full production replacement behavior.

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
also requires the new HCD's own started USB 2.0 root-hub child and exact running
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

Run from an x64 Windows development machine with Visual Studio C++ tools, the
matching WDK/SDK and UCX 1.1 headers/libraries installed:

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
It stages the UCX INF first, creates/installs `root\*xusb_tree`, waits for the
role-2 child, then asks PnPUtil to install the matching UCX HCD package and waits for
its versioned UCX HCD service binding and a started USB root-hub child (ROOT_HUB20 or ROOT_HUB30). Only after both
driver services point to the requested package images does it remove old control
providers:

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
service image hashes. Different bytes under the same versioned name are rejected with
instructions to increase the corresponding `DriverVer`. DevCon exit code 1 also stops
deployment immediately; PnP readiness polling cannot finish a reboot-pending installation.
The package also includes `query-ucx-state.ps1`; if the UCX controller starts but
its USB 2.0 root hub does not appear, the deploy script prints the HCD's recorded
initialization statuses before it exits.

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

The initialization query supports the prior 64-, 104- and 132-byte responses and a 168-byte lifecycle response. The latest tail records hardware release start/completion counts, D0 entry/exit counts, power/stopping state, pending transactions/removal work and the user queue state. The query script displays unavailable lifecycle fields as blank for older drivers. Snapshots are approximate, and the normal user queue is purged during removal, so this interface cannot be used to poll a stack that is already stalled inside teardown. Earlier 2.0.0.17/132-byte observations are historical and are superseded by the 2.0.0.22 runtime evidence above.

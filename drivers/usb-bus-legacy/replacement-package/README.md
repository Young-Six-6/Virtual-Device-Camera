# Combined role-3 + UCX role-2 replacement package

This is a separate experimental deployment workflow. It combines:

- the reconstructed WDM role-3 enumerator (`root\*xusb_tree`), copied from
  `driver-source/package/experimental/xusb_reconstructed_tree.inf` and
  `driver-source/build/x64/<Configuration>/xusb_driver.sys`;
- the KMDF + UCX 1.1 role-2 host-controller client matching the PCI-style
  hardware ID created by role 3 (`VirtualCameraUcxHcd.sys`). The INF targets
  Windows 10 version 2004 (build 19041) or newer and KMDF 1.31.

The UCX HCD now implements the root-hub, USB-device, control/isochronous transfer,
and frozen BEGIN/END transaction paths used by the current UVC application. It is
a replacement candidate, but successful staging or PnP binding does not prove
that USB descriptors enumerate or camera streaming works. The deploy script only
reports device-node presence and service binding; no live Windows USB/UVC
end-to-end validation has been performed. Bulk/interrupt and several standalone
URB types remain outside both this bridge and the current user-mode UVC handler.

## Build and stage

Run from an x64 Windows development machine with Visual Studio C++ tools, the
matching WDK/SDK and UCX 1.1 headers/libraries installed:

```powershell
./replacement-package.ps1 -Configuration Release
```

The script builds both projects, stages the two SYS files and two INFs under
`build/x64/Release`, then creates both catalogs with Inf2Cat. Build output is
unsigned. Sign both `.cat` files with the intended publisher certificate after
catalog creation. Keep the private signing key out of the repository and
package. For development signing, configure and trust a test certificate on a
dedicated test machine according to the applicable Windows driver-signing
policy; do not change production boot policy to install an unsigned driver.

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
role-2 child, then asks PnPUtil to install the matching UCX HCD package and
waits for its `ucx_hcd` service binding:

```powershell
./replacement-deploy.ps1 -Action Install
```

The catalogs must validate as trusted before the script proceeds. Windows may
still require a reboot for driver replacement or removal to finish.

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

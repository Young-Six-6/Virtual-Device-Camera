# Virtual Camera UCX host controller

This is an isolated KMDF + UCX 1.1 host-controller project. It targets the UCX 1.1
DDI surface, pins KMDF 1.31, and is intended for Windows 10 version 2004
(build 19041) or newer.
The UCX 1.1 WDK import library is `ucxstub.lib`; the runtime must provide the same
UCX major version and a minor version greater than or equal to the client driver's.
The local machine has the 19041 UCX runtime (`Ucx01000.sys` file version 10.0.19041.1),
but that file version alone is not the UCX interface version contract. The OS/WDK
compatibility target is based on Microsoft's UCX versioning guidance.

## Implemented paths

- KMDF driver entry, role-2 HCD INF matching the legacy role-3 PDO hardware ID
  `PCI\\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88`, UCX controller creation, root hub object,
  USB-device creation callback, endpoint creation callbacks, and per-endpoint parallel
  WDF queues wired with `UcxEndpointSetWdfIoQueue`.
- Endpoint queue callback retrieves the URB from
  `WDF_REQUEST_PARAMETERS.Parameters.Others.Arg1` for
  `IOCTL_INTERNAL_USB_SUBMIT_URB`.
- Exposes the existing `GUID_XUSB_VIRT_INTERFACE` and the frozen legacy PDO add/remove
  and FOSS identity query IOCTL codes.
- Maintains up to 16 dynamic USB 2.0 root-hub ports with per-open-file slots, unique
  USB addresses, connection/reset status and change bits, hub/port status requests,
  and the hub interrupt bitmap.
- References the semaphore and remove event during `EvtIoInCallerContext`, retains
  them for the slot lifetime, and delays removal notification until the slot's root-hub
  change is acknowledged and USB-device/transfer references drain.
- Bridges the frozen BEGIN/END transaction ABI for control, isochronous, configuration,
  interface selection, descriptors, class/vendor controls, status/configuration queries,
  feature requests, pipe abort/reset, and bulk/interrupt requests. Transactions are
  cancellable and bounded to 32 MiB each, with per-slot and controller aggregate limits.

## Limits and validation status

This builds as a Release x64 driver, but remains an unvalidated replacement candidate.
The HCD maps the URB families accepted by the reconstructed WDM protocol into frozen
wire types 1–6. The unchanged UVC handler intentionally stalls or ignores operations it
does not implement (including bulk data and Microsoft OS feature descriptors), matching
the old user-mode behavior. No driver installation or Windows USB/UVC end-to-end runtime
validation has been run.
The frozen ABI carries a 40-byte operation record and payload rather than an arbitrary
raw URB, so unsupported URB types cannot be passed through unchanged.

The role-2 INF lives in the combined deployment package at
`../replacement-package/package/xusb_ucx_hcd.inf`; there is intentionally no
standalone development INF here. The combined package workflow owns catalog
generation and deployment metadata.

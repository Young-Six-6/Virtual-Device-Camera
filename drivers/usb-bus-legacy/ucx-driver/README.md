# Virtual Camera UCX host controller

Runtime measurement (2026-11-18): tree 1.0.0.11 and HCD 2.0.0.28 were bound. The controller, ROOT_HUB30 child, and SuperSpeed camera node reported OK. DirectShow captured 120 frames of 1280×720 YUY2 in about 3.97 seconds (about 30fps); the HCD pending-transaction count returned to zero after the camera app stopped. This validates one SuperSpeed isochronous camera on the software virtual bus, not physical USB 3 throughput or general USB-device compatibility. Earlier USB 2.0 MJPEG capture reached about 25fps.

Latest read-only PnP/SCM query (2026-11-18) reports tree 1.0.0.13 and HCD 2.0.0.28 in `OK` state, with no active camera application or stream. Source 2.0.0.29 adds multiple SuperSpeed slots and has passed Release build and static package checks, but has not been installed for multi-camera PnP validation.

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

This builds as a Release x64 driver and has passed the camera's tested control,
descriptor and isochronous capture paths at USB 2.0 and SuperSpeed. It remains a partial replacement candidate:
the HCD maps legacy operations into frozen wire types 1–6, while the UVC handler still
does not implement bulk data or the complete Microsoft OS feature-descriptor path.
The reconstructed WDM source maps `IOCTL_INTERNAL_USB_CYCLE_PORT` to wire reset type 2.
That IOCTL targets the USB hub PDO; UCX-based operation is expected to expose the port
state through root-hub callbacks instead. The new root hub models port-power off/on as
a disconnect/reconnect, which may provide the needed cycle semantics without emitting
wire reset type 2. The exact Windows hub-to-UCX callback sequence remains unverified.
The root hub advertises individual port power switching and tracks `PORT_POWER`
independently of virtual-device slot allocation. Clearing power hides the connection
and cancels outstanding transactions. The HCD sends the unchanged user-mode camera a
wire reset notification so it clears its current video-frame position; restoring power
reports a fresh connection. UCX device-reset callbacks also send the wire reset notice.
This models the root-hub request but does not prove Windows maps the legacy cycle-port
IOCTL to that sequence.
The frozen ABI carries a 40-byte operation record and payload rather than an arbitrary
raw URB, so unsupported URB types cannot be passed through unchanged.

The root-hub PDO enumerates after the bus stopped clearing the upper KMDF BusRelations result. This also preserves the PDO references owned by that result. Same-version unload/reload remains unverified.

## USB 3.0 SuperSpeed path

The 2.0.0.29 source adds an additive `IOCTL_PDO_ADD_SUPERSPEED`, reserves seven
independent virtual slots on root-hub ports 17–23, maps UCX's `UsbSuperSpeed` device speed,
reports per-port USB 3 connect/reset/power state, and validates isochronous endpoint companion limits.
The user-mode camera tries this FOSS-only add path first; the original `IOCTL_PDO_ADD` and
the frozen BEGIN/END wire structures are unchanged, and unsupported older/original drivers
fall back to the existing USB 2.0 path.

When SuperSpeed is selected, the camera emits a USB 3.0 device descriptor, BOS with the
SuperSpeed capability, 1024-byte isochronous packets, and a companion descriptor capped at
8,192 bytes per microframe. It exposes the same YUY2/MJPEG formats and advertises 30fps
for 720p YUY2; raw 1280×720 YUY2 at 30fps requires about 55.3 MB/s. Microsoft documents
that SuperSpeed isochronous packet limits come from the companion descriptor's
`wBytesPerInterval`, while high-speed is capped at 3×1024 bytes per microframe:
[Microsoft isochronous transfer guidance](https://learn.microsoft.com/windows-hardware/drivers/usbcon/transfer-data-to-isochronous-endpoints).

The prior 2.0.0.28 Release build passed Windows PnP and DirectShow capture: 120 frames
of 720p YUY2 were delivered in about 3.97 seconds. The 2.0.0.29 multi-slot changes pass
Release build and static deployment/package checks; multi-device Windows PnP capture is
still pending. USB
2.0 YUY2 720p remains limited to about 13.3fps by its 3×1,024-byte high-speed
isochronous interval; the MJPEG path previously reached about 25fps on this machine.
The SuperSpeed measurement is for this software virtual bus and is not a physical USB 3
controller throughput benchmark.

## Removal and unload lifecycle

The role-3 bus now retains a still-present child PDO across a function-driver removal
for update or disable/enable. It only deletes the PDO after the device disappears from
the bus. Its retained PDO uses temporary PnP transaction suspension, so START can resume
it without reinitializing a permanently released remove lock. Role-3 `1.0.0.9`
(`oem67.inf`) and HCD `2.0.0.10` (`oem65.inf`) were installed in the same boot session;
both started successfully. Same-version unload/reload still needs validation.

The HCD caller-context callback captures handles and METHOD_NEITHER buffers only for
the four private user-mode IOCTLs. Other requests return to KMDF dispatch through
WdfDeviceEnqueueRequest, allowing UCX and endpoint queues to receive kernel requests.
PrepareHardware restarts the user queue explicitly purged by ReleaseHardware.

The HCD registers `EvtDevicePrepareHardware` and `EvtDeviceReleaseHardware`. Prepare
clears the stopping state on each PnP restart. Release marks the controller stopping,
purges its user I/O queue, cancels all per-slot URB/reset transactions, waits for the
transaction count to drain, flushes queued completion DPCs, and waits for removal
notification work items to finish before returning. Device cleanup releases any
leftover slot-owned event and semaphore references and clears controller, root-hub,
queue, and address state. The
shared slot lock is parented to the WDF driver object so it remains valid until final
cleanup and can be reused if PnP recreates the device. Normal application handles must
still be closed before uninstall; an open client handle correctly keeps its device stack
alive. These paths compile in Release, but unload/re-enumeration still require runtime
verification. The UCX role-2 HCD previously reported `STATUS_DRIVER_FAILED_PRIOR_UNLOAD`
when role-3 was updated and its child was recreated. Each debug package now has a
version-specific service and image path, so the next package can load without replacing
the failed in-memory instance. The old image may remain until a later maintenance reboot;
same-version device removal/rebind remains unsafe until the underlying teardown issue is
diagnosed.

`ReleaseHardware` also resets all USB 2.0/3.0 port-power flags and clears the cached UCX
root-hub pipe/device handles after queued transactions and removal work items drain. This
prevents a later PnP start or resource rebalance from reusing stale handles from the prior
root-hub instance. This is lifecycle hygiene; it has not been shown to fix a root-hub
startup failure when the HCD's control callback is never invoked.

Cancel registration, transaction selection and `WdfRequestUnmarkCancelable` are serialized
by the slot lock. If Unmark returns `STATUS_CANCELLED`, only the cancellation callback
queues completion; the END/removal path releases its own transaction reference. The
removal-work counter and its drain event also change under the same lock. These rules
prevent early completion/freeing while the cancellation callback still uses the transaction,
and prevent a stale zero-count event from overtaking newly queued removal work.

Each allocated slot has a monotonically increasing generation. File, USB-device, endpoint,
queue and transaction contexts retain it, and operations check it under the slot lock.
After PnP stop/start, an old application handle or late UCX cleanup cannot remove a new
device or decrement its reference count merely because it occupies the same port number.
Private caller-context requests use `WdfDeviceEnqueueRequest` after handle/buffer capture;
they have not yet come from a queue, so `WdfRequestForwardToIoQueue` is not valid there.

Each UCX endpoint queue is now started by `EvtEndpointStart`. Purge and abort cancel the
endpoint's outstanding transactions and purge its WDF queue; their UCX completion calls
run only after KMDF reports that requests delivered to the queue have completed or been
canceled. Abort restarts the WDF queue before completing the UCX abort. This ordering is
necessary for interface changes and disconnect cleanup; it has been build-checked, but
still needs on-device exercise through stream start/stop and unplug/replug.

The role-2 INF lives in the combined deployment package at
`../replacement-package/package/xusb_ucx_hcd.inf`; there is intentionally no
standalone development INF here. The combined package workflow owns catalog
generation and deployment metadata.

## Historical root-hub startup failure (2026-11-16; superseded by HCD 2.0.0.22)

The following note records the earlier Code 43 investigation and is not the current
runtime state.

The role-3 internal controller PDO reports a project-specific software bus GUID
with `PNPBus`, matching the HCD's `UcxControllerParentBusTypeCustom` setting. The
previous PDO implementation reported `GUID_BUS_TYPE_USB` for every child,
including this host-controller PDO. Microsoft recommends a unique bus GUID for a
vendor software bus. The updated PDO was installed on the test machine. UCX created
the controller and root-hub objects and enumerated its root-hub PDO. At that stage the
info callbacks succeeded, but USBHUB3 failed with Code 43 before the HCD control callback
was observed. HCD 2.0.0.22 later passed root-hub startup and camera capture.

The initialization query supports the prior 64-, 104- and 132-byte responses and a 168-byte lifecycle response. The latest tail records hardware release start/completion counts, D0 entry/exit counts, power/stopping state, pending transactions/removal work and the user queue state. The query script displays unavailable lifecycle fields as blank for older drivers. Snapshots are approximate, and the normal user queue is purged during removal, so this interface cannot be used to poll a stack that is already stalled inside teardown. Earlier 2.0.0.17/132-byte and 2.0.0.22 observations are historical; current runtime evidence is recorded at the top of this document.

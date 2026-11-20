# Virtual Camera UCX host controller

## 2026-11-21 current runtime validation

The test machine currently runs role-3 USB tree `1.0.0.22` and UCX HCD `2.0.0.41`
under the local test-signing certificate. Both were installed and bound without a
Windows reboot. The controller, tree and USB 3 root hub report `OK`; the root hub
reports seven ports. DirectShow captured 120 frames of 1280×720 YUY2 at 30fps in
3.95 seconds. In the same app process, the control pipe changed 1280×720 to 640×480
and back; both sizes reported `RUNNING` and each captured 30 frames successfully.
After orderly app shutdown, the camera PDO disappeared and HCD pending transactions
and removal work items were zero. The system boot time did not change and no new
BugCheck 1001 appeared during this validation.

HCD `.40` was a failed intermediate candidate: it treated the isochronous IN packet
descriptor's `Length` as an input capacity, which stalled camera capture. HCD `.41`
uses packet-buffer offsets and the endpoint service-interval limit to derive the
writable capacity, then bounds the returned length. See [Microsoft's isochronous
transfer guidance](https://learn.microsoft.com/windows-hardware/drivers/usbcon/transfer-data-to-isochronous-endpoints).

## 2026-11-20 runtime probes and crash diagnosis (historical)

At that time, the test machine ran role-3 USB tree `1.0.0.20` and UCX HCD `2.0.0.39`
under the local test-signing certificate. The controller and USB 3 root hub report
`OK`; UCX initialization succeeded, the root hub reports seven ports, and pending
transaction/removal-work-item counts return to zero after each fixture is removed.
HCD `.39` was installed and its controller was PnP-restarted without rebooting
Windows.

On `.39`, the unchanged Release camera program enumerated `Pipe HD Camera 1` and
DirectShow captured 120 frames at 1280×720 YUY2 at about 30fps. The control pipe also
changed resolution 1280×720 → 640×480 → 1280×720 in one process; each status reported
the new dimensions and `RUNNING`, and capture succeeded at all three steps. Low- and
full-speed HID fixtures enumerated as `HIDClass`, sustained interrupt-IN traffic for
five seconds, and removed in 754ms and 768ms. A WinUSB fixture completed 512-byte bulk
IN and 16-byte bulk OUT transactions through BEGIN/END and removed in 72ms. A malformed
bulk END was rejected with `ERROR_INVALID_PARAMETER`; the request, fixture and pending
counters all drained cleanly.

The Microsoft OS feature-URB probe first exposed a bug in its test-only KMDF client,
not the production HCD. The dump recorded `0x144/3`; `!analyze -v` attributes the bad
URB submission to `UcxMsFeatureTestClient.sys+0x1799`. That client had registered
`USBD_CLIENT_CONTRACT_VERSION_602` but used a stack-allocated URB. It now allocates URBs
with `WdfUsbTargetDeviceCreateUrb`. The rebuilt, test-signed client retrieved
descriptor 4 (40 bytes) and descriptor 5 (10 bytes) with successful NTSTATUS and
USBD_STATUS, then removed cleanly without another bugcheck. Windows translated each
request into a vendor-control transfer before it reached the HCD; the direct
`URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR` representation was not observed at the HCD
boundary. The probe package was removed from DriverStore after the run.

Historical `.36` issue: a separate earlier dump mapped `0x3B` to deferred completion
dereferencing a missing `VHCD_TRANSFER_CONTEXT`. HCD `.37` added request-context
allocation and `.38` corrected bulk-OUT completion length. HCD `.39` adds USB 2.0
`C_PORT_ENABLE` reporting, validates bulk direction and bounds Microsoft OS feature
descriptor buffers. Release `.40` added an ISO response-length check but incorrectly
treated the IN packet descriptor's `Length` as an input capacity; it stalled video
capture. Release `.41` corrected the capacity calculation using packet offsets and
the endpoint interval limit, and is the runtime version documented above.

With the camera app stopped and its virtual camera PDO absent, PnPUtil restarted only the virtual HCD instance PCI\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88\1&2D12BED1&3&1 (no /reboot). PnPUtil returned 0; the HCD, USB tree and USB 3 root hub returned to OK, and UCX initialization statuses were all zero. The Release camera app then started again and DirectShow captured 60 frames at 1280×720 YUY2/30fps in 2.004 seconds. After the app exited, its camera PDO disappeared and pending transactions/removal work items were zero. Lifecycle counters queried after restart belong to the newly initialized instance and do not independently prove the old instance's ReleaseHardware callback count.

With the Release camera app left running, only its virtual root hub (USB\ROOT_HUB30\2&87D351D&0&0) was restarted through PnP. The root hub and camera PDO returned to OK without restarting Windows or the app. DirectShow reopened the camera and captured 60 frames at 1280×720 YUY2/30fps in 1.992 seconds. After the app exited, the camera PDO disappeared and the HCD reported zero pending transactions and removal work items; the tree and HCD remained OK. This covers one root-hub PnP restart and stream reopen, not sleep/resume or every power/URB path.

Historical runtime validation (2026-11-19): tree 1.0.0.18 and HCD 2.0.0.34 were bound in the same Windows boot session. The controller and ROOT_HUB30 child report OK. DirectShow captured 120 frames of 1280×720 YUY2 in 3.93 seconds (about 30.5fps); two concurrent streams each delivered 120 frames in 3.96 seconds (about 30.3fps per stream). After the app stopped, both camera nodes disappeared and the HCD reported zero pending transactions and removal work items. Low- and full-speed HID fixtures also enumerated with status OK; their periodic interrupt requests were paced, and both fixtures removed cleanly. No system restart was needed. This validates the tested software virtual-bus paths, not physical USB 3 throughput or general USB-device compatibility. Earlier hot-resolution and USB 2.0 MJPEG measurements are historical results.

Read-only PnP/SCM snapshot from 2026-11-19: tree 1.0.0.18 and HCD 2.0.0.34 were in `OK` state, with both services running and no active camera node or stream. HCD 2.0.0.34 retains seven SuperSpeed slots, validates endpoint descriptors, supports the FOSS-only USB 2.0 low/full-speed add path, and paces interrupt requests from `bInterval`. Single- and dual-camera SuperSpeed capture and low/full-speed HID PnP, interrupt, and removal paths were runtime-verified on that version. These results do not cover all URBs or USB classes; current HCD 2.0.0.41 runtime status is listed above.

After a targeted `pnputil /restart-device "ROOT\SYSTEM\0003"` (without `/reboot`), the same tree/HCD versions and the child USB 3 root hub returned to `OK` without restarting Windows. The Release camera app then re-enumerated and DirectShow captured 60 frames of 1280×720 YUY2 at 30fps in 1.98 seconds. After stopping the app, its virtual camera PDO disappeared and HCD pending transactions/removal work items returned to zero. This verifies one tree-device PnP restart and camera recovery; system sleep/resume and every power transition remain unverified.

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
- Preserves the original high-speed `IOCTL_PDO_ADD` and SuperSpeed extension. Adds
  `IOCTL_PDO_ADD_WITH_USB_SPEED` (`0x222034`) with an appended speed field for USB 2.0
  low/full-speed slots; its ABI layout is checked at build time.
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

### Scope of the remaining USB paths

The current virtual camera exposes a UVC control endpoint and an isochronous video-IN endpoint; its descriptors declare no bulk or interrupt endpoint and no Microsoft OS string at index 0xEE. The production user-mode camera responder still does not implement bulk/interrupt transfer type 2 or descriptor type 5, so it is not a general-purpose HID/serial/storage responder. The synthetic test responder exercises bulk/interrupt transactions and Microsoft OS 1.0 vendor-control descriptors through the frozen BEGIN/END ABI. The KMDF feature-URB test proves descriptor retrieval through Windows and the HCD vendor-control path; the direct `URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR` HCD mapping remains unobserved. Root-hub reset/power/link transitions and URB families outside the tested paths also remain only partially verified.

This builds as a Release x64 driver and has passed the virtual camera's tested control,
descriptor and isochronous capture paths at USB 2.0 and SuperSpeed. It is a replacement
candidate for that virtual-camera path, not a general-purpose USB host controller: the
HCD maps legacy operations into frozen wire types 1–6, and the production UVC handler
does not implement arbitrary bulk/interrupt device classes. A test-only KMDF client and
responder validate Microsoft OS feature descriptor retrieval through Windows and the
vendor-control path; the direct HCD `GET_MS_FEATURE_DESCRIPTOR` representation remains
unobserved.
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

## USB 2.0 low/full-speed extension

The driver accepts low- and full-speed device additions on the existing
USB 2.0 root-hub ports. `UsbSpeed` values are `0` for low speed and `1` for full
speed. The 1,584-byte frozen create structure stays intact; the new request appends a
32-bit speed value and is 1,592 bytes on the x64 target. The original add IOCTL keeps
its high-speed behavior and the SuperSpeed add IOCTL is unchanged.

The HCD reports the proper root-hub speed bits and checks endpoint limits by speed:
low speed permits control and interrupt endpoints only, while full speed permits
control, bulk, interrupt, and isochronous endpoints within USB packet/interval limits.
The existing camera handler still selects high speed or SuperSpeed; it does not yet
use this API. ABI assertions and the Release WDK build pass, and a temporary caller
verified that both speed values add and remove a slot and receive the removal event.
HCD 2.0.0.34 was subsequently tested with a small HID transaction responder: low- and
full-speed keyboard fixtures enumerated as `HIDClass`, received periodic interrupt-IN
requests, and removed without leaving a PnP node. The responder sends zero-filled
reports; it does not simulate useful keyboard input or qualify arbitrary HID devices.

## USB 3.0 SuperSpeed path

The 2.0.0.30 source adds an additive `IOCTL_PDO_ADD_SUPERSPEED`, reserves seven
independent virtual slots on root-hub ports 17–23, maps UCX's `UsbSuperSpeed` device speed,
reports per-port USB 3 connect/reset/power state, and validates SuperSpeed isochronous,
bulk and interrupt endpoint companion descriptors. Bulk/interrupt URBs still need matching
user-mode transport support for end-to-end device operation.
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

The 2.0.0.34 Release build passed Windows PnP and DirectShow capture: one camera
delivered 120 frames of 720p YUY2 in 3.93 seconds. A second test opened two
cameras at once and requested 120 frames per stream; both completed in 3.96
seconds with the DirectShow receive buffers sized for simultaneous startup. Both
camera nodes disappeared after the application stopped, and pending transactions/removal
work items returned to zero. USB
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

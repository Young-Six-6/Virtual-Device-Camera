# USB request fallback API

`vcam_create_ex` accepts an optional `USB_REQUEST_CALLBACK`. The callback gets
the first chance to handle supported device requests; it can accept a request,
delegate it to the built-in UVC handler, or stall it. Existing `vcam_create` and
`vcam_create_with_mjpeg` callers keep their current behavior.

## Scope

The built-in path still advertises its UVC descriptors: one control interface
and one isochronous video-IN endpoint. With `vcam_create_ex`, an application
can override the descriptor and request responses needed for a different
device model. The callback must return a coherent device/configuration/string
descriptor set and handle the requests for every endpoint it declares. The
descriptor VID/PID must match `uvc_vcam_t.vid` and `uvc_vcam_t.pid`, which are
also used to create the PnP identity.

The callback handles descriptor/configuration selection, vendor/class control,
control and bulk/interrupt transfers, reset, status, and feature requests.
Bulk/interrupt transfers are routable when the application's configuration
descriptor declares matching endpoints. Isochronous transfers remain with the
built-in UVC handler; the callback cannot currently implement an arbitrary
isochronous device. Older/original drivers may report pipe type 0 for
bulk/interrupt, so responders can use the endpoint address and their own
descriptor model in that case.
Microsoft OS feature descriptor requests are limited to page index 0 and a
4 KiB transfer buffer, matching the Windows URB contract supported by this
driver.

The built-in UVC responder accepts only device-recipient U1/U2 enable and
disable requests advertised by its SuperSpeed BOS descriptor. It stalls
unsupported feature selectors, recipients, and nonzero device indexes. A
custom callback can implement the feature behavior declared by its own device
descriptors.

The callback runs before the UVC handler. Return `VCAM_USB_REQUEST_HANDLED`
(`0`) to complete the request, `VCAM_USB_REQUEST_PASSTHROUGH` (`1`) to let the
built-in UVC handler try it, or any other nonzero value to stall it. Do not
modify request buffers when returning passthrough. A null `frame_callback` is
allowed when `vcam_create_ex` has a USB callback; the session uses a black frame
if a request is passed through to UVC video handling.

## Request and response

The callback receives a `vcam_usb_request_t`. All pointers are valid only until
the callback returns.

| Field | Meaning |
| --- | --- |
| `kind` | One of the `VCAM_USB_REQUEST_*` values. |
| `device_speed` | Active virtual path speed: `VCAM_USB_SPEED_HIGH` or `VCAM_USB_SPEED_SUPER`. Use this when returning speed-specific device/configuration descriptors. |
| `is_read` | Nonzero for device-to-host (IN); zero for host-to-device (OUT). |
| `input_data`, `buffer_capacity` | Writable IN response buffer and its capacity. |
| `output_data`, `transfer_length` | Read-only OUT payload and its byte length. For IN, `transfer_length` is the requested response length. |
| `endpoint_address`, `pipe_type` | Endpoint metadata for bulk/interrupt requests. Older drivers may report pipe type 0 as unknown. |
| `setup_packet`, `timeout_ms` | Setup packet and timeout for a control transfer. |
| `request_type`, `request_subtype` | Vendor/class selector and recipient scope for vendor/class requests: type 1 is class, 2 is vendor; scope 1=device, 2=interface, 3=endpoint, 4=other. |
| `descriptor_recipient` | Legacy scope: 1=device, 3=interface, 4=endpoint. Value 2 denotes configuration/interface selection: `descriptor_type` 1 selects configuration (`descriptor_index` is `0xffffffff`, `value` is the configuration value), 2 selects interface (`descriptor_index` is the interface number, `value` is the alternate setting). This is not the USB setup packet's `bmRequestType` recipient encoding. |
| `recipient`, `interface_number`, `page_index`, `feature_descriptor_index` | Decoded Microsoft OS feature descriptor fields. |
| `status_type`, `feature_type`, `feature_subtype`, `reset_type` | Operation selectors for status, feature, and reset requests. |

For control transfers, the direction bit in `bmRequestType` must agree with the
URB direction. A control OUT payload must match setup `wLength`; an oversized
control IN buffer is limited to `wLength`. For IN, write no more than
`buffer_capacity` bytes to `input_data`, set
`response_length`, and return 0. The transport stalls the request if
`response_length` exceeds the supplied capacity. For OUT, consume the read-only
`output_data` and return 0 to accept the complete payload. END carries no reply
payload for OUT; the WDM and UCX paths complete the URB using the original
request length. Return `VCAM_USB_REQUEST_PASSTHROUGH` for an unsupported request
that should still be offered to the built-in UVC handler. Return any other
nonzero value to complete it as a stall.

```cpp
int handle_usb_request(vcam_usb_request_t* request, void* context) {
    auto* state = static_cast<MyDeviceState*>(context);
    if (request->kind == VCAM_USB_REQUEST_VENDOR_CONTROL &&
        request->is_read && request->request == state->status_request) {
        const unsigned int count = (std::min)(
            request->buffer_capacity,
            static_cast<unsigned int>(state->status.size()));
        if (count != 0)
            std::memcpy(request->input_data, state->status.data(), count);
        request->response_length = count;
        return 0;
    }
    return VCAM_USB_REQUEST_PASSTHROUGH;
}

void* camera = vcam_create_ex(&config, nullptr, handle_usb_request, &state);
```

The example requires `<algorithm>` and `<cstring>`. `state` and its referenced
storage must remain alive until after `vcam_destroy(camera)`.

## Threading and lifecycle

The callback runs synchronously on the session's only USB worker. It must return
promptly; blocking it also blocks USB transaction processing. Do not call
camera-session APIs from a frame, MJPEG, or USB callback. Calls to
`vcam_destroy`, `vcam_set_resolution`, and `vcam_get_resolution` from a camera
worker are rejected to prevent self-join and stop/reconfigure deadlocks. Protect
shared application state if other threads modify it.

The callback contract builds in the current Release application, and protocol
tests cover MS feature field mapping, bulk/interrupt IN and OUT, control
`wLength`, response bounds, and callback-exception cleanup. The `.49/.25`
runtime check exercised the built-in UVC camera only; custom descriptors and
bulk/interrupt responders have not been validated with a runtime device. The
default camera does not register this callback.

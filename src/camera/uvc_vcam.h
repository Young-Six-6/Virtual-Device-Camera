#pragma once

// API contract adapted from Fanxiushu's 2016 implementation.
// Public camera configuration and frame callback contract.
struct frame_t {
    char* buffer;
    int length;      // Writable YUY2 buffer capacity in bytes.
    int width;       // Fixed UVC output dimensions.
    int height;
    int delay_msec;  // Suggested delay before requesting the next frame.
    void* param;     // Opaque caller-owned callback context.
};

typedef int (*FRAME_CALLBACK)(frame_t* frame);

// Optional UVC MJPEG encoder callback. The input is the current YUY2 frame;
// the callback writes one complete JPEG image into the caller-owned buffer.
struct mjpeg_frame_t {
    const char* yuy2;
    int yuy2_length;
    int width;
    int height;
    char* buffer;
    int capacity;
    int length;
    void* param;
};

typedef int (*MJPEG_FRAME_CALLBACK)(mjpeg_frame_t* frame);

enum vcam_usb_request_kind {
    VCAM_USB_REQUEST_CONTROL_TRANSFER = 1,
    VCAM_USB_REQUEST_BULK_OR_INTERRUPT_TRANSFER = 2,
    VCAM_USB_REQUEST_MS_FEATURE_DESCRIPTOR = 3,
    VCAM_USB_REQUEST_DESCRIPTOR = 4,
    VCAM_USB_REQUEST_VENDOR_CONTROL = 5,
    VCAM_USB_REQUEST_STATUS_QUERY = 6,
    VCAM_USB_REQUEST_FEATURE = 7,
    VCAM_USB_REQUEST_RESET = 8
};

enum vcam_usb_request_callback_result {
    VCAM_USB_REQUEST_HANDLED = 0,
    VCAM_USB_REQUEST_PASSTHROUGH = 1,
    VCAM_USB_REQUEST_STALL = -1
};

enum vcam_usb_speed {
    VCAM_USB_SPEED_LOW = 1,
    VCAM_USB_SPEED_FULL = 2,
    VCAM_USB_SPEED_HIGH = 3,
    VCAM_USB_SPEED_SUPER = 4
};

/*
 * Optional responder view for USB transactions. The callback runs before the
 * built-in UVC implementation and can override descriptors and request behavior.
 * It does not itself synthesize descriptors; an application must return a
 * coherent device/configuration descriptor set before declaring custom endpoints.
 * All buffers are valid only for the callback duration.
 * For device-to-host requests, write input_data and set response_length.
 * For host-to-device requests, read output_data. Return HANDLED to accept,
 * PASSTHROUGH (without changing request buffers) to let the built-in UVC handler
 * try, or STALL/another nonzero value to reject. The callback runs synchronously
 * on the camera worker and must return promptly. Do not call camera-session
 * APIs from a frame, MJPEG, or USB callback; such reentrant operations are
 * rejected to avoid joining the worker from itself or deadlocking a stop.
 */
struct vcam_usb_request_t {
    int kind;
    int device_speed;          // One of vcam_usb_speed; reflects the active virtual root-hub path.
    int is_read;               // Nonzero for device-to-host transfers.
    int endpoint_address;      // Zero for endpoint-zero/control requests.
    int pipe_type;             // Control=0, bulk=2, interrupt=3; legacy bulk/interrupt may be 0.
    int request;               // bRequest or vendor/class Request when present.
    int request_type;          // Class/vendor type for VCAM_USB_REQUEST_VENDOR_CONTROL.
    int request_subtype;       // Recipient scope for VCAM_USB_REQUEST_VENDOR_CONTROL.
    unsigned int index;        // Setup wIndex or wire index.
    unsigned int value;        // Setup wValue or wire value.
    unsigned int descriptor_type;
    unsigned int descriptor_recipient; // Legacy scope: device=1, interface=3, endpoint=4.
    unsigned int descriptor_index;
    unsigned int language_id;
    unsigned int recipient;
    unsigned int interface_number;
    unsigned int page_index;
    unsigned int feature_descriptor_index;
    int status_type;           // Wire status request selector for STATUS_QUERY.
    int feature_type;          // Wire feature request selector for FEATURE.
    int feature_subtype;       // Wire feature recipient/scope for FEATURE.
    int reset_type;            // Wire reset operation for RESET.
    unsigned char setup_packet[8];
    unsigned int timeout_ms;
    const unsigned char* output_data; // Host-to-device payload, if any.
    unsigned char* input_data;        // Device-to-host response buffer, if any.
    unsigned int transfer_length;     // OUT payload length or requested IN length.
    unsigned int buffer_capacity;
    unsigned int response_length;     // Set by the callback for IN requests.
};

typedef int (*USB_REQUEST_CALLBACK)(vcam_usb_request_t* request, void* context);

struct uvc_vcam_t {
    int pid;
    int vid;
    const char* manu_fact;
    const char* product;
    FRAME_CALLBACK frame_callback;
    void* param;
    int width = 0;   // Zero selects the default resolution configuration.
    int height = 0;
    void (*request_stop)(void*) = nullptr;
    void (*resume)(void*) = nullptr;
    int fps = 30;  // Preferred default frame rate advertised by the UVC device.
    bool enable_yuy2 = true;
    bool enable_mjpeg = true;
};

void* vcam_create(uvc_vcam_t* uvc);
// Creates a camera that also advertises the optional UVC MJPEG format.
void* vcam_create_with_mjpeg(uvc_vcam_t* uvc, MJPEG_FRAME_CALLBACK callback);
// Registers a callback that may handle supported USB requests before the UVC handler.
void* vcam_create_ex(uvc_vcam_t* uvc, MJPEG_FRAME_CALLBACK mjpeg_callback,
                     USB_REQUEST_CALLBACK usb_request_callback, void* usb_request_context);
void vcam_destroy(void* handle);
int vcam_set_resolution(void* handle, int width, int height);
int vcam_get_resolution(void* handle, int* width, int* height);

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
};

void* vcam_create(uvc_vcam_t* uvc);
void vcam_destroy(void* handle);
int vcam_set_resolution(void* handle, int width, int height);
int vcam_get_resolution(void* handle, int* width, int* height);

#include "camera_session.h"
#include <fstream>
#include <cstdio>
#include <cstring>

namespace {
int empty_frame(frame_t* frame) {
    if (frame == nullptr || frame->length < 0 ||
        (frame->length != 0 && frame->buffer == nullptr)) return -1;
    if (frame->length != 0) std::memset(frame->buffer, 0, static_cast<size_t>(frame->length));
    return 0;
}

vcam::Resolution initial_resolution(const uvc_vcam_t& config) {
    if (config.width || config.height) return {config.width, config.height};
    std::ifstream input("default_res.txt");
    std::string value;
    if (input >> value) {
        if (value == "480") return {640,480};
        if (value == "720") return {1280,720};
        if (value != "1080") fprintf(stderr, "Unknown default_res.txt value '%s'; using 1080p.\n", value.c_str());
    }
    return {1920,1080};
}
}
namespace {
void* create_camera(uvc_vcam_t* config, MJPEG_FRAME_CALLBACK mjpeg_callback,
                    USB_REQUEST_CALLBACK usb_request_callback,
                    void* usb_request_context) {
    if (!config || (!config->frame_callback && !usb_request_callback)) return nullptr;
    try {
        uvc_vcam_t sessionConfig = *config;
        if (sessionConfig.frame_callback == nullptr) {
            /* Generic USB responders need a valid UVC fallback if they pass a
             * request through; provide a harmless black frame in that case. */
            sessionConfig.frame_callback = empty_frame;
            if (sessionConfig.fps < 1 || sessionConfig.fps > 60)
                sessionConfig.fps = 30;
            if (!sessionConfig.enable_yuy2 && (!sessionConfig.enable_mjpeg || !mjpeg_callback))
                sessionConfig.enable_yuy2 = true;
        }
        auto transport = vcam::make_virtual_transport();
        if (!transport) { fprintf(stderr, "not found virtual usb path\n"); return nullptr; }
        auto session = std::make_unique<vcam::CameraSession>(
            sessionConfig, initial_resolution(sessionConfig), std::move(transport), mjpeg_callback,
            usb_request_callback, usb_request_context);
        if (!session->start()) return nullptr;
        return session.release();
    }
    catch (const std::exception& error) { fprintf(stderr, "Camera create: %s\n", error.what()); return nullptr; }
}
}
void* vcam_create(uvc_vcam_t* config) {
    return create_camera(config, nullptr, nullptr, nullptr);
}
void* vcam_create_with_mjpeg(uvc_vcam_t* config, MJPEG_FRAME_CALLBACK callback) {
    return callback ? create_camera(config, callback, nullptr, nullptr) : nullptr;
}
void* vcam_create_ex(uvc_vcam_t* config, MJPEG_FRAME_CALLBACK mjpeg_callback,
                     USB_REQUEST_CALLBACK usb_request_callback,
                     void* usb_request_context) {
    return create_camera(config, mjpeg_callback, usb_request_callback,
                         usb_request_context);
}
void vcam_destroy(void* handle) {
    if (vcam::is_camera_worker_thread()) {
        fprintf(stderr, "vcam_destroy cannot run from a camera callback/worker.\n");
        return;
    }
    delete static_cast<vcam::CameraSession*>(handle);
}
int vcam_set_resolution(void* handle, int width, int height) {
    if (!handle || vcam::is_camera_worker_thread()) return -1;
    try {
        std::string error;
        return static_cast<vcam::CameraSession*>(handle)->resize({width,height}, error) ? 0 : -1;
    }
    catch (...) { return -1; }
}
int vcam_get_resolution(void* handle, int* width, int* height) {
    if (!handle || !width || !height || vcam::is_camera_worker_thread()) return -1;
    const auto size = static_cast<vcam::CameraSession*>(handle)->resolution();
    *width = size.width;
    *height = size.height;
    return 0;
}

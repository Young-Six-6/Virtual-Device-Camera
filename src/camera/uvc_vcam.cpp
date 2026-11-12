#include "camera_session.h"
#include <fstream>
#include <cstdio>

namespace {
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
void* vcam_create(uvc_vcam_t* config) {
    if (!config || !config->frame_callback) return nullptr;
    try {
        auto transport = vcam::make_virtual_transport();
        if (!transport) { fprintf(stderr, "not found virtual usb path\n"); return nullptr; }
        auto session = std::make_unique<vcam::CameraSession>(*config, initial_resolution(*config), std::move(transport));
        if (!session->start()) return nullptr;
        return session.release();
    }
    catch (const std::exception& error) { fprintf(stderr, "Camera create: %s\n", error.what()); return nullptr; }
}
void vcam_destroy(void* handle) { delete static_cast<vcam::CameraSession*>(handle); }
int vcam_set_resolution(void* handle, int width, int height) {
    if (!handle) return -1;
    try {
        std::string error;
        return static_cast<vcam::CameraSession*>(handle)->resize({width,height}, error) ? 0 : -1;
    }
    catch (...) { return -1; }
}
int vcam_get_resolution(void* handle, int* width, int* height) {
    if (!handle || !width || !height) return -1;
    const auto size = static_cast<vcam::CameraSession*>(handle)->resolution();
    *width = size.width;
    *height = size.height;
    return 0;
}

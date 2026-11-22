#pragma once
#include "resolution.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace vcam {
// The reconstructed USB root hub exposes seven downstream ports.
inline constexpr size_t max_virtual_cameras = 7;

struct CameraSettings {
    Resolution resolution{0, 0};
    int fps = 30;
    uint16_t vid = 0xbb10;
    uint16_t pid = 0xcc10;
    std::string manufacturer = "Fanxiushu";
    std::string name = "Pipe HD Camera 1";
    bool yuy2 = true;
    bool mjpeg = true;
};
}

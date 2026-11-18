#pragma once
#include "resolution.h"
#include <cstdint>
#include <string>

namespace vcam {
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

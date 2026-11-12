#pragma once
#include <cstddef>

namespace vcam {
struct Resolution { int width = 1920; int height = 1080; };
inline bool operator==(Resolution a, Resolution b) { return a.width == b.width && a.height == b.height; }
inline bool operator!=(Resolution a, Resolution b) { return !(a == b); }
inline bool valid_input(Resolution size) {
    return size.width > 0 && size.width <= 4096 && size.height > 0 && size.height <= 2160;
}
inline bool valid_output(Resolution size) { return valid_input(size) && size.width % 2 == 0; }
inline size_t rgb_bytes(Resolution size) { return (size_t)size.width * size.height * 3; }
inline size_t yuy2_bytes(Resolution size) { return (size_t)size.width * size.height * 2; }
}

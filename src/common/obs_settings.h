#pragma once
#include <cstdint>
#include <string>

namespace vcam {
struct ObsSettings {
    std::string host = "localhost";
    uint16_t port = 4458;
    std::string source = "test1";
    std::string password;
    int fps = 10;
    bool auto_size = true;
    bool test_color = false;
    uint8_t color[3] = {255, 0, 0};
};
}

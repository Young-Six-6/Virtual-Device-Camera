#pragma once
#include "../common/pipe_security.h"
#include "../common/resolution.h"
#include <string>

namespace vcam {
struct AppSettings {
    std::string rgb_pipe = "\\\\.\\pipe\\vcam_pipe";
    std::string control_pipe = "\\\\.\\pipe\\vcam_control";
    PipeAccess rgb_pipe_access = PipeAccess::owner;
    PipeAccess control_pipe_access = PipeAccess::everyone;
    int web_port = 8765;
    Resolution cameras[2]{{0, 0}, {0, 0}};
};

AppSettings load_app_settings(const std::string& config_path);
bool save_app_settings(const std::string& config_path, const AppSettings& settings, std::string& error);
bool valid_pipe_path(const std::string& path);
}

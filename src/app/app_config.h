#pragma once
#include "../common/camera_settings.h"
#include "../common/pipe_security.h"
#include <string>

namespace vcam {
struct AppSettings {
    std::string rgb_pipe = "\\\\.\\pipe\\vcam_pipe";
    std::string control_pipe = "\\\\.\\pipe\\vcam_control";
    PipeAccess rgb_pipe_access = PipeAccess::owner;
    PipeAccess control_pipe_access = PipeAccess::everyone;
    int web_port = 8765;
    bool web_allow_network = false;
    CameraSettings cameras[2]{};
    bool camera_name_configured[2]{};
    std::string custom_css;
    std::string custom_js;
    AppSettings() {
        cameras[1].vid = 0xbb11;
        cameras[1].pid = 0xcc11;
        cameras[1].name = "MP4 HD Camera 2";
    }
};

AppSettings load_app_settings(const std::string& config_path);
bool save_app_settings(const std::string& config_path, const AppSettings& settings, std::string& error);
bool valid_pipe_path(const std::string& path);
}

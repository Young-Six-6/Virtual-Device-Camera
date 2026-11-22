#pragma once
#include "../common/camera_settings.h"
#include "../common/obs_settings.h"
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
    std::string input_mode = "pipe";
    ObsSettings obs;
    std::string obs_password_encrypted;
    bool obs_password_needs_reentry = false;
    CameraSettings cameras[max_virtual_cameras]{};
    bool camera_enabled[max_virtual_cameras]{};
    bool camera_name_configured[max_virtual_cameras]{};
    std::string custom_css;
    std::string custom_js;
    AppSettings() {
        camera_enabled[0] = true;
        for (size_t i = 0; i < max_virtual_cameras; ++i) {
            cameras[i].vid = static_cast<uint16_t>(0xbb10 + i);
            cameras[i].pid = static_cast<uint16_t>(0xcc10 + i);
            cameras[i].name = "Pipe HD Camera " + std::to_string(i + 1);
        }
        cameras[1].name = "MP4 HD Camera 2";
    }
};

AppSettings load_app_settings(const std::string& config_path);
bool save_app_settings(const std::string& config_path, const AppSettings& settings, std::string& error);
bool valid_pipe_path(const std::string& path);
}

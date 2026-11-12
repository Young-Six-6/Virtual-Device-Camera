#pragma once
#include "app_config.h"
#include "web_server.h"
#include "control_server.h"
#include <mutex>

struct frame_source;

namespace vcam {
class RuntimeController {
    mutable std::mutex mutex_;
    AppSettings settings_;
    std::string config_path_;
    frame_source* source_;
    CameraSession* cameras_[2];
    ControlServer* control_;
public:
    RuntimeController(AppSettings settings, std::string config_path, frame_source* source,
                      void* camera1, void* camera2, ControlServer& control);
    std::string status_json(unsigned short web_port) const;
    std::optional<CameraFrame> frame(int camera) const;
    bool apply(const WebUpdate& update, std::string& error);
};
}

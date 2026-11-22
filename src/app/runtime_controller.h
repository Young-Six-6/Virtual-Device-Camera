#pragma once
#include "app_config.h"
#include "web_server.h"
#include "control_server.h"
#include "../common/camera_settings.h"
#include <array>
#include <functional>
#include <memory>
#include <mutex>

struct frame_source;

namespace vcam {
struct CameraDeleter {
    void operator()(void* camera) const noexcept;
};
using CameraHandle = std::unique_ptr<void, CameraDeleter>;
using CameraFactory = std::function<CameraHandle(const CameraSettings&, int, std::string&)>;

class RuntimeController {
    mutable std::mutex mutex_;
    AppSettings settings_;
    std::string config_path_;
    frame_source* source_;
    std::array<CameraHandle, max_virtual_cameras> cameras_;
    CameraFactory camera_factory_;
    ControlRouter* router_;
    ControlServer* control_;
    bool input_mode_available_ = true;
    CameraActions camera_actions(size_t index) const;
    bool add_camera(int template_camera, std::string& error);
    bool remove_camera(int camera, std::string& error);
public:
    RuntimeController(AppSettings settings, std::string config_path, frame_source* source,
                      std::array<CameraHandle, max_virtual_cameras> cameras,
                      CameraFactory camera_factory, ControlRouter& router, ControlServer& control,
                      bool input_mode_available = true);
    ~RuntimeController();
    std::string status_json(unsigned short web_port) const;
    bool web_network_access() const;
    bool set_web_network_access(bool enabled, std::string& error);
    std::pair<std::string, std::string> theme() const;
    std::optional<CameraFrame> frame(int camera) const;
    bool apply(const WebUpdate& update, std::string& error);
};
}

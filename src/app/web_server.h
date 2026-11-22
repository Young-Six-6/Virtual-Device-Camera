#pragma once
#include "../camera/camera_session.h"
#include "../common/obs_settings.h"
#include "../common/pipe_security.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <thread>

namespace vcam {
enum class WebCameraOperation { none, add, remove };

struct WebUpdate {
    int camera = 1;
    WebCameraOperation camera_operation = WebCameraOperation::none;
    CameraSettings settings;
    std::string rgb_pipe;
    std::string control_pipe;
    PipeAccess control_access = PipeAccess::everyone;
    std::string custom_css;
    std::string custom_js;
    std::string input_mode;
    ObsSettings obs;
    bool has_camera_settings = false;
    bool has_theme = false;
    bool has_input_settings = false;
    bool obs_password_changed = false;
    bool clear_obs_password = false;
};

class WebServer {
    std::atomic<uintptr_t> listener_{static_cast<uintptr_t>(-1)};
    std::atomic<bool> stopping_{false}, running_{false}, winsock_initialized_{false}, network_access_{false};
    std::mutex listener_mutex_;
    std::thread worker_;
    unsigned short port_ = 0;
    std::string token_;
    std::function<std::string()> status_;
    std::function<std::optional<CameraFrame>(int)> frame_;
    std::function<bool(const WebUpdate&, std::string&)> update_;
    std::function<std::pair<std::string, std::string>()> theme_;
    std::function<std::string()> logs_;
    std::function<bool()> window_hidden_;
    std::function<bool(bool)> set_window_hidden_;
    std::function<bool(bool, std::string&)> set_network_access_;
    bool rebind_network_access(bool enabled, std::string& error);
    void run() noexcept;
public:
    ~WebServer();
    bool start(unsigned short port, std::string token,
               std::function<std::string()> status,
               std::function<std::optional<CameraFrame>(int)> frame,
               std::function<bool(const WebUpdate&, std::string&)> update,
               std::function<std::pair<std::string, std::string>()> theme = {},
               std::function<std::string()> logs = {},
               std::function<bool()> window_hidden = {},
               std::function<bool(bool)> set_window_hidden = {},
               bool network_access = false,
               std::function<bool(bool, std::string&)> set_network_access = {});
    void stop() noexcept;
    bool running() const { return running_.load(); }
    unsigned short port() const { return port_; }
};

std::string create_web_token();
}

#pragma once
#include "../camera/camera_session.h"
#include "../common/pipe_security.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace vcam {
struct WebUpdate {
    int camera = 1;
    Resolution resolution;
    std::string rgb_pipe;
    std::string control_pipe;
    PipeAccess control_access = PipeAccess::everyone;
};

class WebServer {
    std::atomic<uintptr_t> listener_{static_cast<uintptr_t>(-1)};
    std::atomic<bool> stopping_{false}, running_{false}, winsock_initialized_{false};
    std::thread worker_;
    unsigned short port_ = 0;
    std::string token_;
    std::function<std::string()> status_;
    std::function<std::optional<CameraFrame>(int)> frame_;
    std::function<bool(const WebUpdate&, std::string&)> update_;
    void run() noexcept;
public:
    ~WebServer();
    bool start(unsigned short port, std::string token,
               std::function<std::string()> status,
               std::function<std::optional<CameraFrame>(int)> frame,
               std::function<bool(const WebUpdate&, std::string&)> update);
    void stop() noexcept;
    bool running() const { return running_.load(); }
    unsigned short port() const { return port_; }
};

std::string create_web_token();
}

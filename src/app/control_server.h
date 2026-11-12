#pragma once
#include "../common/win_handle.h"
#include "../common/resolution.h"
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace vcam {
struct CameraActions {
    std::function<bool(Resolution, std::string&)> resize;
    std::function<Resolution()> resolution;
    std::function<bool()> running;
};
class ControlRouter {
    std::vector<CameraActions> cameras_;
public:
    explicit ControlRouter(std::vector<CameraActions> cameras) : cameras_(std::move(cameras)) {}
    std::string execute(const std::string& command) const;
};
class ControlServer {
    unique_handle pipe_, stop_;
    std::thread worker_;
    std::function<std::string(const std::string&)> handler_;
    void run() noexcept;
public:
    ~ControlServer();
    bool start(const std::string& path, std::function<std::string(const std::string&)> handler);
    void stop() noexcept;
};
}

#include "control_server.h"
#include "../common/pipe_io.h"
#include "../common/pipe_security.h"
#include <sstream>

namespace vcam {
std::string ControlRouter::execute(const std::string& command) const {
    std::istringstream input(command);
    std::string verb, extra;
    int camera = 0;
    if (!(input >> verb >> camera) || camera < 1 || size_t(camera) > cameras_.size()) return "ERR camera-not-found";
    const auto& actions = cameras_[camera-1];
    if (!actions.resolution) return "ERR camera-not-found";
    if (verb == "RESOLUTION") {
        Resolution size;
        if (!(input >> size.width >> size.height) || (input >> extra) || !valid_output(size)) return "ERR invalid-resolution";
        std::string error;
        if (!actions.resize(size, error)) return "ERR " + error;
        const auto applied = actions.resolution();
        return "OK RESOLUTION " + std::to_string(camera) + " " + std::to_string(applied.width) +
            " " + std::to_string(applied.height) + " REOPEN_REQUIRED";
    }
    if (verb == "STATUS" && !(input >> extra)) {
        const auto size = actions.resolution();
        return "OK STATUS " + std::to_string(camera) + " " + std::to_string(size.width) +
            " " + std::to_string(size.height) + (actions.running() ? " RUNNING" : " STOPPED");
    }
    return "ERR invalid-command";
}
ControlServer::~ControlServer() { stop(); }
bool ControlServer::start(const std::string& path, std::function<std::string(const std::string&)> handler,
                          PipeAccess access) {
    if (worker_.joinable()) return false;
    handler_ = std::move(handler);
    stop_.reset(CreateEvent(NULL, TRUE, FALSE, NULL));
    local_pipe_security security(access);
    if (!security) { stop_.reset(); return false; }
    pipe_.reset(CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, security.get()));
    if (!stop_ || !pipe_) { pipe_.reset(); stop_.reset(); return false; }
    path_ = path;
    access_ = access;
    running_.store(true);
    try { worker_ = std::thread(&ControlServer::run, this); }
    catch (...) { running_.store(false); pipe_.reset(); stop_.reset(); return false; }
    return true;
}
bool ControlServer::reconfigure(const std::string& path, PipeAccess access) {
    const std::string previous_path = path_;
    const auto previous_handler = handler_;
    const PipeAccess previous_access = access_;
    stop();
    if (start(path, previous_handler, access)) return true;
    if (!previous_path.empty()) start(previous_path, previous_handler, previous_access);
    return false;
}
void ControlServer::stop() noexcept {
    if (stop_) SetEvent(stop_.get());
    if (worker_.joinable()) worker_.join();
    pipe_.reset();
    stop_.reset();
    running_.store(false);
}
void ControlServer::run() noexcept {
    try {
        while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
            if (!connect_pipe(pipe_.get(), stop_.get())) break;
            std::string command;
            while (read_line(pipe_.get(), stop_.get(), command)) {
                std::string response;
                try { response = handler_(command); }
                catch (...) { response = "ERR operation-failed"; }
                response += '\n';
                if (!write_exact(pipe_.get(), stop_.get(), response.data(), response.size())) break;
            }
            DisconnectNamedPipe(pipe_.get());
        }
    }
    catch (...) { SetEvent(stop_.get()); }
    running_.store(false);
}
}

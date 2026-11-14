#include "runtime_controller.h"
#include "../media/frame_source.h"
#include "../usb/virt_dev.h"
#include <sstream>

namespace vcam {
namespace {
std::string json_string(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else if (c >= 0x20) result.push_back(static_cast<char>(c));
    }
    result.push_back('"');
    return result;
}
const char* access_name(PipeAccess access) {
    switch (access) {
    case PipeAccess::owner: return "owner";
    case PipeAccess::local_users: return "users";
    case PipeAccess::everyone: return "everyone";
    }
    return "owner";
}
const char* driver_kind_name(VirtUsbDriverKind kind) {
    switch (kind) {
    case VirtUsbDriverKind::original: return "original";
    case VirtUsbDriverKind::foss_reconstructed: return "foss";
    case VirtUsbDriverKind::unknown: return "unknown";
    case VirtUsbDriverKind::unavailable: return "unavailable";
    }
    return "unknown";
}
}

RuntimeController::RuntimeController(AppSettings settings, std::string config_path, frame_source* source,
                                     void* camera1, void* camera2, ControlServer& control)
    : settings_(std::move(settings)), config_path_(std::move(config_path)), source_(source),
      cameras_{static_cast<CameraSession*>(camera1), static_cast<CameraSession*>(camera2)}, control_(&control) {}

std::string RuntimeController::status_json(unsigned short web_port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const PipeReaderStatus pipe = frame_source_pipe_status(source_);
    const uint64_t now = GetTickCount64();
    const VirtUsbDriverKind driver = virt_usb_detect_driver();
    std::ostringstream json;
    json << "{\"driverAvailable\":" << (driver != VirtUsbDriverKind::unavailable ? "true" : "false")
         << ",\"driverKind\":" << json_string(driver_kind_name(driver))
         << ",\"controlPipeAccess\":" << json_string(access_name(settings_.control_pipe_access))
         << ",\"webPort\":" << web_port
         << ",\"rgbPipe\":{\"name\":" << json_string(settings_.rgb_pipe)
         << ",\"active\":" << (!pipe.path.empty() ? "true" : "false")
         << ",\"listening\":" << (pipe.listening ? "true" : "false")
         << ",\"connected\":" << (pipe.connected ? "true" : "false")
         << ",\"frames\":" << pipe.frames
         << ",\"frameAgeMs\":" << (pipe.last_frame_ms && now >= pipe.last_frame_ms ? now - pipe.last_frame_ms : 0)
         << ",\"width\":" << pipe.frame_size.width << ",\"height\":" << pipe.frame_size.height << "}"
         << ",\"controlPipe\":{\"name\":" << json_string(settings_.control_pipe)
         << ",\"running\":" << (control_->running() ? "true" : "false") << "}"
         << ",\"cameras\":[";
    for (int index = 0; index < 2; ++index) {
        if (index) json << ',';
        if (cameras_[index]) {
            const Resolution size = cameras_[index]->resolution();
            json << "{\"index\":" << index + 1 << ",\"present\":true,\"running\":"
                 << (cameras_[index]->running() ? "true" : "false") << ",\"width\":" << size.width
                 << ",\"height\":" << size.height << '}';
        } else json << "{\"index\":" << index + 1 << ",\"present\":false,\"running\":false,\"width\":0,\"height\":0}";
    }
    json << "]}";
    return json.str();
}

std::optional<CameraFrame> RuntimeController::frame(int camera) const {
    if (camera < 1 || camera > 2 || !cameras_[camera - 1]) return std::nullopt;
    return cameras_[camera - 1]->snapshot();
}

bool RuntimeController::apply(const WebUpdate& update, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (update.camera < 1 || update.camera > 2 || !cameras_[update.camera - 1]) {
        error = "camera-not-found";
        return false;
    }
    AppSettings previous = settings_;
    AppSettings replacement = settings_;
    replacement.rgb_pipe = update.rgb_pipe;
    replacement.control_pipe = update.control_pipe;
    replacement.control_pipe_access = update.control_access;
    replacement.cameras[update.camera - 1] = update.resolution;

    CameraSession& camera = *cameras_[update.camera - 1];
    const Resolution previous_resolution = camera.resolution();
    bool camera_changed = previous_resolution != update.resolution;
    if (camera_changed && !camera.resize(update.resolution, error)) return false;

    const bool control_changed = previous.control_pipe != replacement.control_pipe ||
        previous.control_pipe_access != replacement.control_pipe_access;
    if (control_changed && !control_->reconfigure(replacement.control_pipe, replacement.control_pipe_access)) {
        if (camera_changed) { std::string ignored; camera.resize(previous_resolution, ignored); }
        error = "control-pipe-reconfigure-failed";
        return false;
    }

    const PipeReaderStatus pipe_status = frame_source_pipe_status(source_);
    const bool rgb_changed = !pipe_status.path.empty() &&
        previous.rgb_pipe != replacement.rgb_pipe;
    if (rgb_changed && !frame_source_reconfigure_pipe(source_, replacement.rgb_pipe.c_str(), replacement.rgb_pipe_access)) {
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.resize(previous_resolution, ignored); }
        error = "rgb-pipe-reconfigure-failed";
        return false;
    }

    if (!save_app_settings(config_path_, replacement, error)) {
        if (rgb_changed) frame_source_reconfigure_pipe(source_, previous.rgb_pipe.c_str(), previous.rgb_pipe_access);
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.resize(previous_resolution, ignored); }
        return false;
    }
    settings_ = std::move(replacement);
    return true;
}
}

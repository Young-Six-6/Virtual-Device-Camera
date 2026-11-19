#include "runtime_controller.h"
#include "../media/frame_source.h"
#include "../usb/virt_dev.h"
#include <sstream>

namespace vcam {
namespace {
std::string json_string(const std::string& value) {
    std::string result = "\"";
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 0x20) {
            result += "\\u00";
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 0x0f]);
        }
        else result.push_back(static_cast<char>(c));
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
         << ",\"webNetworkAccess\":" << (settings_.web_allow_network ? "true" : "false")
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
            const CameraSessionStatus camera = cameras_[index]->status_snapshot();
            json << "{\"index\":" << index + 1 << ",\"present\":true,\"running\":"
                 << (camera.running ? "true" : "false") << ",\"width\":" << camera.size.width
                 << ",\"height\":" << camera.size.height
                 << ",\"usbMode\":" << json_string(camera.usb_mode)
                 << ",\"actualFps\":" << camera.actual_fps;
            const CameraSettings& settings = settings_.cameras[index];
            json << ",\"fps\":" << settings.fps
                 << ",\"vid\":" << json_string([&] { std::ostringstream value; value << std::hex << settings.vid; return value.str(); }())
                 << ",\"pid\":" << json_string([&] { std::ostringstream value; value << std::hex << settings.pid; return value.str(); }())
                 << ",\"manufacturer\":" << json_string(settings.manufacturer)
                 << ",\"name\":" << json_string(settings.name)
                 << ",\"yuy2\":" << (settings.yuy2 ? "true" : "false")
                 << ",\"mjpeg\":" << (settings.mjpeg ? "true" : "false") << '}';
        } else json << "{\"index\":" << index + 1 << ",\"present\":false,\"running\":false,\"width\":0,\"height\":0,\"usbMode\":\"disconnected\",\"actualFps\":0}";
    }
    json << "]}";
    return json.str();
}

bool RuntimeController::web_network_access() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_.web_allow_network;
}

bool RuntimeController::set_web_network_access(bool enabled, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (settings_.web_allow_network == enabled) return true;
    AppSettings replacement = settings_;
    replacement.web_allow_network = enabled;
    if (!save_app_settings(config_path_, replacement, error)) return false;
    settings_ = std::move(replacement);
    return true;
}

std::pair<std::string, std::string> RuntimeController::theme() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {settings_.custom_css, settings_.custom_js};
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
    replacement.cameras[update.camera - 1] = update.has_camera_settings
        ? update.settings : settings_.cameras[update.camera - 1];
    replacement.cameras[update.camera - 1].resolution = update.settings.resolution;
    if (update.has_camera_settings &&
        replacement.cameras[update.camera - 1].name != settings_.cameras[update.camera - 1].name)
        replacement.camera_name_configured[update.camera - 1] = true;
    if (update.has_theme) {
        replacement.custom_css = update.custom_css;
        replacement.custom_js = update.custom_js;
    }

    CameraSession& camera = *cameras_[update.camera - 1];
    const CameraSettings previous_camera_settings = settings_.cameras[update.camera - 1];
    const CameraSettings next_camera_settings = replacement.cameras[update.camera - 1];
    const bool camera_changed = previous_camera_settings.resolution != next_camera_settings.resolution ||
        previous_camera_settings.fps != next_camera_settings.fps || previous_camera_settings.vid != next_camera_settings.vid ||
        previous_camera_settings.pid != next_camera_settings.pid ||
        previous_camera_settings.manufacturer != next_camera_settings.manufacturer ||
        previous_camera_settings.name != next_camera_settings.name || previous_camera_settings.yuy2 != next_camera_settings.yuy2 ||
        previous_camera_settings.mjpeg != next_camera_settings.mjpeg;
    if (camera_changed && !camera.reconfigure(next_camera_settings, error)) return false;

    const bool control_changed = previous.control_pipe != replacement.control_pipe ||
        previous.control_pipe_access != replacement.control_pipe_access;
    if (control_changed && !control_->reconfigure(replacement.control_pipe, replacement.control_pipe_access)) {
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        error = "control-pipe-reconfigure-failed";
        return false;
    }

    const PipeReaderStatus pipe_status = frame_source_pipe_status(source_);
    const bool rgb_changed = !pipe_status.path.empty() &&
        previous.rgb_pipe != replacement.rgb_pipe;
    if (rgb_changed && !frame_source_reconfigure_pipe(source_, replacement.rgb_pipe.c_str(), replacement.rgb_pipe_access)) {
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        error = "rgb-pipe-reconfigure-failed";
        return false;
    }

    if (!save_app_settings(config_path_, replacement, error)) {
        if (rgb_changed) frame_source_reconfigure_pipe(source_, previous.rgb_pipe.c_str(), previous.rgb_pipe_access);
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        return false;
    }
    settings_ = std::move(replacement);
    return true;
}
}

#include "runtime_controller.h"
#include "../media/frame_source.h"
#include "../usb/virt_dev.h"
#include "../camera/uvc_vcam.h"
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
bool obs_settings_equal(const ObsSettings& left, const ObsSettings& right) {
    return left.host == right.host && left.port == right.port && left.source == right.source &&
        left.password == right.password && left.fps == right.fps && left.auto_size == right.auto_size &&
        left.test_color == right.test_color && left.color[0] == right.color[0] &&
        left.color[1] == right.color[1] && left.color[2] == right.color[2];
}
std::string obs_color(const ObsSettings& settings) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string value(6, '0');
    for (size_t i = 0; i < 3; ++i) {
        value[i * 2] = digits[settings.color[i] >> 4];
        value[i * 2 + 1] = digits[settings.color[i] & 15];
    }
    return value;
}
}

void CameraDeleter::operator()(void* camera) const noexcept {
    if (camera) vcam_destroy(camera);
}

RuntimeController::RuntimeController(AppSettings settings, std::string config_path, frame_source* source,
                                     std::array<CameraHandle, max_virtual_cameras> cameras,
                                     CameraFactory camera_factory, ControlRouter& router,
                                     ControlServer& control, bool input_mode_available)
    : settings_(std::move(settings)), config_path_(std::move(config_path)), source_(source),
      cameras_(std::move(cameras)), camera_factory_(std::move(camera_factory)), router_(&router),
      control_(&control), input_mode_available_(input_mode_available) {
    for (size_t index = 0; index < max_virtual_cameras; ++index)
        router_->set_camera(index, camera_actions(index));
}

RuntimeController::~RuntimeController() {
    control_->stop();
    for (size_t index = 0; index < max_virtual_cameras; ++index)
        router_->set_camera(index, {});
    for (auto& camera : cameras_) camera.reset();
}

CameraActions RuntimeController::camera_actions(size_t index) const {
    if (index >= max_virtual_cameras || !cameras_[index]) return {};
    auto* session = static_cast<CameraSession*>(cameras_[index].get());
    return {
        [session](Resolution value, std::string& error) { return session->resize(value, error); },
        [session] { return session->resolution(); },
        [session] { return session->running(); }
    };
}

std::string RuntimeController::status_json(unsigned short web_port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const PipeReaderStatus pipe = frame_source_pipe_status(source_);
    const FrameSourceStatus input = frame_source_input_status(source_);
    const uint64_t now = GetTickCount64();
    std::array<CameraSessionStatus, max_virtual_cameras> cameraStatus{};
    VirtUsbDriverKind driver = VirtUsbDriverKind::unavailable;
    for (size_t index = 0; index < max_virtual_cameras; ++index) {
        if (!cameras_[index]) continue;
        cameraStatus[index] = static_cast<CameraSession*>(cameras_[index].get())->status_snapshot();
        if (driver == VirtUsbDriverKind::unavailable)
            driver = cameraStatus[index].driver_kind;
    }
    if (driver == VirtUsbDriverKind::unavailable) driver = virt_usb_detect_driver();
    std::ostringstream json;
    json << "{\"driverAvailable\":" << (driver != VirtUsbDriverKind::unavailable ? "true" : "false")
         << ",\"driverKind\":" << json_string(driver_kind_name(driver))
         << ",\"controlPipeAccess\":" << json_string(access_name(settings_.control_pipe_access))
         << ",\"webNetworkAccess\":" << (settings_.web_allow_network ? "true" : "false")
         << ",\"webPort\":" << web_port
         << ",\"inputMode\":" << json_string(settings_.input_mode)
         << ",\"inputActiveMode\":" << json_string(input.mode)
         << ",\"inputModeAvailable\":" << (input_mode_available_ ? "true" : "false")
         << ",\"inputSource\":{\"state\":" << json_string(input.state)
         << ",\"error\":" << json_string(input.error)
         << ",\"connected\":" << (input.connected ? "true" : "false")
         << ",\"listening\":" << (input.listening ? "true" : "false")
         << ",\"frames\":" << input.frames
         << ",\"lastFrameMs\":" << (input.last_frame_ms && now >= input.last_frame_ms ? now - input.last_frame_ms : 0)
         << ",\"width\":" << input.frame_size.width << ",\"height\":" << input.frame_size.height << "}"
         << ",\"obs\":{\"host\":" << json_string(settings_.obs.host)
         << ",\"port\":" << settings_.obs.port << ",\"source\":" << json_string(settings_.obs.source)
         << ",\"fps\":" << settings_.obs.fps
         << ",\"autoSize\":" << (settings_.obs.auto_size ? "true" : "false")
         << ",\"testColor\":" << (settings_.obs.test_color ? "true" : "false")
         << ",\"color\":" << json_string(obs_color(settings_.obs))
         << ",\"passwordConfigured\":" << (!settings_.obs.password.empty() ? "true" : "false")
         << ",\"passwordNeedsReentry\":" << (settings_.obs_password_needs_reentry ? "true" : "false") << "}"
         << ",\"rgbPipe\":{\"name\":" << json_string(settings_.rgb_pipe)
         << ",\"active\":" << (!pipe.path.empty() ? "true" : "false")
         << ",\"listening\":" << (pipe.listening ? "true" : "false")
         << ",\"connected\":" << (pipe.connected ? "true" : "false")
         << ",\"frames\":" << pipe.frames
         << ",\"frameAgeMs\":" << (pipe.last_frame_ms && now >= pipe.last_frame_ms ? now - pipe.last_frame_ms : 0)
         << ",\"width\":" << pipe.frame_size.width << ",\"height\":" << pipe.frame_size.height << "}"
         << ",\"controlPipe\":{\"name\":" << json_string(settings_.control_pipe)
         << ",\"running\":" << (control_->running() ? "true" : "false") << "}"
         << ",\"maxCameras\":" << max_virtual_cameras << ",\"cameras\":[";
    for (size_t index = 0; index < max_virtual_cameras; ++index) {
        if (index) json << ',';
        if (cameras_[index]) {
            const CameraSessionStatus& camera = cameraStatus[index];
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (camera < 1 || camera > static_cast<int>(max_virtual_cameras) || !cameras_[camera - 1])
        return std::nullopt;
    return static_cast<CameraSession*>(cameras_[camera - 1].get())->snapshot();
}

bool RuntimeController::apply(const WebUpdate& update, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (update.camera_operation == WebCameraOperation::add) return add_camera(update.camera, error);
    if (update.camera_operation == WebCameraOperation::remove) return remove_camera(update.camera, error);
    if (update.has_input_settings && !input_mode_available_) {
        error = "input-mode-locked-by-file-argument";
        return false;
    }
    if (update.camera < 1 || update.camera > static_cast<int>(max_virtual_cameras) ||
        !cameras_[update.camera - 1]) {
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
    if (update.has_input_settings) {
        replacement.input_mode = update.input_mode;
        replacement.obs = update.obs;
        if (!update.obs_password_changed) replacement.obs.password = settings_.obs.password;
        if (update.clear_obs_password) replacement.obs.password.clear();
        if (update.obs_password_changed || update.clear_obs_password) {
            replacement.obs_password_encrypted.clear();
            replacement.obs_password_needs_reentry = false;
        }
        if (previous.input_mode != replacement.input_mode && !replacement.camera_name_configured[update.camera - 1])
            replacement.cameras[update.camera - 1].name = replacement.input_mode == "obs"
                ? "OBS HD Camera " + std::to_string(update.camera)
                : "Pipe HD Camera " + std::to_string(update.camera);
    }

    CameraSession& camera = *static_cast<CameraSession*>(cameras_[update.camera - 1].get());
    const CameraSettings previous_camera_settings = settings_.cameras[update.camera - 1];
    const CameraSettings next_camera_settings = replacement.cameras[update.camera - 1];
    const bool camera_changed = previous_camera_settings.resolution != next_camera_settings.resolution ||
        previous_camera_settings.fps != next_camera_settings.fps || previous_camera_settings.vid != next_camera_settings.vid ||
        previous_camera_settings.pid != next_camera_settings.pid ||
        previous_camera_settings.manufacturer != next_camera_settings.manufacturer ||
        previous_camera_settings.name != next_camera_settings.name || previous_camera_settings.yuy2 != next_camera_settings.yuy2 ||
        previous_camera_settings.mjpeg != next_camera_settings.mjpeg;
    if (camera_changed && !camera.reconfigure(next_camera_settings, error)) return false;
    const auto primary_camera = [this](const AppSettings& value) {
        for (size_t index = 0; index < max_virtual_cameras; ++index)
            if (value.camera_enabled[index] && cameras_[index]) return static_cast<int>(index);
        return -1;
    };
    const int primary_index = primary_camera(settings_);
    if (update.camera - 1 == primary_index && camera_changed)
        frame_source_set_target(source_, next_camera_settings.resolution);

    const bool control_changed = previous.control_pipe != replacement.control_pipe ||
        previous.control_pipe_access != replacement.control_pipe_access;
    if (control_changed && !control_->reconfigure(replacement.control_pipe, replacement.control_pipe_access)) {
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        if (update.camera - 1 == primary_index && camera_changed) frame_source_set_target(source_, previous_camera_settings.resolution);
        error = "control-pipe-reconfigure-failed";
        return false;
    }

    const PipeReaderStatus pipe_status = frame_source_pipe_status(source_);
    const bool rgb_changed = !pipe_status.path.empty() &&
        previous.rgb_pipe != replacement.rgb_pipe;
    if (rgb_changed && !frame_source_reconfigure_pipe(source_, replacement.rgb_pipe.c_str(), replacement.rgb_pipe_access)) {
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        if (update.camera - 1 == primary_index && camera_changed) frame_source_set_target(source_, previous_camera_settings.resolution);
        error = "rgb-pipe-reconfigure-failed";
        return false;
    }

    const bool input_changed = previous.input_mode != replacement.input_mode ||
        (previous.input_mode == "obs" && !obs_settings_equal(previous.obs, replacement.obs));
    if (input_changed && !frame_source_set_input(source_, replacement.input_mode, replacement.obs,
        replacement.rgb_pipe.c_str(), replacement.rgb_pipe_access,
        replacement.cameras[primary_index >= 0 ? primary_index : 0].resolution, error)) {
        if (rgb_changed) frame_source_reconfigure_pipe(source_, previous.rgb_pipe.c_str(), previous.rgb_pipe_access);
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        if (update.camera - 1 == primary_index && camera_changed) frame_source_set_target(source_, previous_camera_settings.resolution);
        if (error.empty()) error = "input-reconfigure-failed";
        return false;
    }

    if (!save_app_settings(config_path_, replacement, error)) {
        if (input_changed) {
            std::string ignored;
            frame_source_set_input(source_, previous.input_mode, previous.obs,
                previous.rgb_pipe.c_str(), previous.rgb_pipe_access,
                previous.cameras[primary_index >= 0 ? primary_index : 0].resolution, ignored);
        } else if (rgb_changed) frame_source_reconfigure_pipe(source_, previous.rgb_pipe.c_str(), previous.rgb_pipe_access);
        if (control_changed) control_->reconfigure(previous.control_pipe, previous.control_pipe_access);
        if (camera_changed) { std::string ignored; camera.reconfigure(previous_camera_settings, ignored); }
        if (update.camera - 1 == primary_index && camera_changed) frame_source_set_target(source_, previous_camera_settings.resolution);
        return false;
    }
    settings_ = std::move(replacement);
    return true;
}

bool RuntimeController::add_camera(int template_camera, std::string& error) {
    if (template_camera < 1 || template_camera > static_cast<int>(max_virtual_cameras) ||
        !cameras_[template_camera - 1]) {
        error = "template-camera-not-found";
        return false;
    }
    size_t slot = max_virtual_cameras;
    for (size_t index = 0; index < max_virtual_cameras; ++index) {
        if (!cameras_[index]) { slot = index; break; }
    }
    if (slot == max_virtual_cameras) {
        error = "maximum-camera-count-reached";
        return false;
    }

    AppSettings replacement = settings_;
    CameraSettings added = settings_.cameras[template_camera - 1];
    added.vid = static_cast<uint16_t>(0xbb10 + slot);
    added.pid = static_cast<uint16_t>(0xcc10 + slot);
    added.name = (input_mode_available_ && replacement.input_mode == "obs" ? "OBS HD Camera " :
        input_mode_available_ ? "Pipe HD Camera " : "MP4 HD Camera ") + std::to_string(slot + 1);
    replacement.cameras[slot] = added;
    replacement.camera_enabled[slot] = true;
    replacement.camera_name_configured[slot] = false;

    CameraHandle created = camera_factory_(replacement.cameras[slot], static_cast<int>(slot + 1), error);
    if (!created) {
        if (error.empty()) error = "camera-create-failed";
        return false;
    }
    if (!save_app_settings(config_path_, replacement, error)) return false;
    cameras_[slot] = std::move(created);
    settings_ = std::move(replacement);
    router_->set_camera(slot, camera_actions(slot));
    return true;
}

bool RuntimeController::remove_camera(int camera, std::string& error) {
    if (camera < 1 || camera > static_cast<int>(max_virtual_cameras) || !cameras_[camera - 1]) {
        error = "camera-not-found";
        return false;
    }
    size_t active = 0;
    for (const auto& item : cameras_) if (item) ++active;
    if (active <= 1) {
        error = "at-least-one-camera-required";
        return false;
    }

    AppSettings replacement = settings_;
    replacement.camera_enabled[camera - 1] = false;
    if (!save_app_settings(config_path_, replacement, error)) return false;
    router_->set_camera(static_cast<size_t>(camera - 1), {});
    cameras_[camera - 1].reset();
    settings_ = std::move(replacement);
    for (size_t index = 0; index < max_virtual_cameras; ++index) {
        if (cameras_[index]) {
            frame_source_set_target(source_, settings_.cameras[index].resolution);
            break;
        }
    }
    return true;
}
}

#include "camera_session.h"
#include <cstdio>
#include <stdexcept>

namespace {
bool valid_usb_text(const std::string& value) {
    if (value.empty() || value.size() > 504 || value.find('\0') != std::string::npos ||
        value.find('\r') != std::string::npos || value.find('\n') != std::string::npos)
        return false;
    const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    return characters > 0 && characters <= 126;
}
}

namespace vcam {
CameraSession::CameraSession(const uvc_vcam_t& config, Resolution size, std::unique_ptr<UsbTransport> transport,
                             MJPEG_FRAME_CALLBACK mjpeg_callback)
    : manufacturer_(config.manu_fact ? config.manu_fact : "Fanxiushu"),
      product_(config.product ? config.product : "Virtual Camera"), config_(config),
      mjpeg_callback_(mjpeg_callback), transport_(std::move(transport)),
      stop_(CreateEvent(NULL, TRUE, FALSE, NULL)) {
    if (!transport_ || !stop_) throw std::runtime_error("Virtual USB transport unavailable");
    config_.manu_fact = manufacturer_.c_str();
    config_.product = product_.c_str();
    initial_size_ = size;
    settings_.resolution = size;
    settings_.fps = config.fps;
    settings_.vid = static_cast<uint16_t>(config.vid);
    settings_.pid = static_cast<uint16_t>(config.pid);
    settings_.manufacturer = manufacturer_;
    settings_.name = product_;
    settings_.yuy2 = config.enable_yuy2;
    settings_.mjpeg = config.enable_mjpeg && mjpeg_callback_ != nullptr;
    char id[64] = {};
    sprintf_s(id, "usb\\vid_%04x&pid_%04x", config.vid, config.pid);
    identity_ = {id, id, "USB\\DevClass_00&SubClass_00\nUSB\\DevClass_00\nUSB\\COMPOSITE"};
}
CameraSession::~CameraSession() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    stop_worker();
    transport_->resume();
    if (plugged_) transport_->unplug();
}
bool CameraSession::start_worker() {
    ResetEvent(stop_.get());
    transport_->resume();
    if (config_.resume) config_.resume(config_.param);
    try { running_.store(true); worker_ = std::thread(&CameraSession::run, this); }
    catch (...) { running_.store(false); return false; }
    return true;
}
void CameraSession::stop_worker() noexcept {
    SetEvent(stop_.get());
    try { if (config_.request_stop) config_.request_stop(config_.param); }
    catch (...) { fprintf(stderr, "Frame source stop callback failed.\n"); }
    transport_->interrupt();
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}
bool CameraSession::start() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (worker_.joinable() || plugged_) return false;
    transport_->resume();
    plugged_ = transport_->plug(identity_);
    if (!plugged_) return false;
    fprintf(stderr, "Virtual USB link: %s\n",
        transport_->is_superspeed() ? "SuperSpeed" : "High-Speed (USB 2.0)");
    try {
        device_ = std::make_unique<UvcDevice>(config_, initial_size_, mjpeg_callback_, transport_->is_superspeed());
    } catch (...) {
        transport_->resume();
        if (transport_->unplug()) plugged_ = false;
        return false;
    }
    if (start_worker()) return true;
    transport_->resume();
    if (transport_->unplug()) {
        plugged_ = false;
        device_.reset();
    }
    return false;
}
CameraSessionStatus CameraSession::status_snapshot() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_);
    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
    CameraSessionStatus result;
    result.size = device_ ? device_->resolution() : Resolution{};
    result.running = running_.load();
    result.actual_fps = device_ ? device_->actual_fps() : 0;
    if (plugged_) result.usb_mode = transport_->is_superspeed() ? "superspeed" : "high-speed";
    return result;
}
Resolution CameraSession::resolution() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_);
    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
    return device_ ? device_->resolution() : Resolution{};
}
CameraFrame CameraSession::snapshot() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_);
    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
    return device_ ? CameraFrame{device_->resolution(), device_->frame_copy()} : CameraFrame{};
}
bool CameraSession::resize(Resolution size, std::string& error) {
    if (!valid_output(size)) { error = "invalid-resolution"; return false; }
    CameraSettings replacement;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        replacement = settings_;
    }
    replacement.resolution = size;
    return reconfigure(replacement, error);
}
bool CameraSession::reconfigure(const CameraSettings& replacement, std::string& error) {
    if (!valid_output(replacement.resolution) || replacement.fps < 1 || replacement.fps > 60 ||
        replacement.vid == 0 || replacement.pid == 0 || (!replacement.yuy2 && !replacement.mjpeg) ||
        !valid_usb_text(replacement.manufacturer) || !valid_usb_text(replacement.name)) {
        error = "invalid-settings";
        return false;
    }
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (!plugged_ || !running_) { error = "camera-not-running"; return false; }
    if (settings_.resolution == replacement.resolution && settings_.fps == replacement.fps &&
        settings_.vid == replacement.vid && settings_.pid == replacement.pid &&
        settings_.manufacturer == replacement.manufacturer && settings_.name == replacement.name &&
        settings_.yuy2 == replacement.yuy2 && settings_.mjpeg == replacement.mjpeg) return true;

    uvc_vcam_t nextConfig = config_;
    nextConfig.vid = replacement.vid;
    nextConfig.pid = replacement.pid;
    nextConfig.manu_fact = replacement.manufacturer.c_str();
    nextConfig.product = replacement.name.c_str();
    nextConfig.fps = replacement.fps;
    nextConfig.enable_yuy2 = replacement.yuy2;
    nextConfig.enable_mjpeg = replacement.mjpeg;
    auto nextDevice = std::make_unique<UvcDevice>(nextConfig, replacement.resolution,
        mjpeg_callback_, transport_->is_superspeed());
    char id[64] = {};
    sprintf_s(id, "usb\\vid_%04x&pid_%04x", replacement.vid, replacement.pid);
    DeviceIdentity nextIdentity{id, id, "USB\\DevClass_00&SubClass_00\nUSB\\DevClass_00\nUSB\\COMPOSITE"};

    const CameraSettings previousSettings = settings_;
    const DeviceIdentity previousIdentity = identity_;
    const std::string previousManufacturer = manufacturer_;
    const std::string previousProduct = product_;
    auto previousDevice = std::move(device_);
    stop_worker();
    transport_->resume();
    if (!transport_->unplug()) {
        device_ = std::move(previousDevice);
        const bool restored = start_worker();
        error = restored ? "unplug-failed-restored-previous" : "unplug-failed-camera-stopped";
        return false;
    }
    plugged_ = false;
    manufacturer_ = replacement.manufacturer;
    product_ = replacement.name;
    config_.vid = replacement.vid;
    config_.pid = replacement.pid;
    config_.manu_fact = manufacturer_.c_str();
    config_.product = product_.c_str();
    config_.fps = replacement.fps;
    config_.enable_yuy2 = replacement.yuy2;
    config_.enable_mjpeg = replacement.mjpeg;
    settings_ = replacement;
    identity_ = std::move(nextIdentity);
    device_ = std::move(nextDevice);
    plugged_ = transport_->plug(identity_);
    if (plugged_ && start_worker()) return true;

    transport_->resume();
    if (plugged_) transport_->unplug();
    plugged_ = false;
    manufacturer_ = previousManufacturer;
    product_ = previousProduct;
    config_.vid = previousSettings.vid;
    config_.pid = previousSettings.pid;
    config_.manu_fact = manufacturer_.c_str();
    config_.product = product_.c_str();
    config_.fps = previousSettings.fps;
    config_.enable_yuy2 = previousSettings.yuy2;
    config_.enable_mjpeg = previousSettings.mjpeg;
    settings_ = previousSettings;
    identity_ = previousIdentity;
    device_ = std::move(previousDevice);
    plugged_ = transport_->plug(identity_);
    const bool restored = plugged_ && start_worker();
    error = restored ? "reconfigure-failed-restored-previous" : "reconfigure-and-rollback-failed";
    return false;
}
void CameraSession::run() noexcept {
    static const bool traceUsb = [] {
        char value[2] = {};
        return GetEnvironmentVariableA("VCAM_USB_TRACE", value, sizeof(value)) != 0;
    }();
    unsigned long long traceSequence = 0;
    try {
        while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
            auto* request = transport_->begin();
            if (!request) {
                if (traceUsb) fprintf(stderr, "USB trace: begin stopped\n");
                break;
            }
            const bool traceRequest = traceUsb && traceSequence < 128;
            if (traceRequest) {
                if (request->type == 3 && request->transfer.type == 3) {
                    fprintf(stderr, "USB trace[%llu]: begin type=3 transfer=%d ep=0x%02x read=%d packets=%d bytes=%d\n",
                        traceSequence, request->transfer.type, request->transfer.ep_address,
                        request->transfer.is_read, request->transfer.number_packets, request->data_length);
                } else if (request->type == 3) {
                    fprintf(stderr, "USB trace[%llu]: begin type=3 transfer=%d ep=0x%02x read=%d bytes=%d\n",
                        traceSequence, request->transfer.type, request->transfer.ep_address,
                        request->transfer.is_read, request->data_length);
                } else {
                    fprintf(stderr, "USB trace[%llu]: begin type=%d bytes=%d\n",
                        traceSequence, request->type, request->data_length);
                }
            }
            const bool cycle = request->type == 4 && request->reset.type == 2;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                device_->handle(*request, transport_->capacity());
            }
            if (traceRequest) {
                fprintf(stderr, "USB trace[%llu]: handled result=%d bytes=%d\n",
                    traceSequence, request->result, request->data_length);
            }
            const UsbEndResult end_result = transport_->end(request);
            if (traceRequest) {
                fprintf(stderr, "USB trace[%llu]: end=%d\n", traceSequence, static_cast<int>(end_result));
            }
            ++traceSequence;
            if (end_result == UsbEndResult::failed) break;
            if (cycle && end_result == UsbEndResult::completed) {
                if (!transport_->replug()) break;
                std::lock_guard<std::mutex> lock(frame_mutex_);
                if (!device_->set_resolution(device_->resolution())) break;
            }
        }
    }
    catch (const std::exception& error) { fprintf(stderr, "USB worker: %s\n", error.what()); }
    catch (...) { fprintf(stderr, "USB worker stopped after an unknown error.\n"); }
    running_.store(false);
}
}

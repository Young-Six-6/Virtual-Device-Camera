#include "camera_session.h"
#include <cstdio>
#include <stdexcept>

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
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (!plugged_ || !running_) { error = "camera-not-running"; return false; }
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex_);
        if (device_->resolution() == size) return true;
    }
    // Allocate descriptors and frame storage before disrupting the active device.
    auto replacement = std::make_unique<UvcDevice>(config_, size, mjpeg_callback_, transport_->is_superspeed());
    stop_worker();
    transport_->resume();
    if (!transport_->unplug()) {
        error = "unplug-failed-camera-stopped";
        return false;
    }
    plugged_ = false;
    device_.swap(replacement);
    plugged_ = transport_->plug(identity_);
    if (plugged_ && start_worker()) return true;
    transport_->resume();
    if (plugged_) transport_->unplug();
    device_.swap(replacement);
    plugged_ = transport_->plug(identity_);
    const bool restored = plugged_ && start_worker();
    error = restored ? "resize-failed-restored-previous" : "resize-and-rollback-failed";
    return false;
}
void CameraSession::run() noexcept {
    try {
        while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
            auto* request = transport_->begin();
            if (!request) break;
            const bool cycle = request->type == 4 && request->reset.type == 2;
            int delay = 0;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                delay = device_->delay_before(*request);
            }
            if (delay && WaitForSingleObject(stop_.get(), delay) == WAIT_OBJECT_0) break;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                device_->handle(*request, transport_->capacity());
            }
            const UsbEndResult end_result = transport_->end(request);
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

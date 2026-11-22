#include "camera_session.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
thread_local const vcam::CameraSession* active_camera_worker = nullptr;

class CameraWorkerScope {
    const vcam::CameraSession* previous_;
public:
    explicit CameraWorkerScope(const vcam::CameraSession* session) noexcept
        : previous_(active_camera_worker) { active_camera_worker = session; }
    ~CameraWorkerScope() { active_camera_worker = previous_; }
};

bool valid_usb_text(const std::string& value) {
    if (value.empty() || value.size() > 504 || value.find('\0') != std::string::npos ||
        value.find('\r') != std::string::npos || value.find('\n') != std::string::npos)
        return false;
    const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    return characters > 0 && characters <= 126;
}
uint16_t little16(const unsigned char* value) {
    return static_cast<uint16_t>(value[0] | (static_cast<uint16_t>(value[1]) << 8));
}
}

namespace vcam {
bool is_camera_worker_thread() noexcept { return active_camera_worker != nullptr; }

CameraSession::CameraSession(const uvc_vcam_t& config, Resolution size, std::unique_ptr<UsbTransport> transport,
                             MJPEG_FRAME_CALLBACK mjpeg_callback,
                             USB_REQUEST_CALLBACK usb_request_callback,
                             void* usb_request_context)
    : manufacturer_(config.manu_fact ? config.manu_fact : "Fanxiushu"),
      product_(config.product ? config.product : "Virtual Camera"), config_(config),
      mjpeg_callback_(mjpeg_callback), usb_request_callback_(usb_request_callback),
      usb_request_context_(usb_request_context), transport_(std::move(transport)),
      usb_device_speed_(transport_ && transport_->is_superspeed()
          ? VCAM_USB_SPEED_SUPER
          : VCAM_USB_SPEED_HIGH),
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
    transport_removed_.store(false, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);
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
    const bool transportRemoved = transport_removed_.exchange(false, std::memory_order_acq_rel);
    const bool workerFailed = worker_failed_.exchange(false, std::memory_order_acq_rel);
    if (transportRemoved || workerFailed) {
        stop_worker();
        transport_->resume();
        if (!transport_->unplug()) {
            worker_failed_.store(true, std::memory_order_release);
            return false;
        }
        plugged_ = false;
        device_.reset();
    }
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
    result.driver_kind = transport_->driver_kind();
    if (plugged_ && !transport_removed_.load(std::memory_order_acquire))
        result.usb_mode = transport_->is_superspeed() ? "superspeed" : "high-speed";
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
    const bool transportRemoved = transport_removed_.exchange(false, std::memory_order_acq_rel);
    const bool workerFailed = worker_failed_.exchange(false, std::memory_order_acq_rel);
    if (transportRemoved || workerFailed) plugged_ = false;
    if ((!plugged_ || !running_) && !transportRemoved && !workerFailed) { error = "camera-not-running"; return false; }
    if (settings_.resolution == replacement.resolution && settings_.fps == replacement.fps &&
        settings_.vid == replacement.vid && settings_.pid == replacement.pid &&
        settings_.manufacturer == replacement.manufacturer && settings_.name == replacement.name &&
        settings_.yuy2 == replacement.yuy2 && settings_.mjpeg == replacement.mjpeg &&
        !transportRemoved && !workerFailed) return true;

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
    stop_worker();
    auto previousDevice = std::move(device_);
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
    if (plugged_ && start_worker()) {
        transport_removed_.store(false, std::memory_order_release);
        return true;
    }

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
    transport_removed_.store(false, std::memory_order_release);
    worker_failed_.store(!restored, std::memory_order_release);
    error = restored ? "reconfigure-failed-restored-previous" : "reconfigure-and-rollback-failed";
    return false;
}
bool CameraSession::reconnect_removed_transport(bool traceUsb) {
    /* Consume removal even if stop raced with it. The next start/reconfigure
     * must know that the transport's PDO and event handles were already torn
     * down by virt_usb_begin(). */
    if (!transport_->consume_removed()) return false;
    transport_removed_.store(true, std::memory_order_release);
    if (WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0) return false;

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0) break;
        if (traceUsb) fprintf(stderr, "USB transport removed; reconnect attempt %u/3\n", attempt + 1);
        if (transport_->replug()) {
            bool restored = false;
            {
                std::lock_guard<std::mutex> frame_lock(frame_mutex_);
                restored = device_ && device_->set_resolution(device_->resolution());
            }
            if (restored) {
                transport_removed_.store(false, std::memory_order_release);
                if (traceUsb) fprintf(stderr, "USB transport reconnected\n");
                return true;
            }
            transport_->unplug();
        }
        if (WaitForSingleObject(stop_.get(), 250) == WAIT_OBJECT_0) break;
    }
    if (traceUsb) fprintf(stderr, "USB transport reconnect failed\n");
    return false;
}
CameraSession::UsbRequestDisposition CameraSession::handle_usb_request(
    usbtx_header_t& wire,
    int requestedLength,
    size_t payloadCapacity) noexcept {
    if (usb_request_callback_ == nullptr || requestedLength < 0 ||
        static_cast<size_t>(requestedLength) > payloadCapacity)
        return UsbRequestDisposition::pass_through;

    vcam_usb_request_t request{};
    request.device_speed = usb_device_speed_;
    request.transfer_length = static_cast<unsigned int>(requestedLength);
    request.buffer_capacity = static_cast<unsigned int>(requestedLength);

    switch (wire.type) {
    case 1:
        if (wire.descriptor.type == 5) {
            const unsigned int packedIndex = static_cast<unsigned int>(wire.descriptor.index);
            request.kind = VCAM_USB_REQUEST_MS_FEATURE_DESCRIPTOR;
            request.is_read = wire.descriptor.is_read != 0;
            request.descriptor_type = 5;
            request.recipient = (packedIndex >> 8) & 0xff;
            request.interface_number = packedIndex & 0xff;
            request.page_index = (packedIndex >> 16) & 0xff;
            request.feature_descriptor_index = static_cast<unsigned int>(wire.descriptor.value) & 0xffff;
            request.index = packedIndex;
            request.value = static_cast<unsigned int>(wire.descriptor.value) & 0xffff;
        } else {
            request.kind = VCAM_USB_REQUEST_DESCRIPTOR;
            request.is_read = wire.descriptor.is_read != 0;
            request.descriptor_recipient = static_cast<unsigned int>(wire.descriptor.type);
            request.descriptor_type = static_cast<unsigned int>(wire.descriptor.subtype);
            request.descriptor_index = static_cast<unsigned int>(wire.descriptor.index);
            request.language_id = static_cast<unsigned int>(wire.descriptor.value);
            request.index = request.descriptor_index;
            request.value = request.language_id;
        }
        break;
    case 2:
        request.kind = VCAM_USB_REQUEST_VENDOR_CONTROL;
        request.is_read = wire.vendor.is_read != 0;
        request.request_type = wire.vendor.type;
        request.request_subtype = wire.vendor.subtype;
        request.request = wire.vendor.request;
        request.index = static_cast<unsigned int>(wire.vendor.index);
        request.value = static_cast<unsigned int>(wire.vendor.value);
        break;
    case 3:
        if (wire.transfer.type == 1) {
            request.kind = VCAM_USB_REQUEST_CONTROL_TRANSFER;
            request.is_read = wire.transfer.is_read != 0;
            request.endpoint_address = 0;
            request.pipe_type = 0;
            std::memcpy(request.setup_packet, wire.transfer.setup_packet,
                        sizeof(request.setup_packet));
            {
                const bool setupIsRead = (request.setup_packet[0] & 0x80) != 0;
                const unsigned int setupLength = little16(request.setup_packet + 6);
                if (setupIsRead != (request.is_read != 0) ||
                    (!setupIsRead && static_cast<unsigned int>(requestedLength) != setupLength)) {
                    wire.result = -1;
                    wire.data_length = 0;
                    return UsbRequestDisposition::stall;
                }
            }
            request.request = request.setup_packet[1];
            request.value = little16(request.setup_packet + 2);
            request.index = little16(request.setup_packet + 4);
            request.timeout_ms = wire.transfer.timeout;
            request.transfer_length = (std::min)(request.transfer_length,
                static_cast<unsigned int>(little16(request.setup_packet + 6)));
            request.buffer_capacity = request.transfer_length;
        } else if (wire.transfer.type == 2) {
            request.kind = VCAM_USB_REQUEST_BULK_OR_INTERRUPT_TRANSFER;
            request.is_read = wire.transfer.is_read != 0;
            request.endpoint_address = wire.transfer.ep_address;
            request.pipe_type = wire.transfer.reserved0;
        } else {
            /* Isochronous video remains owned by the built-in UVC responder. */
            return UsbRequestDisposition::pass_through;
        }
        break;
    case 4:
        request.kind = VCAM_USB_REQUEST_RESET;
        request.endpoint_address = wire.reset.ep_address;
        request.reset_type = wire.reset.type;
        break;
    case 5:
        request.kind = VCAM_USB_REQUEST_STATUS_QUERY;
        request.is_read = 1;
        request.status_type = wire.status.type;
        request.index = static_cast<unsigned int>(wire.status.index);
        break;
    case 6:
        request.kind = VCAM_USB_REQUEST_FEATURE;
        request.feature_type = wire.feature.type;
        request.feature_subtype = wire.feature.subtype;
        request.index = static_cast<unsigned int>(wire.feature.index);
        request.value = static_cast<unsigned int>(wire.feature.value);
        break;
    default:
        return UsbRequestDisposition::pass_through;
    }

    const bool isRead = request.is_read != 0;
    const unsigned int inputCapacity = request.buffer_capacity;
    if (request.is_read) {
        request.input_data = reinterpret_cast<unsigned char*>(wire.data);
        request.output_data = nullptr;
        request.response_length = 0;
    } else {
        request.input_data = nullptr;
        request.output_data = reinterpret_cast<const unsigned char*>(wire.data);
        request.response_length = 0;
    }

    int callbackResult = -1;
    try {
        callbackResult = usb_request_callback_(&request, usb_request_context_);
    } catch (...) {
        wire.result = -1;
        wire.data_length = 0;
        return UsbRequestDisposition::stall;
    }
    if (callbackResult == VCAM_USB_REQUEST_PASSTHROUGH)
        return UsbRequestDisposition::pass_through;
    if (callbackResult != VCAM_USB_REQUEST_HANDLED) {
        wire.result = -1;
        wire.data_length = 0;
        return UsbRequestDisposition::stall;
    }
    if (isRead) {
        if (request.response_length > inputCapacity) {
            wire.result = -1;
            wire.data_length = 0;
            return UsbRequestDisposition::stall;
        }
        wire.data_length = static_cast<int>(request.response_length);
    } else {
        /* OUT payload was consumed above; END only needs to acknowledge it. */
        wire.data_length = 0;
    }
    wire.result = 0;
    return UsbRequestDisposition::handled;
}
void CameraSession::run() noexcept {
    CameraWorkerScope workerScope(this);
    static const bool traceUsb = [] {
        char value[2] = {};
        return GetEnvironmentVariableA("VCAM_USB_TRACE", value, sizeof(value)) != 0;
    }();
    unsigned long long traceSequence = 0;
    try {
        while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
            auto* request = transport_->begin();
            if (!request) {
                if (reconnect_removed_transport(traceUsb)) continue;
                if (traceUsb) fprintf(stderr, "USB trace: begin stopped or transport unavailable\n");
                break;
            }
            const bool traceRequest = traceUsb && traceSequence < 128;
            const int requestedLength = request->data_length;
            const size_t payloadCapacity = transport_->capacity();
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
            bool processingFailed = false;
            try {
                const UsbRequestDisposition disposition = handle_usb_request(
                    *request, requestedLength, payloadCapacity);
                if (disposition == UsbRequestDisposition::pass_through) {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    device_->handle(*request, payloadCapacity);
                }
            } catch (const std::exception& error) {
                request->result = -1;
                request->data_length = 0;
                processingFailed = true;
                if (traceUsb) fprintf(stderr, "USB worker: request callback threw: %s\n", error.what());
            } catch (...) {
                request->result = -1;
                request->data_length = 0;
                processingFailed = true;
                if (traceUsb) fprintf(stderr, "USB worker: request callback threw an unknown exception.\n");
            }
            if (traceRequest) {
                fprintf(stderr, "USB trace[%llu]: handled result=%d bytes=%d\n",
                    traceSequence, request->result, request->data_length);
            }
            /* The transport owns the transaction buffer after END. Cache the
             * reset result before completion so no code reads the request once
             * the backend may have released or recycled its storage. */
            const int requestResult = request->result;
            const UsbEndResult end_result = transport_->end(request);
            if (traceRequest) {
                fprintf(stderr, "USB trace[%llu]: end=%d\n", traceSequence, static_cast<int>(end_result));
            }
            ++traceSequence;
            if (processingFailed) break;
            if (end_result == UsbEndResult::failed) {
                if (reconnect_removed_transport(traceUsb)) continue;
                break;
            }
            if (cycle && requestResult == 0 && end_result == UsbEndResult::completed) {
                if (!transport_->replug()) break;
                std::lock_guard<std::mutex> lock(frame_mutex_);
                if (!device_->set_resolution(device_->resolution())) break;
            }
        }
    }
    catch (const std::exception& error) { fprintf(stderr, "USB worker: %s\n", error.what()); }
    catch (...) { fprintf(stderr, "USB worker stopped after an unknown error.\n"); }
    if (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
        worker_failed_.store(true, std::memory_order_release);
    running_.store(false);
}
}

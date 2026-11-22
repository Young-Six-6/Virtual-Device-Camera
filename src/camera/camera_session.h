#pragma once
#include "uvc_device.h"
#include "../common/camera_settings.h"
#include "../usb/transport.h"
#include "../common/win_handle.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcam {
struct CameraFrame {
    Resolution size;
    std::vector<char> yuy2;
};

struct CameraSessionStatus {
    Resolution size;
    bool running = false;
    std::string usb_mode = "disconnected";
    double actual_fps = 0;
    VirtUsbDriverKind driver_kind = VirtUsbDriverKind::unavailable;
};

class CameraSession {
    enum class UsbRequestDisposition { pass_through, handled, stall };
    mutable std::mutex lifecycle_;
    mutable std::mutex frame_mutex_;
    std::string manufacturer_, product_;
    uvc_vcam_t config_;
    CameraSettings settings_;
    MJPEG_FRAME_CALLBACK mjpeg_callback_ = nullptr;
    USB_REQUEST_CALLBACK usb_request_callback_ = nullptr;
    void* usb_request_context_ = nullptr;
    Resolution initial_size_;
    std::unique_ptr<UvcDevice> device_;
    std::unique_ptr<UsbTransport> transport_;
    int usb_device_speed_ = VCAM_USB_SPEED_HIGH;
    DeviceIdentity identity_;
    unique_handle stop_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> transport_removed_{false};
    std::atomic<bool> worker_failed_{false};
    bool plugged_ = false;
    bool start_worker();
    void stop_worker() noexcept;
    bool reconnect_removed_transport(bool trace_usb);
    UsbRequestDisposition handle_usb_request(usbtx_header_t&, int requested_length,
                                             size_t payload_capacity) noexcept;
    void run() noexcept;
public:
    CameraSession(const uvc_vcam_t&, Resolution, std::unique_ptr<UsbTransport>,
                  MJPEG_FRAME_CALLBACK mjpeg_callback = nullptr,
                  USB_REQUEST_CALLBACK usb_request_callback = nullptr,
                  void* usb_request_context = nullptr);
    ~CameraSession();
    bool start();
    bool resize(Resolution, std::string& error);
    bool reconfigure(const CameraSettings&, std::string& error);
    CameraSessionStatus status_snapshot() const;
    Resolution resolution() const;
    CameraFrame snapshot() const;
    bool running() const { return running_.load(); }
};
bool is_camera_worker_thread() noexcept;
}

#pragma once
#include "uvc_device.h"
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
};

class CameraSession {
    mutable std::mutex lifecycle_;
    mutable std::mutex frame_mutex_;
    std::string manufacturer_, product_;
    uvc_vcam_t config_;
    MJPEG_FRAME_CALLBACK mjpeg_callback_ = nullptr;
    Resolution initial_size_;
    std::unique_ptr<UvcDevice> device_;
    std::unique_ptr<UsbTransport> transport_;
    DeviceIdentity identity_;
    unique_handle stop_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    bool plugged_ = false;
    bool start_worker();
    void stop_worker() noexcept;
    void run() noexcept;
public:
    CameraSession(const uvc_vcam_t&, Resolution, std::unique_ptr<UsbTransport>,
                  MJPEG_FRAME_CALLBACK mjpeg_callback = nullptr);
    ~CameraSession();
    bool start();
    bool resize(Resolution, std::string& error);
    CameraSessionStatus status_snapshot() const;
    Resolution resolution() const;
    CameraFrame snapshot() const;
    bool running() const { return running_.load(); }
};
}

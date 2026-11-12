#pragma once
#include "uvc_device.h"
#include "../usb/transport.h"
#include "../common/win_handle.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace vcam {
class CameraSession {
    mutable std::mutex lifecycle_;
    std::string manufacturer_, product_;
    uvc_vcam_t config_;
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
    CameraSession(const uvc_vcam_t&, Resolution, std::unique_ptr<UsbTransport>);
    ~CameraSession();
    bool start();
    bool resize(Resolution, std::string& error);
    Resolution resolution() const;
    bool running() const { return running_.load(); }
};
}

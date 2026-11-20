#pragma once
#include "pipe_reader.h"
#include "../common/obs_settings.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vcam {
struct ObsCaptureStatus {
    std::string state = "stopped";
    std::string error;
    uint64_t frames = 0;
    uint64_t last_frame_ms = 0;
    Resolution frame_size{0, 0};
};

class ObsCapture {
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    ObsSettings settings_;
    Resolution target_{640, 480};
    std::shared_ptr<const RgbFrame> latest_;
    ObsCaptureStatus status_;
    bool stopping_ = false;
    void run() noexcept;
public:
    ObsCapture(ObsSettings settings, Resolution target);
    ~ObsCapture();
    ObsCapture(const ObsCapture&) = delete;
    ObsCapture& operator=(const ObsCapture&) = delete;
    bool start(std::string& error);
    void stop() noexcept;
    void set_target(Resolution target);
    std::shared_ptr<const RgbFrame> latest() const;
    ObsCaptureStatus status() const;
};
}

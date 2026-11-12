#pragma once
#include "../common/resolution.h"
#include "../common/win_handle.h"
#include "../common/pipe_security.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcam {
struct RgbFrame { Resolution size; std::vector<unsigned char> pixels; };
struct PipeReaderStatus {
    std::string path;
    bool listening = false;
    bool connected = false;
    uint64_t frames = 0;
    uint64_t last_frame_ms = 0;
    Resolution frame_size{0, 0};
};
class PipeReader {
    unique_handle pipe_;
    unique_handle stop_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::shared_ptr<const RgbFrame> latest_;
    std::string path_;
    PipeAccess access_ = PipeAccess::owner;
    std::atomic<bool> running_{false};
    bool connected_ = false;
    uint64_t frames_ = 0;
    uint64_t last_frame_ms_ = 0;
    Resolution frame_size_{0, 0};
    void run() noexcept;
public:
    ~PipeReader();
    bool start(const std::string& path, PipeAccess access = PipeAccess::owner);
    bool reconfigure(const std::string& path, PipeAccess access);
    void stop() noexcept;
    std::shared_ptr<const RgbFrame> latest() const;
    PipeReaderStatus status() const;
};
}

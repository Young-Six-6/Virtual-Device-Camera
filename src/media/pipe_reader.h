#pragma once
#include "../common/resolution.h"
#include "../common/win_handle.h"
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcam {
struct RgbFrame { Resolution size; std::vector<unsigned char> pixels; };
class PipeReader {
    unique_handle pipe_;
    unique_handle stop_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::shared_ptr<const RgbFrame> latest_;
    void run() noexcept;
public:
    ~PipeReader();
    bool start(const std::string& path);
    void stop() noexcept;
    std::shared_ptr<const RgbFrame> latest() const;
};
}

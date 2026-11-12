#include "pipe_reader.h"
#include "../common/pipe_io.h"
#include "../common/pipe_security.h"
#include <cstdint>
#include <cstdio>

namespace vcam {
PipeReader::~PipeReader() { stop(); }
bool PipeReader::start(const std::string& path, PipeAccess access) {
    if (thread_.joinable()) return false;
    stop_.reset(CreateEvent(NULL, TRUE, FALSE, NULL));
    local_pipe_security security(access);
    if (!security) { stop_.reset(); return false; }
    pipe_.reset(CreateNamedPipeA(path.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 0, 1024 * 1024, 0, security.get()));
    if (!stop_ || !pipe_) { pipe_.reset(); stop_.reset(); return false; }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        access_ = access;
        frames_ = last_frame_ms_ = 0;
        frame_size_ = {0, 0};
    }
    running_.store(true);
    try { thread_ = std::thread(&PipeReader::run, this); }
    catch (...) { running_.store(false); pipe_.reset(); stop_.reset(); return false; }
    return true;
}
bool PipeReader::reconfigure(const std::string& path, PipeAccess access) {
    std::string previous_path;
    PipeAccess previous_access;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous_path = path_;
        previous_access = access_;
    }
    stop();
    if (start(path, access)) return true;
    if (!previous_path.empty()) start(previous_path, previous_access);
    return false;
}
void PipeReader::stop() noexcept {
    if (stop_) SetEvent(stop_.get());
    if (thread_.joinable()) thread_.join();
    pipe_.reset();
    stop_.reset();
    running_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
    connected_ = false;
}
std::shared_ptr<const RgbFrame> PipeReader::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}
PipeReaderStatus PipeReader::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {path_, running_.load(), connected_, frames_, last_frame_ms_, frame_size_};
}
void PipeReader::run() noexcept {
    while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
        try {
            if (!connect_pipe(pipe_.get(), stop_.get())) break;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                connected_ = true;
            }
            int32_t header[2] = {};
            if (read_exact(pipe_.get(), stop_.get(), header, sizeof(header)) &&
                valid_input({header[0], header[1]})) {
                const Resolution size{header[0], header[1]};
                while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
                    auto frame = std::make_shared<RgbFrame>();
                    frame->size = size;
                    // Extra zero padding keeps SIMD readers within allocated storage.
                    frame->pixels.resize(rgb_bytes(size) + 64, 0);
                    if (!read_exact(pipe_.get(), stop_.get(), frame->pixels.data(), rgb_bytes(size))) break;
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_ = std::move(frame);
                    frame_size_ = size;
                    ++frames_;
                    last_frame_ms_ = GetTickCount64();
                }
            }
        }
        catch (const std::exception& error) {
            fprintf(stderr, "Pipe receiver: %s\n", error.what());
            SetEvent(stop_.get());
            break;
        }
        catch (...) {
            SetEvent(stop_.get());
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_.reset();
            connected_ = false;
        }
        DisconnectNamedPipe(pipe_.get());
    }
    running_.store(false);
}
}

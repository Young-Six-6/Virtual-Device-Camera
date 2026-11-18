#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <io.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>

namespace vcam {
// Captures the existing CRT stdout/stderr streams, mirrors them to the original
// console, and keeps a bounded tail for the local Web UI.
class ConsoleLogCapture {
    HANDLE read_pipe_ = nullptr;
    HANDLE mirror_ = INVALID_HANDLE_VALUE;
    int saved_stdout_ = -1;
    int saved_stderr_ = -1;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::string secret_;
    size_t retained_bytes_ = 0;
    bool redirected_ = false;

    void append_line(std::string line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!secret_.empty()) {
            size_t position = 0;
            while ((position = line.find(secret_, position)) != std::string::npos) {
                line.replace(position, secret_.size(), "[redacted]");
                position += 10;
            }
        }
        retained_bytes_ += line.size();
        lines_.push_back(std::move(line));
        while (retained_bytes_ > 256 * 1024 && !lines_.empty()) {
            retained_bytes_ -= lines_.front().size();
            lines_.pop_front();
        }
    }

    void read_loop() noexcept {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(read_pipe_, buffer, sizeof(buffer), &count, nullptr) || !count) break;
            pending.append(buffer, count);
            size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline + 1);
                if (mirror_ != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(mirror_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
                }
                append_line(std::move(line));
                pending.erase(0, newline + 1);
            }
            if (pending.size() >= 16 * 1024) {
                pending += "\n[log line truncated]\n";
                if (mirror_ != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(mirror_, pending.data(), static_cast<DWORD>(pending.size()), &written, nullptr);
                }
                append_line(std::move(pending));
                pending.clear();
            }
        }
        if (!pending.empty()) {
            if (mirror_ != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(mirror_, pending.data(), static_cast<DWORD>(pending.size()), &written, nullptr);
            }
            append_line(std::move(pending));
        }
    }

public:
    ConsoleLogCapture() = default;
    ConsoleLogCapture(const ConsoleLogCapture&) = delete;
    ConsoleLogCapture& operator=(const ConsoleLogCapture&) = delete;
    ~ConsoleLogCapture() { stop(); }

    bool start() noexcept {
        if (redirected_) return true;
        saved_stdout_ = _dup(_fileno(stdout));
        saved_stderr_ = _dup(_fileno(stderr));
        if (saved_stdout_ < 0 || saved_stderr_ < 0) { stop(); return false; }

        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&read_pipe_, &write_pipe, &attributes, 0)) { stop(); return false; }
        const intptr_t raw_write = reinterpret_cast<intptr_t>(write_pipe);
        const int pipe_fd = _open_osfhandle(raw_write, _O_WRONLY | _O_BINARY);
        if (pipe_fd < 0) {
            CloseHandle(write_pipe);
            stop();
            return false;
        }

        mirror_ = reinterpret_cast<HANDLE>(_get_osfhandle(saved_stdout_));
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        redirected_ = true;
        const bool redirected_stdout = _dup2(pipe_fd, _fileno(stdout)) == 0;
        const bool redirected_stderr = redirected_stdout && _dup2(pipe_fd, _fileno(stderr)) == 0;
        _close(pipe_fd);
        if (!redirected_stdout || !redirected_stderr) { stop(); return false; }
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
        try { reader_ = std::thread(&ConsoleLogCapture::read_loop, this); }
        catch (...) { stop(); return false; }
        return true;
    }

    void stop() noexcept {
        if (redirected_) {
            fflush(stdout);
            fflush(stderr);
            if (saved_stdout_ >= 0) _dup2(saved_stdout_, _fileno(stdout));
            if (saved_stderr_ >= 0) _dup2(saved_stderr_, _fileno(stderr));
            redirected_ = false;
        }
        if (reader_.joinable()) reader_.join();
        if (read_pipe_) { CloseHandle(read_pipe_); read_pipe_ = nullptr; }
        if (saved_stdout_ >= 0) { _close(saved_stdout_); saved_stdout_ = -1; }
        if (saved_stderr_ >= 0) { _close(saved_stderr_); saved_stderr_ = -1; }
        mirror_ = INVALID_HANDLE_VALUE;
    }

    std::string tail(size_t maximum_bytes = 32 * 1024) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string result;
        for (auto line = lines_.rbegin(); line != lines_.rend(); ++line) {
            if (result.size() + line->size() > maximum_bytes && !result.empty()) break;
            result.insert(0, *line);
            if (result.size() >= maximum_bytes) break;
        }
        return result;
    }

    void set_secret(std::string value) {
        std::lock_guard<std::mutex> lock(mutex_);
        secret_ = std::move(value);
    }
};
}

#pragma once
#include <Windows.h>
#include <utility>

namespace vcam {
class unique_handle {
    HANDLE value_ = NULL;
public:
    unique_handle() noexcept = default;
    explicit unique_handle(HANDLE value) noexcept : value_(value) {}
    ~unique_handle() { reset(); }
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&& other) noexcept : value_(other.release()) {}
    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = NULL) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
};
}

#pragma once
#include <Windows.h>
#include <sddl.h>

namespace vcam {
class local_pipe_security {
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
public:
    local_pipe_security() {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor_, nullptr)) {
            attributes_.lpSecurityDescriptor = descriptor_;
        }
    }
    ~local_pipe_security() { if (descriptor_) LocalFree(descriptor_); }
    local_pipe_security(const local_pipe_security&) = delete;
    local_pipe_security& operator=(const local_pipe_security&) = delete;
    explicit operator bool() const { return descriptor_ != nullptr; }
    SECURITY_ATTRIBUTES* get() { return &attributes_; }
};
}

#pragma once
#include <Windows.h>
#include <sddl.h>

namespace vcam {
enum class PipeAccess { owner, local_users, everyone };

class local_pipe_security {
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
public:
    explicit local_pipe_security(PipeAccess scope = PipeAccess::owner) {
        // Pipe creation also uses PIPE_REJECT_REMOTE_CLIENTS; Everyone is local-only here.
        const wchar_t* sddl = scope == PipeAccess::everyone
            ? L"D:P(A;;GA;;;SY)(A;;GA;;;OW)(A;;GA;;;WD)"
            : scope == PipeAccess::local_users
                ? L"D:P(A;;GA;;;SY)(A;;GA;;;OW)(A;;GA;;;BU)"
                : L"D:P(A;;GA;;;SY)(A;;GA;;;OW)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &descriptor_, nullptr)) {
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

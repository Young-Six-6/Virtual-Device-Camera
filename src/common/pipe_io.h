#pragma once
#include "win_handle.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace vcam {
// Every pending operation is completed (or cancelled and completed) before its
// OVERLAPPED structure, event, or buffer can be destroyed.
inline bool finish_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD& bytes) {
    HANDLE events[] = { operation.hEvent, stop };
    const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (result != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        SetLastError(ERROR_OPERATION_ABORTED);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}
inline bool connect_pipe(HANDLE pipe, HANDLE stop) {
    unique_handle event(CreateEvent(NULL, TRUE, FALSE, NULL));
    if (!event) return false;
    OVERLAPPED operation = {};
    operation.hEvent = event.get();
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    DWORD bytes = 0;
    return finish_io(pipe, operation, stop, bytes);
}
inline bool pipe_transfer(HANDLE pipe, HANDLE stop, void* buffer, size_t length, bool write) {
    unique_handle event(CreateEvent(NULL, TRUE, FALSE, NULL));
    if (!event) return false;
    auto* data = static_cast<unsigned char*>(buffer);
    while (length) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
        OVERLAPPED operation = {};
        ResetEvent(event.get());
        operation.hEvent = event.get();
        DWORD bytes = 0;
        const DWORD count = static_cast<DWORD>((std::min)(length, size_t(16 * 1024 * 1024)));
        const BOOL complete = write ? WriteFile(pipe, data, count, &bytes, &operation)
                                    : ReadFile(pipe, data, count, &bytes, &operation);
        if (!complete && (GetLastError() != ERROR_IO_PENDING ||
            !finish_io(pipe, operation, stop, bytes))) return false;
        if (!bytes) return false;
        data += bytes;
        length -= bytes;
    }
    return true;
}
inline bool read_exact(HANDLE pipe, HANDLE stop, void* buffer, size_t length) {
    return pipe_transfer(pipe, stop, buffer, length, false);
}
inline bool write_exact(HANDLE pipe, HANDLE stop, const void* buffer, size_t length) {
    return pipe_transfer(pipe, stop, const_cast<void*>(buffer), length, true);
}
inline bool read_line(HANDLE pipe, HANDLE stop, std::string& line, size_t maximum = 256) {
    line.clear();
    char c = 0;
    while (line.size() < maximum && read_exact(pipe, stop, &c, 1)) {
        if (c == '\n') return true;
        if (c != '\r') line += c;
    }
    return false;
}
}

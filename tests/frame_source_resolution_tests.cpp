#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/media/frame_source.h"

namespace {
const std::string test_pipe = "\\\\.\\pipe\\vcam_resolution_test_" + std::to_string(GetCurrentProcessId());
struct Resolution { int width; int height; };
const Resolution resolutions[] = { {640, 480}, {1280, 720}, {1920, 1080} };
using Source = std::unique_ptr<frame_source, decltype(&frame_source_destroy)>;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Output {
    static constexpr size_t guard_size = 64;
    static constexpr unsigned char sentinel = 0xcd;
    std::vector<unsigned char> bytes;
    frame_t frame = {};

    Output(Resolution size, frame_source* source)
        : bytes((size_t)size.width * size.height * 2 + guard_size * 2, sentinel)
    {
        frame.buffer = reinterpret_cast<char*>(bytes.data() + guard_size);
        frame.length = static_cast<int>(bytes.size() - guard_size * 2);
        frame.width = size.width;
        frame.height = size.height;
        frame.param = source;
    }

    void reset() { std::fill(bytes.begin(), bytes.end(), sentinel); }
    bool untouched() const
    {
        return std::all_of(bytes.begin(), bytes.end(), [](unsigned char c) { return c == sentinel; });
    }
    bool guards_intact() const
    {
        return std::all_of(bytes.begin(), bytes.begin() + guard_size,
            [](unsigned char c) { return c == sentinel; }) &&
            std::all_of(bytes.end() - guard_size, bytes.end(),
                [](unsigned char c) { return c == sentinel; });
    }
    bool is_color(int y, int u, int v) const
    {
        const unsigned char* pixels = bytes.data() + guard_size;
        const size_t count = bytes.size() - guard_size * 2;
        for (size_t offset = 0; offset + 3 < count; offset += 4)
        {
            if (std::abs(static_cast<int>(pixels[offset]) - y) > 8 ||
                std::abs(static_cast<int>(pixels[offset + 1]) - u) > 8 ||
                std::abs(static_cast<int>(pixels[offset + 2]) - y) > 8 ||
                std::abs(static_cast<int>(pixels[offset + 3]) - v) > 8) return false;
        }
        return true;
    }
};

class PipeSender {
    HANDLE done_ = CreateEvent(NULL, TRUE, FALSE, NULL);
    std::thread worker_;

    bool write_all(HANDLE pipe, const void* buffer, size_t length, DWORD chunk_size)
    {
        const unsigned char* data = static_cast<const unsigned char*>(buffer);
        while (length > 0)
        {
            if (WaitForSingleObject(done_, 0) == WAIT_OBJECT_0) return false;
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>((std::min)(length, (size_t)chunk_size));
            if (!WriteFile(pipe, data, chunk, &written, NULL) || written == 0) return false;
            data += written;
            length -= written;
        }
        return true;
    }

public:
    std::atomic<bool> sent{false};
    std::atomic<bool> rejected{false};
    PipeSender(Resolution size, bool send_frame)
    {
        require(done_ != NULL, "CreateEvent failed");
        worker_ = std::thread([this, size, send_frame]() {
            HANDLE pipe = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                if (WaitForSingleObject(done_, 0) == WAIT_OBJECT_0) return;
                pipe = CreateFileA(test_pipe.c_str(), GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
                if (pipe != INVALID_HANDLE_VALUE) break;
                Sleep(10);
            }
            if (pipe == INVALID_HANDLE_VALUE) return;
            const int32_t header[] = { size.width, size.height };
            bool ok = write_all(pipe, header, sizeof(header), 1);
            if (ok && send_frame)
            {
                std::vector<unsigned char> rgb((size_t)size.width * size.height * 3, 0);
                for (size_t i = 1; i < rgb.size(); i += 3) rgb[i] = 255;
                ok = write_all(pipe, rgb.data(), rgb.size(), 65521);
            }
            sent.store(ok);
            if (!send_frame && ok) {
                unsigned char probe = 0;
                for (int attempt = 0; attempt < 100; ++attempt) {
                    DWORD bytes = 0;
                    if (!WriteFile(pipe, &probe, 1, &bytes, NULL)) { rejected.store(true); break; }
                    Sleep(10);
                }
            }
            WaitForSingleObject(done_, 5000);
            CloseHandle(pipe);
        });
    }
    ~PipeSender()
    {
        SetEvent(done_);
        if (worker_.joinable())
        {
            CancelSynchronousIo(worker_.native_handle());
            worker_.join();
        }
        CloseHandle(done_);
    }
};

void check_rejection_boundaries(const std::string& fixture)
{
    Source source(frame_source_create_file(fixture.c_str()), frame_source_destroy);
    require(source != nullptr, "Unable to create file source");
    Output output({640, 480}, source.get());
    --output.frame.length;
    require(frame_source_callback(&output.frame) < 0 && output.untouched(),
        "Short YUY2 buffer was not rejected without writes");
    ++output.frame.length;
    output.frame.width = 641;
    require(frame_source_callback(&output.frame) < 0 && output.untouched(),
        "Odd YUY2 width was not rejected without writes");
    output.frame.width = INT_MAX;
    output.frame.height = INT_MAX;
    require(frame_source_callback(&output.frame) < 0 && output.untouched(),
        "Oversized dimensions were not rejected without writes");
}

void check_output_change(frame_source* source, Resolution original)
{
    const Resolution changed = original.width == 640 ? resolutions[1] : resolutions[0];
    Output output(changed, source);
    const ULONGLONG started = GetTickCount64();
    require(frame_source_callback(&output.frame) == 0 && output.guards_intact() && output.is_color(144,54,34),
        "Hot output change did not resize safely");
    require(GetTickCount64() - started < 1000, "Output change attempted to read another input frame");
}

void check_file(const std::string& fixture, Resolution target)
{
    Source source(frame_source_create_file(fixture.c_str()), frame_source_destroy);
    require(source != nullptr, "Unable to create file source");
    Output output(target, source.get());
    // Each fixture has three frames: five callbacks also exercise EOF/looping.
    for (int i = 0; i < 5; ++i)
    {
        output.reset();
        require(frame_source_callback(&output.frame) == 0, "MP4 callback failed");
        require(output.guards_intact(), "MP4 output wrote outside its YUY2 buffer");
        require(output.frame.width == target.width && output.frame.height == target.height,
            "MP4 input overwrote the UVC output dimensions");
        require(output.is_color(144, 54, 34), "MP4 resize produced incomplete or incorrect pixels");
    }
    check_output_change(source.get(), target);
}

void check_pipe(Resolution input, Resolution target, bool valid_header)
{
    Source source(frame_source_create_pipe(test_pipe.c_str()), frame_source_destroy);
    require(source != nullptr && frame_source_start_pipe_listener(source.get()),
        "Unable to start test pipe server (stop other vcam_pipe users)");
    Output output(target, source.get());
    PipeSender sender(input, valid_header);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    bool completed = false;
    do
    {
        output.reset();
        const int result = frame_source_callback(&output.frame);
        require(output.guards_intact(), "Pipe output wrote outside its YUY2 buffer");
        if (!valid_header && sender.rejected.load())
        {
            require(result == 0 && output.is_color(16,128,128), "Invalid pipe header did not leave a valid black frame");
            completed = true;
            break;
        }
        require(result == 0, "Pipe callback reported failure for a valid frame");
        if (valid_header && output.is_color(144, 54, 34))
        {
            require(output.frame.width == target.width && output.frame.height == target.height,
                "Pipe input overwrote the UVC output dimensions");
            check_output_change(source.get(), target);
            completed = true;
            break;
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    require(completed, "Pipe test did not complete before its deadline");
}

void check_dynamic_file(const std::string& fixture, Resolution target)
{
    Source source(frame_source_create_file(fixture.c_str()), frame_source_destroy);
    Output output(target, source.get());
    const int colors[][3] = { {144, 54, 34}, {82, 90, 240}, {41, 240, 110} };
    for (int frame_index = 0; frame_index < 9; ++frame_index)
    {
        output.reset();
        require(frame_source_callback(&output.frame) == 0, "Dynamic-size decode failed");
        require(output.guards_intact(), "Dynamic-size video overwrote its output buffer");
        const int* color = colors[frame_index / 3];
        require(output.is_color(color[0], color[1], color[2]),
            "Dynamic-size source produced stale, incomplete, or incorrect pixels");
    }
}

void check_pipe_shutdown() {
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int pass = 0; pass < 30; ++pass) {
        Source source(frame_source_create_pipe(test_pipe.c_str()), frame_source_destroy);
        require(source && frame_source_start_pipe_listener(source.get()), "Shutdown-test pipe start failed");
        HANDLE client = INVALID_HANDLE_VALUE;
        if (pass % 3 != 0) {
            client = CreateFileA(test_pipe.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            require(client != INVALID_HANDLE_VALUE, "Shutdown-test client connection failed");
            const int32_t header[] = {1920,1080};
            DWORD written = 0;
            require(WriteFile(client, header, pass % 3 == 1 ? 1 : sizeof(header), &written, NULL) != FALSE, "Partial header write failed");
            if (pass % 3 == 2) require(WriteFile(client, "rgb", 3, &written, NULL) != FALSE, "Partial frame write failed");
        }
        const auto started = GetTickCount64();
        source.reset();
        require(GetTickCount64()-started < 1000, "Pending pipe I/O blocked shutdown");
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    require(after <= before+2, "Pipe reader leaked Windows handles");
}
}

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: resolution-tests 480.mp4 720.mp4 1080.mp4 dynamic.h264\n");
        return 2;
    }
    try
    {
        check_rejection_boundaries(argv[1]);
        for (int input = 0; input < 3; ++input)
            for (Resolution target : resolutions)
            {
                check_file(argv[input + 1], target);
                check_pipe(resolutions[input], target, true);
                printf("PASS: %dx%d -> %dx%d (MP4 + pipe)\n",
                    resolutions[input].width, resolutions[input].height, target.width, target.height);
            }
        for (Resolution target : resolutions) check_dynamic_file(argv[4], target);
        check_pipe({641, 481}, resolutions[1], true);
        check_pipe({0, 480}, resolutions[0], false);
        check_pipe({INT_MAX, INT_MAX}, resolutions[0], false);
        check_pipe_shutdown();
        printf("PASS: 9 MP4 + 9 pipe combinations, odd-size pipe, dynamic-size video, looping, and buffer/header guards.\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

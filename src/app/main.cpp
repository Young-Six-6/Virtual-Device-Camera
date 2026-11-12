#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include "control_server.h"
#include "../camera/camera_session.h"
#include "../media/frame_source.h"

namespace {
vcam::unique_handle application_stop(CreateEvent(NULL, TRUE, FALSE, NULL));
BOOL WINAPI console_signal(DWORD code) {
    if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT || code == CTRL_CLOSE_EVENT ||
        code == CTRL_LOGOFF_EVENT || code == CTRL_SHUTDOWN_EVENT) {
        SetEvent(application_stop.get());
        return TRUE;
    }
    return FALSE;
}
std::string utf8(const wchar_t* input) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, NULL, 0, NULL, NULL);
    if (!count) throw std::runtime_error("Invalid command-line text");
    std::string value(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, value.data(), count, NULL, NULL);
    value.pop_back();
    return value;
}
void executable_directory() {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(NULL, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("Unable to locate executable directory");
    std::filesystem::current_path(std::filesystem::path(path.data()).parent_path());
}
bool parse_resolution(std::string text, vcam::Resolution& size) {
    if (text == "480") text = "640x480";
    if (text == "720") text = "1280x720";
    if (text == "1080") text = "1920x1080";
    std::istringstream input(text);
    char separator = 0;
    std::string extra;
    return (input >> size.width >> separator >> size.height) && separator == 'x' &&
        !(input >> extra) && vcam::valid_output(size);
}
void wait_for_exit() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) { WaitForSingleObject(application_stop.get(), INFINITE); return; }
    HANDLE events[] = {application_stop.get(), input};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0+1) {
        INPUT_RECORD record = {};
        DWORD count = 0;
        if (!ReadConsoleInputW(input, &record, 1, &count)) break;
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
            record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
    }
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        executable_directory();
        if (!application_stop) throw std::runtime_error("Unable to create shutdown event");
        std::string rgb_pipe = "\\\\.\\pipe\\vcam_pipe", control_pipe = "\\\\.\\pipe\\vcam_control";
        std::vector<std::string> files;
        vcam::Resolution size{};
        bool override_size = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = utf8(argv[index]);
            if (argument == "--help" || argument == "-h") {
                printf("Usage: virtual-device-camera [video1 [video2]] [--resolution 480|720|1080|WIDTHxHEIGHT]\n"
                       "       [--pipe PATH] [--control-pipe PATH]\n");
                return 0;
            }
            if (argument == "--pipe" || argument == "--control-pipe" || argument == "--resolution") {
                if (++index == argc) throw std::invalid_argument("Missing option value");
                const auto value = utf8(argv[index]);
                if (argument == "--pipe") rgb_pipe = value;
                else if (argument == "--control-pipe") control_pipe = value;
                else {
                    if (!parse_resolution(value, size)) throw std::invalid_argument("Invalid output resolution");
                    override_size = true;
                }
            }
            else if (argument.rfind("--", 0) == 0) throw std::invalid_argument("Unknown option: " + argument);
            else files.push_back(argument);
        }
        if (files.size() > 2) throw std::invalid_argument("At most two video inputs are supported");
        if (rgb_pipe == control_pipe) throw std::invalid_argument("RGB and control pipes must have different names");
        using Source = std::unique_ptr<frame_source, decltype(&frame_source_destroy)>;
        using Camera = std::unique_ptr<void, decltype(&vcam_destroy)>;
        Source source1(files.empty() ? frame_source_create_pipe(rgb_pipe.c_str()) : frame_source_create_file(files[0].c_str()), frame_source_destroy);
        Source source2(files.size() == 2 ? frame_source_create_file(files[1].c_str()) : nullptr, frame_source_destroy);
        if (!source1 || (files.size() == 2 && !source2)) throw std::runtime_error("Unable to allocate frame source");
        if (files.empty() && !frame_source_start_pipe_listener(source1.get())) throw std::runtime_error("Unable to start RGB pipe");
        auto create = [&](frame_source* source, int number) {
            uvc_vcam_t config{};
            config.pid = number == 1 ? 0xcc10 : 0xcc11;
            config.vid = number == 1 ? 0xbb10 : 0xbb11;
            config.manu_fact = "Fanxiushu";
            config.product = number == 1 ? (files.empty() ? "Pipe HD Camera 1" : "MP4 HD Camera 1") : "MP4 HD Camera 2";
            config.frame_callback = frame_source_callback;
            config.param = source;
            config.request_stop = frame_source_interrupt;
            config.resume = frame_source_resume;
            if (override_size) { config.width = size.width; config.height = size.height; }
            return Camera(vcam_create(&config), vcam_destroy);
        };
        Camera camera1 = create(source1.get(), 1);
        if (!camera1) throw std::runtime_error("Unable to create camera 1; check the virtual USB driver");
        Camera camera2(source2 ? create(source2.get(), 2).release() : nullptr, vcam_destroy);
        if (source2 && !camera2) fprintf(stderr, "Warning: camera 2 could not be created.\n");
        auto actions = [](void* camera) -> vcam::CameraActions {
            if (!camera) return {};
            auto* session = static_cast<vcam::CameraSession*>(camera);
            return {[session](vcam::Resolution value, std::string& error) { return session->resize(value, error); },
                [session] { return session->resolution(); }, [session] { return session->running(); }};
        };
        vcam::ControlRouter router({actions(camera1.get()), actions(camera2.get())});
        vcam::ControlServer server;
        if (!server.start(control_pipe, [&router](const std::string& command) {
            return router.execute(command);
        })) throw std::runtime_error("Unable to start camera control pipe");
        SetConsoleCtrlHandler(console_signal, TRUE);
        printf("Control pipe: %s\nPress Enter or Ctrl+C to stop.\n", control_pipe.c_str());
        wait_for_exit();
        // server is destroyed and joined before cameras; cameras stop and join
        // their workers before the frame sources are released.
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}

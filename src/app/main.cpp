#include <Windows.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>
#include "app_config.h"
#include "control_server.h"
#include "console_log.h"
#include "runtime_controller.h"
#include "tray_controller.h"
#include "web_server.h"
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
    const bool has_console_input = GetConsoleMode(input, &mode) != FALSE;
    HANDLE events[] = {application_stop.get(), input};
    const DWORD handle_count = has_console_input ? 2 : 1;
    for (;;) {
        const DWORD result = MsgWaitForMultipleObjects(handle_count, events, FALSE, INFINITE, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) break;
        if (has_console_input && result == WAIT_OBJECT_0 + 1) {
            INPUT_RECORD record = {};
            DWORD count = 0;
            if (!ReadConsoleInputW(input, &record, 1, &count)) break;
            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
                record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
            continue;
        }
        if (result == WAIT_OBJECT_0 + handle_count) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { SetEvent(application_stop.get()); break; }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            continue;
        }
        break;
    }
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    vcam::ConsoleLogCapture log_capture;
    try {
        if (!log_capture.start()) throw std::runtime_error("Unable to initialize Web UI log capture");
        executable_directory();
        if (!application_stop) throw std::runtime_error("Unable to create shutdown event");
        const std::filesystem::path config_path = std::filesystem::current_path() / "camera.ini";
        auto settings = vcam::load_app_settings(config_path.u8string());
        std::vector<std::string> files;
        bool override_size = false;
        vcam::Resolution size{};
        for (int index = 1; index < argc; ++index) {
            const std::string argument = utf8(argv[index]);
            if (argument == "--help" || argument == "-h") {
                printf("Usage: virtual-device-camera [video1 [video2]] [--resolution 480|720|1080|WIDTHxHEIGHT]\n"
                       "       [--pipe PATH] [--control-pipe PATH]\n"
                       "Settings and the local web UI are configured in camera.ini.\n");
                return 0;
            }
            if (argument == "--pipe" || argument == "--control-pipe" || argument == "--resolution") {
                if (++index == argc) throw std::invalid_argument("Missing option value");
                const auto value = utf8(argv[index]);
                if (argument == "--pipe") settings.rgb_pipe = value;
                else if (argument == "--control-pipe") settings.control_pipe = value;
                else {
                    if (!parse_resolution(value, size)) throw std::invalid_argument("Invalid output resolution");
                    override_size = true;
                }
            }
            else if (argument.rfind("--", 0) == 0) throw std::invalid_argument("Unknown option: " + argument);
            else files.push_back(argument);
        }
        if (files.size() > 2) throw std::invalid_argument("At most two video inputs are supported");
        if (!vcam::valid_pipe_path(settings.rgb_pipe) || !vcam::valid_pipe_path(settings.control_pipe) ||
            settings.rgb_pipe == settings.control_pipe) throw std::invalid_argument("Invalid or duplicate pipe name");
        if (override_size) {
            for (auto& camera : settings.cameras) camera.resolution = size;
        }
        if (!files.empty()) {
            for (auto& enabled : settings.camera_enabled) enabled = false;
            settings.camera_enabled[0] = true;
            if (files.size() == 2) settings.camera_enabled[1] = true;
        }
        for (size_t index = 0; index < vcam::max_virtual_cameras; ++index) {
            if (!settings.camera_enabled[index] || settings.camera_name_configured[index]) continue;
            const std::string number = std::to_string(index + 1);
            settings.cameras[index].name = !files.empty() ? "MP4 HD Camera " + number :
                settings.input_mode == "obs" ? "OBS HD Camera " + number : "Pipe HD Camera " + number;
        }
        using Source = std::unique_ptr<frame_source, decltype(&frame_source_destroy)>;
        Source source1(files.empty() ? frame_source_create_pipe(settings.rgb_pipe.c_str()) : frame_source_create_file(files[0].c_str()), frame_source_destroy);
        Source source2(files.size() == 2 ? frame_source_create_file(files[1].c_str()) : nullptr, frame_source_destroy);
        if (!source1 || (files.size() == 2 && !source2)) throw std::runtime_error("Unable to allocate frame source");
        if (files.empty()) {
            std::string input_error;
            size_t primary_camera = 0;
            for (size_t index = 0; index < vcam::max_virtual_cameras; ++index)
                if (settings.camera_enabled[index]) { primary_camera = index; break; }
            if (!frame_source_set_input(source1.get(), settings.input_mode, settings.obs,
                settings.rgb_pipe.c_str(), settings.rgb_pipe_access,
                settings.cameras[primary_camera].resolution, input_error))
                throw std::runtime_error("Unable to start input source: " + input_error);
            printf("Input source: %s\n", settings.input_mode == "obs" ? "OBS WebSocket" : "RGB named pipe");
        }
        auto create = [](frame_source* source, const vcam::CameraSettings& camera_settings) {
            uvc_vcam_t config{};
            config.pid = camera_settings.pid;
            config.vid = camera_settings.vid;
            config.manu_fact = camera_settings.manufacturer.c_str();
            config.product = camera_settings.name.c_str();
            config.frame_callback = frame_source_callback;
            config.param = source;
            config.request_stop = frame_source_interrupt;
            config.resume = frame_source_resume;
            config.width = camera_settings.resolution.width;
            config.height = camera_settings.resolution.height;
            config.fps = camera_settings.fps;
            config.enable_yuy2 = camera_settings.yuy2;
            config.enable_mjpeg = camera_settings.mjpeg;
            return vcam::CameraHandle(vcam_create_with_mjpeg(&config, frame_source_encode_mjpeg));
        };
        vcam::CameraFactory camera_factory = [&](const vcam::CameraSettings& camera_settings,
                                                  int number, std::string& error) {
            frame_source* source = number == 2 && source2 ? source2.get() : source1.get();
            auto camera = create(source, camera_settings);
            if (!camera) error = "camera-create-failed";
            return camera;
        };
        std::array<vcam::CameraHandle, vcam::max_virtual_cameras> cameras;
        bool camera_settings_changed = false;
        for (size_t index = 0; index < vcam::max_virtual_cameras; ++index) {
            if (!settings.camera_enabled[index]) continue;
            std::string create_error;
            cameras[index] = camera_factory(settings.cameras[index], static_cast<int>(index + 1), create_error);
            if (!cameras[index]) {
                fprintf(stderr, "Warning: unable to create camera %zu (%s).\n", index + 1,
                    create_error.empty() ? "unknown error" : create_error.c_str());
                settings.camera_enabled[index] = false;
                camera_settings_changed = true;
            }
        }
        bool any_camera = false;
        std::vector<vcam::CameraActions> actions(vcam::max_virtual_cameras);
        for (size_t index = 0; index < vcam::max_virtual_cameras; ++index) {
            if (!cameras[index]) continue;
            any_camera = true;
            auto* session = static_cast<vcam::CameraSession*>(cameras[index].get());
            actions[index] = {
                [session](vcam::Resolution value, std::string& error) { return session->resize(value, error); },
                [session] { return session->resolution(); }, [session] { return session->running(); }
            };
        }
        if (!any_camera) throw std::runtime_error("Unable to create any camera; check the virtual USB driver");
        if (camera_settings_changed) {
            std::string save_error;
            if (!vcam::save_app_settings(config_path.u8string(), settings, save_error))
                fprintf(stderr, "Warning: could not persist unavailable cameras: %s\n", save_error.c_str());
        }
        vcam::ControlRouter router(std::move(actions));
        vcam::ControlServer server;
        if (!server.start(settings.control_pipe, [&router](const std::string& command) {
            return router.execute(command);
        }, settings.control_pipe_access)) throw std::runtime_error("Unable to start camera control pipe");
        vcam::TrayController tray(application_stop.get());
        if (!tray.initialize()) throw std::runtime_error("Unable to initialize the system tray controller");
        vcam::RuntimeController runtime(settings, config_path.u8string(), source1.get(), std::move(cameras),
            std::move(camera_factory), router, server, files.empty());
        vcam::WebServer web;
        const std::string token = vcam::create_web_token();
        if (!web.start(static_cast<unsigned short>(settings.web_port), token,
            [&runtime, &web] { return runtime.status_json(web.port()); },
            [&runtime](int camera) { return runtime.frame(camera); },
            [&runtime](const vcam::WebUpdate& update, std::string& error) { return runtime.apply(update, error); },
            [&runtime] { return runtime.theme(); },
            [&log_capture] { return log_capture.tail(); },
            [&tray] { return tray.hidden(); },
            [&tray](bool hidden) { return tray.set_hidden(hidden); },
            settings.web_allow_network,
            [&runtime](bool enabled, std::string& error) {
                return runtime.set_web_network_access(enabled, error);
            }))
            throw std::runtime_error("Unable to start local Web UI; check camera.ini web_port");
        SetConsoleCtrlHandler(console_signal, TRUE);
        log_capture.set_secret(token);
        printf("Control pipe: %s\n", settings.control_pipe.c_str());
        printf("Web UI: http://127.0.0.1:%u/#%s\n", settings.web_port, token.c_str());
        if (settings.web_allow_network)
            printf("WARNING: Web UI network access is enabled; use this PC's IPv4 address and the same port/token.\n");
        printf("Press Enter or Ctrl+C to stop.\n");
        wait_for_exit();
        // server is destroyed and joined before cameras; cameras stop and join
        // their workers before the frame sources are released.
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}

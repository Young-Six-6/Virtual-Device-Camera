#include "app_config.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace vcam {
namespace {
Resolution default_resolution(const std::filesystem::path& config_path) {
    std::ifstream input(config_path.parent_path() / "default_res.txt");
    std::string preset;
    if (input >> preset) {
        if (preset == "480") return {640, 480};
        if (preset == "720") return {1280, 720};
    }
    return {1920, 1080};
}
int integer_value(const std::map<std::string, std::string>& values, const char* key, int fallback) {
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    size_t consumed = 0;
    const int value = std::stoi(found->second, &consumed);
    if (consumed != found->second.size()) throw std::runtime_error(std::string("Invalid config value: ") + key);
    return value;
}
void resolve_camera(Resolution& value, Resolution fallback) {
    if (value.width == 0 && value.height == 0) value = fallback;
    if (!valid_output(value)) throw std::runtime_error("Invalid camera resolution in camera.ini");
}
const char* access_value(PipeAccess access) {
    switch (access) {
    case PipeAccess::owner: return "owner";
    case PipeAccess::local_users: return "users";
    case PipeAccess::everyone: return "everyone";
    }
    return "owner";
}
}

bool valid_pipe_path(const std::string& path) {
    constexpr char prefix[] = "\\\\.\\pipe\\";
    if (path.rfind(prefix, 0) != 0 || path.size() <= sizeof(prefix) - 1 || path.size() > 240) return false;
    for (unsigned char character : path) if (character < 0x21 || character > 0x7e) return false;
    return true;
}

AppSettings load_app_settings(const std::string& config_name) {
    const std::filesystem::path config_path = std::filesystem::u8path(config_name);
    AppSettings settings;
    const Resolution fallback = default_resolution(config_path);
    std::ifstream input(config_path);
    if (input) {
        std::map<std::string, std::string> values;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            const size_t separator = line.find('=');
            if (separator == std::string::npos) throw std::runtime_error("Invalid line in camera.ini");
            values[line.substr(0, separator)] = line.substr(separator + 1);
        }
        if (values.count("rgb_pipe")) settings.rgb_pipe = values.at("rgb_pipe");
        if (values.count("control_pipe")) settings.control_pipe = values.at("control_pipe");
        auto parse_access = [&values](const char* key, PipeAccess& access) {
            if (!values.count(key)) return;
            const auto& value = values.at(key);
            if (value == "owner") access = PipeAccess::owner;
            else if (value == "users") access = PipeAccess::local_users;
            else if (value == "everyone") access = PipeAccess::everyone;
            else throw std::runtime_error(std::string(key) + " must be owner, users, or everyone");
        };
        // Older builds used one scope for both pipes; preserve it when migrating.
        parse_access("pipe_access", settings.rgb_pipe_access);
        if (values.count("pipe_access")) settings.control_pipe_access = settings.rgb_pipe_access;
        parse_access("rgb_pipe_access", settings.rgb_pipe_access);
        parse_access("control_pipe_access", settings.control_pipe_access);
        settings.web_port = integer_value(values, "web_port", settings.web_port);
        settings.cameras[0] = {integer_value(values, "camera1_width", 0), integer_value(values, "camera1_height", 0)};
        settings.cameras[1] = {integer_value(values, "camera2_width", 0), integer_value(values, "camera2_height", 0)};
    }
    resolve_camera(settings.cameras[0], fallback);
    resolve_camera(settings.cameras[1], fallback);
    if (!valid_pipe_path(settings.rgb_pipe) || !valid_pipe_path(settings.control_pipe) ||
        settings.rgb_pipe == settings.control_pipe)
        throw std::runtime_error("Pipe names in camera.ini must be distinct local named-pipe paths");
    if (settings.web_port < 1024 || settings.web_port > 65535)
        throw std::runtime_error("web_port must be in the range 1024..65535");
    return settings;
}

bool save_app_settings(const std::string& config_name, const AppSettings& settings, std::string& error) {
    if (!valid_pipe_path(settings.rgb_pipe) || !valid_pipe_path(settings.control_pipe) ||
        settings.rgb_pipe == settings.control_pipe || !valid_output(settings.cameras[0]) ||
        !valid_output(settings.cameras[1]) || settings.web_port < 1024 || settings.web_port > 65535) {
        error = "invalid-settings";
        return false;
    }
    const auto path = std::filesystem::u8path(config_name);
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) { error = "config-write-failed"; return false; }
        output << "# Virtual Device Camera runtime settings\n"
               << "rgb_pipe=" << settings.rgb_pipe << "\n"
               << "control_pipe=" << settings.control_pipe << "\n"
               << "rgb_pipe_access=" << access_value(settings.rgb_pipe_access) << "\n"
               << "control_pipe_access=" << access_value(settings.control_pipe_access) << "\n"
               << "web_port=" << settings.web_port << "\n"
               << "camera1_width=" << settings.cameras[0].width << "\n"
               << "camera1_height=" << settings.cameras[0].height << "\n"
               << "camera2_width=" << settings.cameras[1].width << "\n"
               << "camera2_height=" << settings.cameras[1].height << "\n";
        output.flush();
        if (!output) { error = "config-write-failed"; return false; }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "config-commit-failed";
        return false;
    }
    return true;
}
}

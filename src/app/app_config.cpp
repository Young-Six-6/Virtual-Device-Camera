#include "app_config.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <charconv>

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
int hex_value(const std::map<std::string, std::string>& values, const std::string& key, int fallback) {
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    std::string value = found->second;
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) value.erase(0, 2);
    unsigned int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > 0xffff)
        throw std::runtime_error("Invalid hexadecimal config value: " + key);
    return static_cast<int>(parsed);
}
bool bool_value(const std::map<std::string, std::string>& values, const std::string& key, bool fallback) {
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    if (found->second == "true" || found->second == "1") return true;
    if (found->second == "false" || found->second == "0") return false;
    throw std::runtime_error("Invalid boolean config value: " + key);
}
std::string hex_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}
std::string hex_decode(const std::map<std::string, std::string>& values, const char* key) {
    const auto found = values.find(key);
    if (found == values.end()) return {};
    const auto& encoded = found->second;
    if (encoded.size() % 2 || encoded.size() > 131072)
        throw std::runtime_error(std::string("Invalid encoded config value: ") + key);
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (size_t i = 0; i < encoded.size(); i += 2) {
        const int high = nibble(encoded[i]), low = nibble(encoded[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0))
            throw std::runtime_error(std::string("Invalid encoded config value: ") + key);
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}
bool valid_utf8_text(const std::string& value, size_t maximum_bytes, bool allow_empty = false,
                     bool allow_line_breaks = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum_bytes || value.find('\0') != std::string::npos ||
        (!allow_line_breaks && (value.find('\r') != std::string::npos || value.find('\n') != std::string::npos)))
        return false;
    if (value.empty()) return true;
    const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    return characters > 0 && (allow_empty || characters <= 126);
}
bool valid_camera(const CameraSettings& camera) {
    return valid_output(camera.resolution) && camera.fps >= 1 && camera.fps <= 60 &&
        camera.vid != 0 && camera.pid != 0 && valid_utf8_text(camera.manufacturer, 504) &&
        valid_utf8_text(camera.name, 504) && (camera.yuy2 || camera.mjpeg);
}
bool valid_theme(const std::string& source) {
    return valid_utf8_text(source, 65536, true, true);
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
        for (int i = 0; i < 2; ++i) {
            CameraSettings& camera = settings.cameras[i];
            const std::string prefix = "camera" + std::to_string(i + 1) + "_";
            camera.resolution = {integer_value(values, (prefix + "width").c_str(), 0),
                integer_value(values, (prefix + "height").c_str(), 0)};
            camera.fps = integer_value(values, (prefix + "fps").c_str(), camera.fps);
            camera.vid = static_cast<uint16_t>(hex_value(values, prefix + "vid", camera.vid));
            camera.pid = static_cast<uint16_t>(hex_value(values, prefix + "pid", camera.pid));
            if (values.count(prefix + "manufacturer")) camera.manufacturer = values.at(prefix + "manufacturer");
            if (values.count(prefix + "name")) camera.name = values.at(prefix + "name");
            settings.camera_name_configured[i] = bool_value(values, prefix + "name_custom",
                values.count(prefix + "name") != 0);
            camera.yuy2 = bool_value(values, prefix + "yuy2", camera.yuy2);
            camera.mjpeg = bool_value(values, prefix + "mjpeg", camera.mjpeg);
        }
        settings.custom_css = hex_decode(values, "custom_css_hex");
        settings.custom_js = hex_decode(values, "custom_js_hex");
    }
    resolve_camera(settings.cameras[0].resolution, fallback);
    resolve_camera(settings.cameras[1].resolution, fallback);
    if (!valid_camera(settings.cameras[0]) || !valid_camera(settings.cameras[1]) ||
        !valid_theme(settings.custom_css) || !valid_theme(settings.custom_js))
        throw std::runtime_error("Invalid camera or theme settings in camera.ini");
    if (!valid_pipe_path(settings.rgb_pipe) || !valid_pipe_path(settings.control_pipe) ||
        settings.rgb_pipe == settings.control_pipe)
        throw std::runtime_error("Pipe names in camera.ini must be distinct local named-pipe paths");
    if (settings.web_port < 1024 || settings.web_port > 65535)
        throw std::runtime_error("web_port must be in the range 1024..65535");
    return settings;
}

bool save_app_settings(const std::string& config_name, const AppSettings& settings, std::string& error) {
    if (!valid_pipe_path(settings.rgb_pipe) || !valid_pipe_path(settings.control_pipe) ||
        settings.rgb_pipe == settings.control_pipe || !valid_camera(settings.cameras[0]) ||
        !valid_camera(settings.cameras[1]) || settings.web_port < 1024 || settings.web_port > 65535 ||
        !valid_theme(settings.custom_css) || !valid_theme(settings.custom_js)) {
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
               << "camera1_width=" << settings.cameras[0].resolution.width << "\n"
               << "camera1_height=" << settings.cameras[0].resolution.height << "\n"
               << "camera1_fps=" << settings.cameras[0].fps << "\n"
               << "camera1_vid=" << std::hex << settings.cameras[0].vid << std::dec << "\n"
               << "camera1_pid=" << std::hex << settings.cameras[0].pid << std::dec << "\n"
               << "camera1_manufacturer=" << settings.cameras[0].manufacturer << "\n"
               << "camera1_name=" << settings.cameras[0].name << "\n"
               << "camera1_name_custom=" << (settings.camera_name_configured[0] ? "true" : "false") << "\n"
               << "camera1_yuy2=" << (settings.cameras[0].yuy2 ? "true" : "false") << "\n"
               << "camera1_mjpeg=" << (settings.cameras[0].mjpeg ? "true" : "false") << "\n"
               << "camera2_width=" << settings.cameras[1].resolution.width << "\n"
               << "camera2_height=" << settings.cameras[1].resolution.height << "\n"
               << "camera2_fps=" << settings.cameras[1].fps << "\n"
               << "camera2_vid=" << std::hex << settings.cameras[1].vid << std::dec << "\n"
               << "camera2_pid=" << std::hex << settings.cameras[1].pid << std::dec << "\n"
               << "camera2_manufacturer=" << settings.cameras[1].manufacturer << "\n"
               << "camera2_name=" << settings.cameras[1].name << "\n"
               << "camera2_name_custom=" << (settings.camera_name_configured[1] ? "true" : "false") << "\n"
               << "camera2_yuy2=" << (settings.cameras[1].yuy2 ? "true" : "false") << "\n"
               << "camera2_mjpeg=" << (settings.cameras[1].mjpeg ? "true" : "false") << "\n"
               << "custom_css_hex=" << hex_encode(settings.custom_css) << "\n"
               << "custom_js_hex=" << hex_encode(settings.custom_js) << "\n";
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

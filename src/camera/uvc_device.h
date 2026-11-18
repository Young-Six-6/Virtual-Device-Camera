#pragma once
#include <Windows.h>
#include <cstdint>
#include <array>
#include <vector>
#include "uvc_vcam.h"
#include "../common/resolution.h"
#include "../usb/virt_dev.h"
#include "../protocol/uvc_struct.h"

namespace vcam {
#pragma pack(push, 1)
struct StreamControl {
    uint16_t hint = 1;
    uint8_t format = 1, frame = 1;
    uint32_t interval = 333333;
    uint16_t key_rate = 0, p_rate = 0, quality = 0, window = 0, delay = 10;
    uint32_t frame_bytes = 0, payload_bytes = 3u * 1024u;
};
#pragma pack(pop)
static_assert(sizeof(StreamControl) == 26, "UVC 1.0 probe/commit layout");

// Single-threaded protocol engine. CameraSession owns synchronization and I/O.
class UvcDevice {
    int curr_config = 0;
    int interface_alt_settings_[2] = {0, 0};
    int iso_ep_addr = 0x82, bitsPerPixel = 16, frame_count = 1;
    bool super_speed_ = false;
    uint32_t iso_payload_bytes_ = 3u * 1024u;
    uint64_t iso_video_bytes_per_second_ = uint64_t(3u * 1024u - 2u) * 8000u;
    GUID guidFormat{0x32595559, 0x0000, 0x0010, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    struct { int width; int height; } frames[1]{};
    usb_device_descriptor dev_desc{};
    char str_buffer[1024]{};
    int str_count = 0;
    usb_string_descriptor* str_desc[10]{};
    char cfg_buffer[16*1024]{};
    usb_config_descriptor* cfg_desc = nullptr;
    std::vector<char> pixels_;
    std::vector<char> mjpeg_;
    size_t mjpeg_length_ = 0;
    size_t mjpeg_max_frame_bytes_ = 0;
    size_t position_ = 0;
    uint8_t flip_ = 0;
    uint32_t frame_intervals_[2] = {333333, 666667};
    uint32_t mjpeg_frame_intervals_[2] = {333333, 666667};
    bool enable_yuy2_ = true;
    bool enable_mjpeg_ = false;
    uint8_t yuy2_format_index_ = 1;
    uint8_t mjpeg_format_index_ = 2;
    int requested_fps_ = 30;
    std::array<ULONGLONG, 16> completed_frame_times_{};
    size_t completed_frame_cursor_ = 0;
    size_t completed_frame_count_ = 0;
    double actual_fps_ = 0;
    int delay_ = 33;
    ULONGLONG next_frame_due_ = 0;
    StreamControl stream_;
    FRAME_CALLBACK callback_;
    MJPEG_FRAME_CALLBACK mjpeg_callback_ = nullptr;
    void* parameter_;
    void fill_dev_desc(unsigned short pid, unsigned int vid);
    void fill_str_desc(const char* manufacturer, const char* product);
    void fill_cfg_desc();
    void descriptor(usbtx_header_t& request);
    void streaming(usbtx_header_t& request);
    void control(usbtx_header_t& request);
    void iso(usbtx_header_t& request);
    int encode(unsigned char* destination, size_t capacity);
public:
    UvcDevice(const uvc_vcam_t& config, Resolution size, MJPEG_FRAME_CALLBACK mjpeg_callback = nullptr,
              bool super_speed = false);
    bool set_resolution(Resolution size);
    Resolution resolution() const { return {frames[0].width, frames[0].height}; }
    std::vector<char> frame_copy() const { return pixels_; }
    double actual_fps() const {
        if (completed_frame_count_ < 2) return 0;
        const size_t last = (completed_frame_cursor_ + completed_frame_times_.size() - 1) %
            completed_frame_times_.size();
        return GetTickCount64() - completed_frame_times_[last] > 2000 ? 0 : actual_fps_;
    }
    void handle(usbtx_header_t& request, size_t payload_capacity);
};
}

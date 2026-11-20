#pragma once

#include "../camera/uvc_vcam.h"
#include "../common/obs_settings.h"
#include "../common/resolution.h"
#include "pipe_reader.h"
#include <string>

struct frame_source;
struct FrameSourceStatus {
    std::string mode;
    std::string state;
    std::string error;
    bool connected = false;
    bool listening = false;
    uint64_t frames = 0;
    uint64_t last_frame_ms = 0;
    vcam::Resolution frame_size{0, 0};
};

frame_source* frame_source_create_file(const char* path);
frame_source* frame_source_create_pipe(const char* pipe_path = nullptr);
bool frame_source_start_pipe_listener(frame_source* source, vcam::PipeAccess access = vcam::PipeAccess::owner);
bool frame_source_reconfigure_pipe(frame_source* source, const char* path, vcam::PipeAccess access);
bool frame_source_set_input(frame_source* source, const std::string& mode, const vcam::ObsSettings& obs,
                            const char* pipe_path, vcam::PipeAccess access, vcam::Resolution target,
                            std::string& error);
void frame_source_set_target(frame_source* source, vcam::Resolution target);
FrameSourceStatus frame_source_input_status(frame_source* source);
vcam::PipeReaderStatus frame_source_pipe_status(frame_source* source);
int frame_source_callback(frame_t* frame);
int frame_source_encode_mjpeg(mjpeg_frame_t* frame);
void frame_source_destroy(frame_source* source);
void frame_source_interrupt(void* source);
void frame_source_resume(void* source);

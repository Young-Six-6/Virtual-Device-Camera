#pragma once

#include "../camera/uvc_vcam.h"
#include "pipe_reader.h"

struct frame_source;

frame_source* frame_source_create_file(const char* path);
frame_source* frame_source_create_pipe(const char* pipe_path = nullptr);
bool frame_source_start_pipe_listener(frame_source* source, vcam::PipeAccess access = vcam::PipeAccess::owner);
bool frame_source_reconfigure_pipe(frame_source* source, const char* path, vcam::PipeAccess access);
vcam::PipeReaderStatus frame_source_pipe_status(frame_source* source);
int frame_source_callback(frame_t* frame);
void frame_source_destroy(frame_source* source);
void frame_source_interrupt(void* source);
void frame_source_resume(void* source);

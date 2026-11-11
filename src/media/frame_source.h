#pragma once

#include "../camera/uvc_vcam.h"

struct frame_source;

frame_source* frame_source_create_file(const char* path);
frame_source* frame_source_create_pipe(void);
bool frame_source_start_pipe_listener(frame_source* source);
int frame_source_callback(frame_t* frame);
void frame_source_destroy(frame_source* source);

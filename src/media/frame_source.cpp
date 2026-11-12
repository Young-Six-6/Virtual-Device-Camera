#include "frame_source.h"
#include "pipe_reader.h"
#include "../common/resolution.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace {
struct FormatDeleter { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct ScaleDeleter { void operator()(SwsContext* p) const { sws_freeContext(p); } };
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

void rgb_to_yuy2(const uint8_t* input, int stride, frame_t& output) {
    auto clip = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    auto* destination = reinterpret_cast<uint8_t*>(output.buffer);
    for (int row = 0; row < output.height; ++row) {
        const uint8_t* source = input + (size_t)row * stride;
        for (int x = 0; x < output.width; x += 2, source += 6, destination += 4) {
            const int r1 = source[0], g1 = source[1], b1 = source[2];
            const int r2 = source[3], g2 = source[4], b2 = source[5];
            destination[0] = clip(((66*r1 + 129*g1 + 25*b1 + 128) >> 8) + 16);
            destination[1] = clip((((-38*r1 - 74*g1 + 112*b1 + 128) >> 8) +
                                   ((-38*r2 - 74*g2 + 112*b2 + 128) >> 8)) / 2 + 128);
            destination[2] = clip(((66*r2 + 129*g2 + 25*b2 + 128) >> 8) + 16);
            destination[3] = clip((((112*r1 - 94*g1 - 18*b1 + 128) >> 8) +
                                   ((112*r2 - 94*g2 - 18*b2 + 128) >> 8)) / 2 + 128);
        }
    }
}
}

struct frame_source {
    bool pipe_mode = false;
    std::string path;
    std::string pipe_path = "\\\\.\\pipe\\vcam_pipe";
    vcam::PipeReader receiver;
    std::mutex render_mutex;
    std::unique_ptr<AVFormatContext, FormatDeleter> format;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec;
    Frame decoded{av_frame_alloc()};
    Frame rgb{av_frame_alloc()};
    std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
    std::unique_ptr<SwsContext, ScaleDeleter> scaler;
    int stream = -1;
    bool draining = false;
    bool pending = false;
    std::atomic<bool> cancelled{false};

    bool open_video() {
        if (format) return true;
        static std::once_flag network;
        std::call_once(network, [] { avformat_network_init(); });
        AVFormatContext* raw = avformat_alloc_context();
        if (!raw) return false;
        raw->interrupt_callback = {[](void* context) { return static_cast<frame_source*>(context)->cancelled.load() ? 1 : 0; }, this};
        if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) {
            if (raw) avformat_close_input(&raw);
            return false;
        }
        std::unique_ptr<AVFormatContext, FormatDeleter> opened(raw);
        if (avformat_find_stream_info(opened.get(), nullptr) < 0) return false;
        const AVCodec* decoder = nullptr;
        const int index = av_find_best_stream(opened.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (index < 0 || !decoder) return false;
        std::unique_ptr<AVCodecContext, CodecDeleter> context(avcodec_alloc_context3(decoder));
        if (!context || avcodec_parameters_to_context(context.get(), opened->streams[index]->codecpar) < 0 ||
            avcodec_open2(context.get(), decoder, nullptr) < 0) return false;
        format = std::move(opened);
        codec = std::move(context);
        stream = index;
        return true;
    }
    bool next_video_frame() {
        if (!open_video()) return false;
        for (;;) {
            if (cancelled.load()) return false;
            const int received = avcodec_receive_frame(codec.get(), decoded.get());
            if (received == 0) return true;
            if (received == AVERROR_EOF) {
                if (av_seek_frame(format.get(), stream, 0, AVSEEK_FLAG_BACKWARD) < 0) return false;
                avcodec_flush_buffers(codec.get());
                av_packet_unref(packet.get());
                draining = pending = false;
                continue;
            }
            if (received != AVERROR(EAGAIN)) return false;
            if (pending) {
                const int sent = avcodec_send_packet(codec.get(), packet.get());
                if (sent == AVERROR(EAGAIN)) continue;
                av_packet_unref(packet.get());
                pending = false;
                if (sent < 0) return false;
                continue;
            }
            if (draining) return false;
            if (av_read_frame(format.get(), packet.get()) < 0) {
                const int sent = avcodec_send_packet(codec.get(), nullptr);
                if (sent == AVERROR(EAGAIN)) continue;
                if (sent < 0 && sent != AVERROR_EOF) return false;
                draining = true;
            }
            else if (packet->stream_index == stream) pending = true;
            else av_packet_unref(packet.get());
        }
    }
    bool convert(const uint8_t* const input[], const int strides[], vcam::Resolution input_size,
                 AVPixelFormat pixel_format, frame_t& output) {
        if (rgb->width != output.width || rgb->height != output.height) {
            av_frame_unref(rgb.get());
            rgb->format = AV_PIX_FMT_RGB24;
            rgb->width = output.width;
            rgb->height = output.height;
            if (av_frame_get_buffer(rgb.get(), 32) < 0) { av_frame_unref(rgb.get()); return false; }
        }
        scaler.reset(sws_getCachedContext(scaler.release(), input_size.width, input_size.height,
            pixel_format, output.width, output.height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!scaler || sws_scale(scaler.get(), input, strides, 0, input_size.height,
            rgb->data, rgb->linesize) != output.height) return false;
        rgb_to_yuy2(rgb->data[0], rgb->linesize[0], output);
        return true;
    }
};

frame_source* frame_source_create_file(const char* path) {
    if (!path || !*path) return nullptr;
    try {
        auto source = std::make_unique<frame_source>();
        if (!source->decoded || !source->rgb || !source->packet) return nullptr;
        source->path = path;
        return source.release();
    }
    catch (...) { return nullptr; }
}
frame_source* frame_source_create_pipe(const char* pipe_path) {
    try {
        auto source = std::make_unique<frame_source>();
        if (!source->decoded || !source->rgb || !source->packet) return nullptr;
        source->pipe_mode = true;
        if (pipe_path && *pipe_path) source->pipe_path = pipe_path;
        return source.release();
    }
    catch (...) { return nullptr; }
}
bool frame_source_start_pipe_listener(frame_source* source, vcam::PipeAccess access) {
    return source && source->pipe_mode && source->receiver.start(source->pipe_path, access);
}
bool frame_source_reconfigure_pipe(frame_source* source, const char* path, vcam::PipeAccess access) {
    if (!source || !source->pipe_mode || !path || !*path) return false;
    if (!source->receiver.reconfigure(path, access)) return false;
    source->pipe_path = path;
    return true;
}
vcam::PipeReaderStatus frame_source_pipe_status(frame_source* source) {
    return source && source->pipe_mode ? source->receiver.status() : vcam::PipeReaderStatus{};
}
void frame_source_destroy(frame_source* source) { delete source; }
void frame_source_interrupt(void* pointer) {
    if (auto* source = static_cast<frame_source*>(pointer)) source->cancelled.store(true);
}
void frame_source_resume(void* pointer) {
    if (auto* source = static_cast<frame_source*>(pointer)) {
        std::lock_guard<std::mutex> lock(source->render_mutex);
        if (source->format && source->format->pb && source->format->pb->error == AVERROR_EXIT) {
            source->format->pb->error = 0;
            source->format->pb->eof_reached = 0;
        }
        source->cancelled.store(false);
    }
}

int frame_source_callback(frame_t* output) {
    if (!output || !output->buffer || !output->param ||
        !vcam::valid_output({output->width, output->height}) || output->length < 0 ||
        (size_t)output->length < vcam::yuy2_bytes({output->width, output->height})) return -1;
    auto& source = *static_cast<frame_source*>(output->param);
    std::lock_guard<std::mutex> lock(source.render_mutex);
    try {
        if (source.pipe_mode) {
            auto frame = source.receiver.latest();
            output->delay_msec = 33;
            if (!frame) {
                auto* pixels = reinterpret_cast<uint8_t*>(output->buffer);
                for (size_t offset = 0; offset < vcam::yuy2_bytes({output->width, output->height}); offset += 4) {
                    pixels[offset] = pixels[offset+2] = 16;
                    pixels[offset+1] = pixels[offset+3] = 128;
                }
                return 0;
            }
            const uint8_t* data[] = {frame->pixels.data(), nullptr, nullptr, nullptr};
            const int stride[] = {frame->size.width * 3, 0, 0, 0};
            return source.convert(data, stride, frame->size, AV_PIX_FMT_RGB24, *output) ? 0 : -1;
        }
        if (!source.next_video_frame()) return -1;
        auto* frame = source.decoded.get();
        if (frame->width <= 0 || frame->height <= 0 || frame->format < 0) return -1;
        if (!source.convert(frame->data, frame->linesize, {frame->width, frame->height},
            static_cast<AVPixelFormat>(frame->format), *output)) return -1;
        AVRational rate = source.format->streams[source.stream]->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = source.format->streams[source.stream]->r_frame_rate;
        const double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30.0;
        output->delay_msec = static_cast<int>(1000.0 / (fps >= 1 && fps <= 120 ? fps : 30));
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "Frame source: %s\n", error.what()); return -1; }
}

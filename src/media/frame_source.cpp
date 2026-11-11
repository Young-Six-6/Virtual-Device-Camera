//////Fanxiushu 2016-10-06
//////Young-Six-6
//////version 1.4.0

#include <Windows.h>
#include <atomic>
#include <new>
#include <string>
#include "frame_source.h"
#include <stdio.h>
#include <synchapi.h>
#include <string.h>
#include "../camera/uvc_vcam.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>  // 必须包含这个头文件！
}
//// RGB -> YUV 从网络查询的算法
//// RGB -> YUV 修复颜色颠倒（交换 R 和 B 通道
void rgb24_yuy2(void* rgb, void* yuy2, int width, int height, int row_bytes)
{
    // 新增：检查输入参数合法性
    if (width <= 0 || height <= 0 || row_bytes < width * 3) {
        printf("【错误】无效参数：width=%d, height=%d, row_bytes=%d\n", width, height, row_bytes);
        return;
    }

    int R1, G1, B1, R2, G2, B2, Y1, U1, Y2, V1;
    unsigned char* pRGBData = (unsigned char*)rgb;
    unsigned char* pYUVData = (unsigned char*)yuy2;

    for (int i = 0; i < height; ++i)
    {
        unsigned char* row_start = pRGBData + i * row_bytes;
        // 新增：计算当前行最大可访问的字节数（防止j循环越界）
        int max_j = width / 2;
        if (width % 2 != 0) {
            max_j = (width - 1) / 2; // 若宽度为奇数，丢弃最后一个像素
            printf("【警告】宽度为奇数（%d），最后一个像素将被忽略\n", width);
        }
        for (int j = 0; j < max_j; ++j)
        {
            // 新增：检查当前像素偏移是否超过行字节数
            if (j * 6 + 5 >= row_bytes) {
                printf("【错误】行内像素越界：j=%d, 偏移=%d, 行字节数=%d\n", j, j * 6 + 5, row_bytes);
                break;
            }

            R1 = *(row_start + j * 6);
            G1 = *(row_start + j * 6 + 1);
            B1 = *(row_start + j * 6 + 2);
            R2 = *(row_start + j * 6 + 3);
            G2 = *(row_start + j * 6 + 4);
            B2 = *(row_start + j * 6 + 5);

            // YUV计算（不变）
            Y1 = ((66 * R1 + 129 * G1 + 25 * B1 + 128) >> 8) + 16;
            U1 = (((-38 * R1 - 74 * G1 + 112 * B1 + 128) >> 8) +
                ((-38 * R2 - 74 * G2 + 112 * B2 + 128) >> 8)) / 2 + 128;
            Y2 = ((66 * R2 + 129 * G2 + 25 * B2 + 128) >> 8) + 16;
            V1 = (((112 * R1 - 94 * G1 - 18 * B1 + 128) >> 8) +
                ((112 * R2 - 94 * G2 - 18 * B2 + 128) >> 8)) / 2 + 128;

            // 边界处理（不变）
            *(pYUVData + i * width * 2 + j * 4) = (unsigned char)max(min(Y1, 255), 0);
            *(pYUVData + i * width * 2 + j * 4 + 1) = (unsigned char)max(min(U1, 255), 0);
            *(pYUVData + i * width * 2 + j * 4 + 2) = (unsigned char)max(min(Y2, 255), 0);
            *(pYUVData + i * width * 2 + j * 4 + 3) = (unsigned char)max(min(V1, 255), 0);
        }
    }
}

struct frame_source
{
    // 原有成员...
    HBITMAP hbmp = NULL;               // 初始化句柄为NULL
    HDC hdc = NULL;                    // 初始化DC为NULL
    void* rgb_data = NULL;             // 初始化数据指针为NULL
    int width = 0;                     // 初始化宽度为0
    int height = 0;                    // 初始化高度为0
    std::string video_path = "";       // 初始化路径为空
    AVFormatContext* fmt_ctx = NULL;   // 初始化FFmpeg上下文为NULL
    AVCodecContext* codec_ctx = NULL;
    AVFrame* yuv_frame = NULL;
    AVFrame* rgb_frame = NULL;
    uint8_t* rgb_buffer = NULL;
    SwsContext* sws_ctx = NULL;
    SwsContext* pipe_sws_ctx = NULL;
    AVPacket* pkt = NULL;
    int video_stream_idx = -1;         // 初始化为-1（无流）
    bool is_loop = false;              // 默认为不循环
    bool is_inited = false;            // 默认为未初始化
    bool decoder_draining = false;
    bool packet_pending = false;
    // 管道相关成员
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    bool is_pipe_mode = false;
    std::atomic<bool> pipe_connected{ false };
    std::atomic<bool> shutting_down{ false };
    char* pipe_buffer = NULL;
    int pipe_buf_size = 0;
    int pipe_width = 0;
    int pipe_height = 0;
    //
    HANDLE hPipeMutex = NULL; // 管道数据互斥锁
    HANDLE pipe_connect_thread = NULL;

};

static void release_video_resources(frame_source* p)
{
    if (p->sws_ctx) sws_freeContext(p->sws_ctx);
    p->sws_ctx = NULL;
    if (p->pipe_sws_ctx) sws_freeContext(p->pipe_sws_ctx);
    p->pipe_sws_ctx = NULL;
    av_freep(&p->rgb_buffer);
    av_frame_free(&p->rgb_frame);
    av_frame_free(&p->yuv_frame);
    av_packet_free(&p->pkt);
    avcodec_free_context(&p->codec_ctx);
    avformat_close_input(&p->fmt_ctx);
    p->is_inited = false;
}

static bool read_pipe_exact(HANDLE pipe, void* buffer, DWORD length)
{
    BYTE* destination = (BYTE*)buffer;
    DWORD remaining = length;
    while (remaining > 0)
    {
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, destination, remaining, &bytes_read, NULL) || bytes_read == 0)
            return false;
        destination += bytes_read;
        remaining -= bytes_read;
    }
    return true;
}

static void disconnect_pipe(frame_source* p)
{
    p->pipe_connected = false;
    if (p->hPipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(p->hPipe);
        CloseHandle(p->hPipe);
        p->hPipe = INVALID_HANDLE_VALUE;
    }
    if (p->pipe_sws_ctx)
    {
        sws_freeContext(p->pipe_sws_ctx);
        p->pipe_sws_ctx = NULL;
    }
    free(p->pipe_buffer);
    p->pipe_buffer = NULL;
    p->pipe_buf_size = 0;
    p->pipe_width = 0;
    p->pipe_height = 0;
}

int init_video_decoder(frame_source* p, int output_width, int output_height)
{
    if (p->is_inited) return 0; // 已初始化则直接返回
    if (output_width <= 0 || output_width > 4096 || (output_width & 1) != 0 ||
        output_height <= 0 || output_height > 2160)
    {
        printf("错误：虚拟摄像头输出分辨率无效：%dx%d\n", output_width, output_height);
        return -1;
    }

    // 步骤1：注册 FFmpeg 组件（旧版本必需，兼容用）
    avformat_network_init();

    // 步骤2：打开 MP4 文件
    if (avformat_open_input(&p->fmt_ctx, p->video_path.c_str(), NULL, NULL) != 0)
    {
        printf("错误：无法打开 MP4 文件 %s\n", p->video_path.c_str());
        return -1;
    }

    // 步骤3：获取视频流信息（如分辨率、编码格式）
    if (avformat_find_stream_info(p->fmt_ctx, NULL) < 0)
    {
        printf("错误：无法获取视频流信息\n");
        avformat_close_input(&p->fmt_ctx); // 失败时释放资源
        return -1;
    }

    // 步骤4：查找视频流（跳过音频流）
    p->video_stream_idx = -1;
    for (unsigned int i = 0; i < p->fmt_ctx->nb_streams; i++)
    {
        if (p->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            p->video_stream_idx = i;
            break;
        }
    }
    if (p->video_stream_idx == -1)
    {
        printf("错误：MP4 文件中没有视频流\n");
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }

    // 步骤5：初始化解码器
    AVCodecParameters* codec_par = p->fmt_ctx->streams[p->video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id); // 根据编码格式找解码器
    if (!codec)
    {
        printf("错误：不支持的编码格式（ID：%d）\n", codec_par->codec_id);
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }

    // 分配解码器上下文并关联参数
    p->codec_ctx = avcodec_alloc_context3(codec);
    if (!p->codec_ctx)
    {
        printf("错误：无法分配解码器上下文\n");
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }
    if (avcodec_parameters_to_context(p->codec_ctx, codec_par) < 0)
    {
        printf("错误：解码器参数配置失败\n");
        avcodec_free_context(&p->codec_ctx);
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }

    // 打开解码器
    if (avcodec_open2(p->codec_ctx, codec, NULL) < 0)
    {
        printf("错误：解码器打开失败\n");
        avcodec_free_context(&p->codec_ctx);
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }

    if (p->codec_ctx->width <= 0 || p->codec_ctx->height <= 0 ||
        p->codec_ctx->pix_fmt == AV_PIX_FMT_NONE)
    {
        printf("错误：MP4 视频流没有有效的分辨率或像素格式\n");
        avcodec_free_context(&p->codec_ctx);
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }

    // 步骤6：初始化帧缓存（存储解码后的数据）
    p->yuv_frame = av_frame_alloc();
    p->rgb_frame = av_frame_alloc();
    p->pkt = av_packet_alloc();
    if (!p->yuv_frame || !p->rgb_frame || !p->pkt)
    {
        printf("错误：无法分配视频帧或数据包\n");
        release_video_resources(p);
        return -1;
    }

    // 步骤7：创建格式转换上下文（YUV→RGB24，与原项目兼容）
    p->sws_ctx = sws_getContext(
        p->codec_ctx->width,        // 输入宽度（视频原始宽度）
        p->codec_ctx->height,       // 输入高度（视频原始高度）
        p->codec_ctx->pix_fmt,      // 输入格式（视频的 YUV 格式）
        output_width,
        output_height,
        AV_PIX_FMT_RGB24,           // 输出格式（24位 RGB，原项目需要）
        SWS_BILINEAR,               // 缩放算法（平滑处理）
        NULL, NULL, NULL
    );
    if (!p->sws_ctx)
    {
        printf("错误：格式转换上下文创建失败\n");
        release_video_resources(p);
        return -1;
    }

    // 步骤8：为 RGB 帧分配内存（与 DIB 缓冲区格式对齐）
    int rgb_buf_size = av_image_get_buffer_size(
        AV_PIX_FMT_RGB24,
        output_width,
        output_height,
        1  // 无字节对齐（兼容 DIB）
    );
    if (rgb_buf_size <= 0)
    {
        printf("错误：RGB 视频帧尺寸无效\n");
        release_video_resources(p);
        return -1;
    }
    p->rgb_buffer = (uint8_t*)av_malloc((size_t)rgb_buf_size);
    if (!p->rgb_buffer || av_image_fill_arrays(
        p->rgb_frame->data,
        p->rgb_frame->linesize,
        p->rgb_buffer,
        AV_PIX_FMT_RGB24,
        output_width,
        output_height,
        1
    ) < 0)
    {
        printf("错误：无法分配 RGB 视频帧缓冲区\n");
        release_video_resources(p);
        return -1;
    }

    // 步骤9：记录视频分辨率（用于虚拟摄像头输出）
    p->width = output_width;
    p->height = output_height;
    p->is_inited = true;  // 标记初始化完成

    printf("MP4 输入 %dx%d -> 虚拟摄像头 %dx%d\n",
        p->codec_ctx->width, p->codec_ctx->height, p->width, p->height);
    return 0;
}

// 修改后的 create_dib 函数（仅保留 DIB 缓冲区创建逻辑）
int create_dib(frame_source* p, int w, int h)
{
    // Release the DC before deleting its bitmap so the DIB is no longer selected.
    if (p->hdc) {
        DeleteDC(p->hdc);
        p->hdc = NULL;
    }
    if (p->hbmp) {
        DeleteObject(p->hbmp);
        p->hbmp = NULL;
    }
    p->rgb_data = NULL;

    // 2. 分辨率验证（不变）
    if (w <= 0 || w > 4096 || (w & 1) != 0 || h <= 0 || h > 2160) {
        printf("【错误】create_dib：无效分辨率 %dx%d\n", w, h);
        return -1;
    }

    // 3. 关键修复：计算4字节对齐后的行字节数和总大小
    int row_bytes = (w * 3 + 3) & ~3;  // 每行字节数按4对齐（例：1920×3=5760，5760是4的倍数，row_bytes=5760）
    int total_size = row_bytes * h;    // 总大小 = 对齐后的行字节数 × 高度（而非 w×h×3）

    // 4. 创建兼容DC（不变）
    p->hdc = CreateCompatibleDC(NULL);
    if (!p->hdc) {
        printf("【错误】create_dib：CreateCompatibleDC 失败！错误码：%d\n", GetLastError());
        return -1;
    }

    // 5. 配置DIB信息（修改biSizeImage为对齐后的总大小）
    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;              // 负高度：从上到下存储
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = total_size;   // 关键：用对齐后的总大小，而非 w×h×3
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    // 6. 创建DIB（不变，但此时分配的内存是对齐后的大小）
    p->hbmp = CreateDIBSection(
        p->hdc,
        (BITMAPINFO*)&bi,
        DIB_RGB_COLORS,
        &p->rgb_data,
        NULL,
        0
    );
    if (!p->hbmp || !p->rgb_data) {
        printf("【严重错误】create_dib：CreateDIBSection 失败！错误码：%d\n", GetLastError());
        if (p->hbmp) DeleteObject(p->hbmp);
        if (p->hdc) DeleteDC(p->hdc);
        p->hdc = NULL;
        return -1;
    }

    // 7. 绑定DIB到DC（不变）
    SelectObject(p->hdc, p->hbmp);

    // 8. 打印日志（新增对齐信息，方便调试）
    printf("【成功】create_dib：创建对齐DIB缓冲区\n");
    printf("  - 分辨率：%dx%d\n", w, h);
    printf("  - 每行字节数（对齐后）：%d\n", row_bytes);
    printf("  - 总大小：%d 字节\n", total_size);
    return 0;
}

static void copy_rgb_frame_to_dib(frame_source* p)
{
    const int source_stride = p->rgb_frame->linesize[0];
    const int destination_stride = (p->width * 3 + 3) & ~3;
    const int row_bytes = p->width * 3;
    for (int row = 0; row < p->height; ++row)
    {
        memcpy((uint8_t*)p->rgb_data + (size_t)row * destination_stride,
            p->rgb_frame->data[0] + (size_t)row * source_stride,
            (size_t)row_bytes);
    }
}

static bool decode_next_video_frame(frame_source* p)
{
    for (;;)
    {
        const int receive_result = avcodec_receive_frame(p->codec_ctx, p->yuv_frame);
        if (receive_result == 0) return true;
        if (receive_result == AVERROR_EOF)
        {
            if (!p->is_loop) return false;
            if (av_seek_frame(p->fmt_ctx, p->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0)
                return false;
            avcodec_flush_buffers(p->codec_ctx);
            p->decoder_draining = false;
            p->packet_pending = false;
            continue;
        }
        if (receive_result != AVERROR(EAGAIN)) return false;

        if (p->packet_pending)
        {
            const int send_result = avcodec_send_packet(p->codec_ctx, p->pkt);
            if (send_result == AVERROR(EAGAIN)) continue;
            av_packet_unref(p->pkt);
            p->packet_pending = false;
            if (send_result < 0) return false;
            continue;
        }

        if (p->decoder_draining)
        {
            // A draining decoder should return a frame or EOF. EAGAIN here
            // means it cannot make progress, so fail rather than spin forever.
            return false;
        }

        const int read_result = av_read_frame(p->fmt_ctx, p->pkt);
        if (read_result < 0)
        {
            const int send_result = avcodec_send_packet(p->codec_ctx, NULL);
            if (send_result == AVERROR(EAGAIN)) continue;
            if (send_result < 0 && send_result != AVERROR_EOF) return false;
            p->decoder_draining = true;
            continue;
        }

        if (p->pkt->stream_index != p->video_stream_idx)
        {
            av_packet_unref(p->pkt);
            continue;
        }
        p->packet_pending = true;
    }
}


// 修改后的帧回调函数（核心逻辑）
int frame_source_callback(frame_t* frame)
{
    if (!frame || !frame->param || !frame->buffer) return -1;
    frame_source* p = (frame_source*)frame->param;
    // The callback dimensions describe the fixed UVC output, not the source.
    // Validate them before allocating, reading input, or writing YUY2 pixels.
    if (frame->width <= 0 || frame->width > 4096 || (frame->width & 1) != 0 ||
        frame->height <= 0 || frame->height > 2160 ||
        frame->length < (int64_t)frame->width * frame->height * 2)
    {
        printf("Invalid YUY2 output buffer: %dx%d, capacity %d bytes.\n",
            frame->width, frame->height, frame->length);
        return -1;
    }
    if ((p->width != 0 || p->height != 0) &&
        (p->width != frame->width || p->height != frame->height))
    {
        printf("Camera output changed from %dx%d to %dx%d; recreate the camera.\n",
            p->width, p->height, frame->width, frame->height);
        return -1;
    }

    // --------------------------
    // 1. 管道模式处理（无参数时）
    // --------------------------
    if (p->is_pipe_mode)
    {
        // The USB descriptor fixes the output size. Scale incoming frames to it.
        if (p->width == 0 || p->height == 0)
        {
            if (create_dib(p, frame->width, frame->height) != 0) return -1;
            p->width = frame->width;
            p->height = frame->height;
        }

        if (p->pipe_connected && p->hPipe != INVALID_HANDLE_VALUE)
        {
            if (!p->pipe_buffer)
            {
                int width, height;
                if (!read_pipe_exact(p->hPipe, &width, sizeof(width)) ||
                    !read_pipe_exact(p->hPipe, &height, sizeof(height)))
                {
                    printf("Pipe closed before sending its resolution header.\n");
                    disconnect_pipe(p);
                    return -1;
                }

                if (width <= 0 || width > 4096 || height <= 0 || height > 2160)
                {
                    printf("Rejected pipe resolution %dx%d.\n", width, height);
                    disconnect_pipe(p);
                    return -1;
                }
                const int64_t source_size = (int64_t)width * height * 3;
                if (source_size > 30LL * 1024 * 1024)
                {
                    printf("Rejected pipe frame size of %lld bytes.\n", source_size);
                    disconnect_pipe(p);
                    return -1;
                }

                p->pipe_sws_ctx = sws_getContext(
                    width, height, AV_PIX_FMT_RGB24,
                    p->width, p->height, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, NULL, NULL, NULL);
                if (!p->pipe_sws_ctx)
                {
                    printf("Unable to scale pipe frames from %dx%d to %dx%d.\n",
                        width, height, p->width, p->height);
                    disconnect_pipe(p);
                    return -1;
                }

                p->pipe_buf_size = (int)source_size;
                p->pipe_buffer = (char*)malloc((size_t)p->pipe_buf_size);
                if (!p->pipe_buffer)
                {
                    printf("Unable to allocate %d bytes for pipe frames.\n", p->pipe_buf_size);
                    disconnect_pipe(p);
                    return -1;
                }
                p->pipe_width = width;
                p->pipe_height = height;
                printf("Pipe input %dx%d -> camera output %dx%d.\n",
                    width, height, p->width, p->height);
            }

            const DWORD mutex_status = WaitForSingleObject(p->hPipeMutex, INFINITE);
            if (mutex_status != WAIT_OBJECT_0 && mutex_status != WAIT_ABANDONED)
            {
                printf("Unable to lock the pipe frame buffer.\n");
                disconnect_pipe(p);
                return -1;
            }

            const BOOL read_ok = read_pipe_exact(p->hPipe, p->pipe_buffer, (DWORD)p->pipe_buf_size);
            ReleaseMutex(p->hPipeMutex);
            if (!read_ok)
            {
                printf("Pipe disconnected while sending a frame.\n");
                disconnect_pipe(p);
                return -1;
            }

            const uint8_t* source_data[4] = { (const uint8_t*)p->pipe_buffer, NULL, NULL, NULL };
            const int source_linesize[4] = { p->pipe_width * 3, 0, 0, 0 };
            const int output_row_bytes = (p->width * 3 + 3) & ~3;
            const int output_linesize[4] = { output_row_bytes, 0, 0, 0 };
            uint8_t* output_data[4] = { (uint8_t*)p->rgb_data, NULL, NULL, NULL };
            const int scaled_rows = sws_scale(
                p->pipe_sws_ctx, source_data, source_linesize, 0, p->pipe_height,
                output_data, output_linesize);
            if (scaled_rows != p->height)
            {
                printf("Unable to scale a complete pipe frame.\n");
                disconnect_pipe(p);
                return -1;
            }
        }

        if (!p->rgb_data || !frame->buffer || p->width <= 0 || p->height <= 0)
        {
            printf("Pipe mode has no valid output frame buffer.\n");
            return -1;
        }

        const int output_row_bytes = (p->width * 3 + 3) & ~3;
        if (!p->pipe_sws_ctx || !p->pipe_buffer)
            memset(p->rgb_data, 0, (size_t)output_row_bytes * p->height);
        rgb24_yuy2(p->rgb_data, frame->buffer, p->width, p->height, output_row_bytes);
        frame->delay_msec = 33;
        return 0;
    }

    // --------------------------
    // 2. MP4 模式处理（有参数时）
    // --------------------------
    else
    {
        // 2.1 首次调用：初始化解码器
        if (!p->is_inited)
        {
            if (init_video_decoder(p, frame->width, frame->height) != 0)
            {
                printf("MP4 解码器初始化失败\n");
                return -1;
            }
            if (create_dib(p, p->width, p->height) != 0)
            {
                printf("Unable to allocate the video output frame.\n");
                release_video_resources(p);
                return -1;
            }
        }

        // 2.2 读取并解码一帧 MP4
        if (!decode_next_video_frame(p))
        {
            return -1;
        }

        // A decoder can change dimensions or pixel format within a stream.
        // Reconfigure from the actual frame while keeping UVC output fixed.
        if (p->yuv_frame->width <= 0 || p->yuv_frame->height <= 0 ||
            p->yuv_frame->format < 0) return -1;
        p->sws_ctx = sws_getCachedContext(
            p->sws_ctx,
            p->yuv_frame->width, p->yuv_frame->height,
            (AVPixelFormat)p->yuv_frame->format,
            p->width, p->height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, NULL, NULL, NULL);
        if (!p->sws_ctx) return -1;

        const int scaled_rows = sws_scale(
            p->sws_ctx,
            (const uint8_t* const*)p->yuv_frame->data,
            p->yuv_frame->linesize,
            0,
            p->yuv_frame->height,
            p->rgb_frame->data,
            p->rgb_frame->linesize
        );
        if (scaled_rows != p->height) return -1;

        copy_rgb_frame_to_dib(p);
        const int row_bytes = (p->width * 3 + 3) & ~3;
        rgb24_yuy2(p->rgb_data, frame->buffer, p->width, p->height, row_bytes);

        AVRational frame_rate = p->fmt_ctx->streams[p->video_stream_idx]->avg_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0)
            frame_rate = p->fmt_ctx->streams[p->video_stream_idx]->r_frame_rate;
        const double fps = frame_rate.num > 0 && frame_rate.den > 0 ? av_q2d(frame_rate) : 30.0;
        frame->delay_msec = (int)(1000.0 / (fps >= 1.0 && fps <= 120.0 ? fps : 30.0));
        return 0;
    }

}

static DWORD WINAPI PipeConnectThread(LPVOID param);

frame_source* frame_source_create_file(const char* path)
{
    if (!path || !*path) return NULL;
    frame_source* source = new (std::nothrow) frame_source();
    if (!source) return NULL;
    source->video_path = path;
    source->is_loop = true;
    return source;
}

frame_source* frame_source_create_pipe(void)
{
    frame_source* source = new (std::nothrow) frame_source();
    if (!source) return NULL;
    source->is_pipe_mode = true;
    source->is_loop = true;
    source->hPipeMutex = CreateMutex(NULL, FALSE, NULL);
    if (!source->hPipeMutex)
    {
        printf("Unable to create the pipe mutex (error %lu).\n", GetLastError());
        delete source;
        return NULL;
    }
    return source;
}

bool frame_source_start_pipe_listener(frame_source* source)
{
    if (!source || !source->is_pipe_mode || source->hPipe != INVALID_HANDLE_VALUE)
        return false;

    source->hPipe = CreateNamedPipeA(
        "\\\\.\\pipe\\vcam_pipe",
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        0,
        1024 * 1024,
        5000,
        NULL);
    if (source->hPipe == INVALID_HANDLE_VALUE)
    {
        printf("Unable to create the named pipe (error %lu).\n", GetLastError());
        return false;
    }

    source->pipe_connect_thread = CreateThread(
        NULL, 0, PipeConnectThread, source, 0, NULL);
    if (!source->pipe_connect_thread)
    {
        printf("Unable to start the pipe listener (error %lu).\n", GetLastError());
        CloseHandle(source->hPipe);
        source->hPipe = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

void frame_source_destroy(frame_source* source)
{
    if (!source) return;
    source->shutting_down.store(true);

    if (source->pipe_connect_thread)
    {
        CancelSynchronousIo(source->pipe_connect_thread);
        if (source->hPipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(source->hPipe, NULL);
            CloseHandle(source->hPipe);
        }
        WaitForSingleObject(source->pipe_connect_thread, INFINITE);
        CloseHandle(source->pipe_connect_thread);
        source->pipe_connect_thread = NULL;
        source->hPipe = INVALID_HANDLE_VALUE;
    }

    disconnect_pipe(source);
    release_video_resources(source);
    if (source->hdc) DeleteDC(source->hdc);
    if (source->hbmp) DeleteObject(source->hbmp);
    if (source->hPipeMutex) CloseHandle(source->hPipeMutex);
    delete source;
}

static DWORD WINAPI PipeConnectThread(LPVOID param)
{
    if (!param) return 1;
    frame_source* source = (frame_source*)param;
    const BOOL connected = ConnectNamedPipe(source->hPipe, NULL);
    const DWORD error = GetLastError();
    if (connected || error == ERROR_PIPE_CONNECTED)
    {
        source->pipe_connected.store(true);
        printf("Named-pipe client connected; waiting for RGB24 frames.\n");
    }
    else
    {
        source->pipe_connected.store(false);
        if (!source->shutting_down.load())
            printf("Named-pipe connection failed (error %lu).\n", error);
    }
    return 0;
}

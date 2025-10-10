//////Fanxiushu 2016-10-06

#include <Windows.h>
#include <stdio.h>
#include "uvc_vcam.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>  // 必须包含这个头文件！
}
//// RGB -> YUV 从网络查询的算法
void rgb24_yuy2(void* rgb, void* yuy2, int width, int height)
{
    int R1, G1, B1, R2, G2, B2, Y1, U1, Y2, V1;
    unsigned char* pRGBData = (unsigned char*)rgb;
    unsigned char* pYUVData = (unsigned char*)yuy2;

    for (int i = 0; i < height; ++i)
    {
        for (int j = 0; j < width / 2; ++j)
        {
            B1 = *(pRGBData + i * width * 3 + j * 6);
            G1 = *(pRGBData + i * width * 3 + j * 6 + 1);
            R1 = *(pRGBData + i * width * 3 + j * 6 + 2);
            B2 = *(pRGBData + i * width * 3 + j * 6 + 3);
            G2 = *(pRGBData + i * width * 3 + j * 6 + 4);
            R2 = *(pRGBData + i * width * 3 + j * 6 + 5);

            Y1 = ((66 * R1 + 129 * G1 + 25 * B1 + 128) >> 8) + 16;
            U1 = (((-38 * R1 - 74 * G1 + 112 * B1 + 128) >> 8) + ((-38 * R2 - 74 * G2 + 112 * B2 + 128) >> 8)) / 2 + 128;
            Y2 = ((66 * R2 + 129 * G2 + 25 * B2 + 128) >> 8) + 16;
            V1 = (((112 * R1 - 94 * G1 - 18 * B1 + 128) >> 8) + ((112 * R2 - 94 * G2 - 18 * B2 + 128) >> 8)) / 2 + 128;

            *(pYUVData + i * width * 2 + j * 4) = max(min(Y1, 255), 0);
            *(pYUVData + i * width * 2 + j * 4 + 1) = max(min(U1, 255), 0);
            *(pYUVData + i * width * 2 + j * 4 + 2) = max(min(Y2, 255), 0);
            *(pYUVData + i * width * 2 + j * 4 + 3) = max(min(V1, 255), 0);
        }
    }
}

////////////////////
#include <string>
// 引入 FFmpeg 头文件（必须放在 extern "C" 中，避免编译错误）
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct vcam_param
{
    // 保留原有的 DIB 相关成员（用于 RGB 临时缓存）
    HBITMAP hbmp;
    HDC hdc;
    void* rgb_data;
    int width;
    int height;

    // 新增：MP4 解码相关成员（核心修改点）
    std::string video_path;       // 存储 MP4 文件路径
    AVFormatContext* fmt_ctx;     // FFmpeg 格式上下文（管理视频文件）
    AVCodecContext* codec_ctx;    // 解码器上下文（负责视频解码）
    AVFrame* yuv_frame;           // 存储解码后的原始 YUV 帧
    AVFrame* rgb_frame;           // 存储转换后的 RGB 帧（用于后续转 YUV）
    SwsContext* sws_ctx;          // 格式转换上下文（YUV→RGB）
    AVPacket* pkt;                // 存储待解码的视频数据包
    int video_stream_idx;         // 视频流索引（区分视频/音频）
    bool is_loop;                 // 是否循环播放视频
    bool is_inited;               // 解码器是否初始化完成（避免重复初始化）
};

// 新增：初始化 MP4 解码器（关键函数）
int init_video_decoder(vcam_param* p)
{
    if (p->is_inited) return 0; // 已初始化则直接返回

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
    for (int i = 0; i < p->fmt_ctx->nb_streams; i++)
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

    // 步骤6：初始化帧缓存（存储解码后的数据）
    p->yuv_frame = av_frame_alloc();  // 原始 YUV 帧
    p->rgb_frame = av_frame_alloc();  // 转换后的 RGB 帧
    p->pkt = av_packet_alloc();       // 待解码的数据包

    // 步骤7：创建格式转换上下文（YUV→RGB24，与原项目兼容）
    p->sws_ctx = sws_getContext(
        p->codec_ctx->width,        // 输入宽度（视频原始宽度）
        p->codec_ctx->height,       // 输入高度（视频原始高度）
        p->codec_ctx->pix_fmt,      // 输入格式（视频的 YUV 格式）
        p->codec_ctx->width,        // 输出宽度（与输入一致）
        p->codec_ctx->height,       // 输出高度（与输入一致）
        AV_PIX_FMT_RGB24,           // 输出格式（24位 RGB，原项目需要）
        SWS_BILINEAR,               // 缩放算法（平滑处理）
        NULL, NULL, NULL
    );
    if (!p->sws_ctx)
    {
        printf("错误：格式转换上下文创建失败\n");
        // 释放已分配的资源（避免内存泄漏）
        av_packet_free(&p->pkt);
        av_frame_free(&p->rgb_frame);
        av_frame_free(&p->yuv_frame);
        avcodec_free_context(&p->codec_ctx);
        avformat_close_input(&p->fmt_ctx);
        return -1;
    }

    // 步骤8：为 RGB 帧分配内存（与 DIB 缓冲区格式对齐）
    int rgb_buf_size = av_image_get_buffer_size(
        AV_PIX_FMT_RGB24,
        p->codec_ctx->width,
        p->codec_ctx->height,
        1  // 无字节对齐（兼容 DIB）
    );
    uint8_t* rgb_buf = (uint8_t*)av_malloc(rgb_buf_size);
    av_image_fill_arrays(
        p->rgb_frame->data,
        p->rgb_frame->linesize,
        rgb_buf,
        AV_PIX_FMT_RGB24,
        p->codec_ctx->width,
        p->codec_ctx->height,
        1
    );

    // 步骤9：记录视频分辨率（用于虚拟摄像头输出）
    p->width = p->codec_ctx->width;
    p->height = p->codec_ctx->height;
    p->is_inited = true;  // 标记初始化完成

    printf("MP4 初始化成功：%dx%d\n", p->width, p->height);
    return 0;
}

// 修改后的 create_dib 函数（仅保留 DIB 缓冲区创建逻辑）
int create_dib(vcam_param* p, int w, int h)
{
    // 分辨率未变则无需重新创建
    if (p->width == w && p->height == h && p->hbmp) return 0;

    // 释放旧资源（避免内存泄漏）
    if (p->hbmp) DeleteObject(p->hbmp);
    if (p->hdc) DeleteDC(p->hdc);
    p->hbmp = NULL;
    p->hdc = NULL;

    // 创建兼容 DC
    p->hdc = CreateCompatibleDC(NULL);
    if (!p->hdc)
    {
        printf("CreateCompatibleDC 错误：%d\n", GetLastError());
        return -1;
    }

    // 配置 DIB 信息（24位 RGB，与视频帧匹配）
    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;  // 关键：与 FFmpeg 输出的 RGB24 一致
    bi.biCompression = BI_RGB;

    // 创建 DIB 缓冲区（获取 RGB 数据指针）
    p->hbmp = CreateDIBSection(
        p->hdc,
        (BITMAPINFO*)&bi,
        DIB_RGB_COLORS,
        &p->rgb_data,
        NULL,
        0
    );
    if (!p->hbmp)
    {
        p->rgb_data = NULL;
        printf("CreateDIBSection 错误：%d\n", GetLastError());
        DeleteDC(p->hdc);
        p->hdc = NULL;
        return -1;
    }

    // 关联 DIB 到 DC
    SelectObject(p->hdc, p->hbmp);
    p->width = w;
    p->height = h;

    return 0;
}



// 修改后的帧回调函数（核心逻辑）
int frame_callback(frame_t* frame)
{
    vcam_param* p = (vcam_param*)frame->param;

    // 步骤1：首次调用时初始化解码器
    if (!p->is_inited)
    {
        if (init_video_decoder(p) != 0)
        {
            printf("回调错误：解码器初始化失败\n");
            return -1;
        }
        // 同步视频分辨率到虚拟摄像头
        frame->width = p->width;
        frame->height = p->height;
        // 创建匹配的 DIB 缓冲区
        if (create_dib(p, p->width, p->height) != 0)
        {
            printf("回调错误：DIB 创建失败\n");
            return -1;
        }
    }

    // 步骤2：读取并解码一帧视频
    int ret = 0;
    while (av_read_frame(p->fmt_ctx, p->pkt) >= 0)
    {
        // 只处理视频流
        if (p->pkt->stream_index != p->video_stream_idx)
        {
            av_packet_unref(p->pkt); // 释放音频包
            continue;
        }

        // 发送数据包到解码器
        ret = avcodec_send_packet(p->codec_ctx, p->pkt);
        if (ret < 0)
        {
            av_packet_unref(p->pkt);
            printf("解码错误：发送数据包失败\n");
            break;
        }

        // 接收解码后的 YUV 帧
        ret = avcodec_receive_frame(p->codec_ctx, p->yuv_frame);
        if (ret == AVERROR(EAGAIN)) // 需要更多数据
        {
            av_packet_unref(p->pkt);
            continue;
        }
        else if (ret == AVERROR_EOF) // 视频播放结束
        {
            if (p->is_loop) // 循环播放：重置到开头
            {
                av_seek_frame(p->fmt_ctx, p->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(p->codec_ctx); // 清空解码器缓存
                av_packet_unref(p->pkt);
                continue;
            }
            else
            {
                return -1; // 非循环模式：结束播放
            }
        }
        else if (ret < 0) // 其他错误
        {
            printf("解码错误：接收帧失败\n");
            av_packet_unref(p->pkt);
            break;
        }

        // 步骤3：将 YUV 帧转换为 RGB（原项目需要 RGB 输入）
        sws_scale(
            p->sws_ctx,
            (const uint8_t* const*)p->yuv_frame->data,
            p->yuv_frame->linesize,
            0,
            p->height,
            p->rgb_frame->data,
            p->rgb_frame->linesize
        );

        // 步骤4：将 RGB 数据复制到 DIB 缓冲区
        memcpy(p->rgb_data, p->rgb_frame->data[0], p->width * p->height * 3);

        // 步骤5：转换为 YUY2 格式（虚拟摄像头需要）
        rgb24_yuy2(p->rgb_data, frame->buffer, frame->width, frame->height);

        // 步骤6：设置帧率（从视频中获取，默认 33ms ≈ 30fps）
        frame->delay_msec = 1000 / av_q2d(p->fmt_ctx->streams[p->video_stream_idx]->r_frame_rate);
        av_packet_unref(p->pkt); // 释放当前数据包
        break; // 成功处理一帧，退出循环
    }

    return 0;
}

// 修改后的 main 函数
int main(int argc, char** argv)
{
    // 检查是否提供了 MP4 文件路径
    if (argc < 2)
    {
        printf("用法：%s <MP4文件路径1> [MP4文件路径2]\n", argv[0]);
        return 1;
    }

    // 初始化第一个虚拟摄像头
    uvc_vcam_t uvc1;
    vcam_param p1;
    memset(&p1, 0, sizeof(p1));
    p1.video_path = argv[1];    // 设置第一个摄像头的 MP4 路径
    p1.is_loop = true;          // 循环播放
    p1.is_inited = false;       // 初始化解码器标记

    uvc1.pid = 0xcc10;
    uvc1.vid = 0xbb10;
    uvc1.manu_fact = "Fanxiushu";
    uvc1.product = "MP4 Virtual Camera 1";
    uvc1.frame_callback = frame_callback;
    uvc1.param = &p1;

    void* vcam1 = vcam_create(&uvc1);
    if (!vcam1)
    {
        printf("创建第一个虚拟摄像头失败\n");
        return 1;
    }

    // 初始化第二个虚拟摄像头（如果提供了第二个文件）
    void* vcam2 = NULL;
    vcam_param p2;
    if (argc >= 3)
    {
        memset(&p2, 0, sizeof(p2));
        p2.video_path = argv[2];    // 第二个摄像头的 MP4 路径
        p2.is_loop = true;
        p2.is_inited = false;

        uvc_vcam_t uvc2;
        uvc2.pid = 0xcc11;
        uvc2.vid = 0xbb11;
        uvc2.manu_fact = "Fanxiushu";
        uvc2.product = "MP4 Virtual Camera 2";
        uvc2.frame_callback = frame_callback;
        uvc2.param = &p2;

        vcam2 = vcam_create(&uvc2);
        if (!vcam2)
        {
            printf("创建第二个虚拟摄像头失败\n");
        }
    }

    printf("虚拟摄像头已启动，按任意键退出...\n");
    getchar();

    // 释放资源（关键：避免内存泄漏）
    vcam_destroy(vcam1);
    if (vcam2) vcam_destroy(vcam2);

    // 释放第一个摄像头的 FFmpeg 资源
    if (p1.is_inited)
    {
        sws_freeContext(p1.sws_ctx);
        av_frame_free(&p1.rgb_frame);
        av_frame_free(&p1.yuv_frame);
        av_packet_free(&p1.pkt);
        avcodec_free_context(&p1.codec_ctx);
        avformat_close_input(&p1.fmt_ctx);
    }
    // 释放 DIB 资源
    if (p1.hbmp) DeleteObject(p1.hbmp);
    if (p1.hdc) DeleteDC(p1.hdc);

    // 释放第二个摄像头的资源（如果存在）
    if (p2.is_inited)
    {
        sws_freeContext(p2.sws_ctx);
        av_frame_free(&p2.rgb_frame);
        av_frame_free(&p2.yuv_frame);
        av_packet_free(&p2.pkt);
        avcodec_free_context(&p2.codec_ctx);
        avformat_close_input(&p2.fmt_ctx);
    }
    if (p2.hbmp) DeleteObject(p2.hbmp);
    if (p2.hdc) DeleteDC(p2.hdc);

    return 0;
}
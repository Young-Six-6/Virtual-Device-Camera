//////Fanxiushu 2016-10-06

#include <Windows.h>
#include <stdio.h>
#include <synchapi.h>
#include "uvc_vcam.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>  // 必须包含这个头文件！
}
//// RGB -> YUV 从网络查询的算法
//// RGB -> YUV 修复颜色颠倒（交换 R 和 B 通道）
void rgb24_yuy2(void* rgb, void* yuy2, int width, int height)
{
    int R1, G1, B1, R2, G2, B2, Y1, U1, Y2, V1;
    unsigned char* pRGBData = (unsigned char*)rgb;
    unsigned char* pYUVData = (unsigned char*)yuy2;

    for (int i = 0; i < height; ++i)
    {
        for (int j = 0; j < width / 2; ++j)
        {
            // 核心修复：按 [R, G, B] 顺序读取（原代码是 [B, G, R]）
            R1 = *(pRGBData + i * width * 3 + j * 6);        // 第1个像素的 R 通道
            G1 = *(pRGBData + i * width * 3 + j * 6 + 1);    // 第1个像素的 G 通道
            B1 = *(pRGBData + i * width * 3 + j * 6 + 2);    // 第1个像素的 B 通道

            R2 = *(pRGBData + i * width * 3 + j * 6 + 3);    // 第2个像素的 R 通道
            G2 = *(pRGBData + i * width * 3 + j * 6 + 4);    // 第2个像素的 G 通道
            B2 = *(pRGBData + i * width * 3 + j * 6 + 5);    // 第2个像素的 B 通道

            // 原有 YUV 计算逻辑不变（现在 R 和 B 已正确）
            Y1 = ((66 * R1 + 129 * G1 + 25 * B1 + 128) >> 8) + 16;
            // 取两个像素的 U 平均值（正确）
            U1 = (((-38 * R1 - 74 * G1 + 112 * B1 + 128) >> 8) +
                ((-38 * R2 - 74 * G2 + 112 * B2 + 128) >> 8)) / 2 + 128;
            Y2 = ((66 * R2 + 129 * G2 + 25 * B2 + 128) >> 8) + 16;
            // 取两个像素的 V 平均值（正确）
            V1 = (((112 * R1 - 94 * G1 - 18 * B1 + 128) >> 8) +
                ((112 * R2 - 94 * G2 - 18 * B2 + 128) >> 8)) / 2 + 128;

            // 边界处理（确保值在 0-255 范围内）
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
    SwsContext* sws_ctx = NULL;
    AVPacket* pkt = NULL;
    int video_stream_idx = -1;         // 初始化为-1（无流）
    bool is_loop = false;              // 默认为不循环
    bool is_inited = false;            // 默认为未初始化
    // 管道相关成员
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    bool is_pipe_mode = false;
    bool pipe_connected = false;
    char* pipe_buffer = NULL;
    int pipe_buf_size = 0;
    //
    HANDLE hPipeMutex; // 新增：管道数据互斥锁

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
    // 1. 先释放旧资源（避免内存泄漏）
    if (p->hbmp) {
        DeleteObject(p->hbmp);
        p->hbmp = NULL;
    }
    if (p->hdc) {
        DeleteDC(p->hdc);
        p->hdc = NULL;
    }
    // 重置 rgb_data（避免残留旧地址）
    p->rgb_data = NULL;

    // 2. 验证输入分辨率（必须>0，且不超过合理范围）
    if (w <= 0 || w > 4096 || h <= 0 || h > 2160) { // 限制最大 4K（4096x2160）
        printf("【错误】create_dib：无效分辨率 %dx%d（超出支持范围）\n", w, h);
        return -1;
    }

    // 3. 创建兼容 DC（必须成功）
    p->hdc = CreateCompatibleDC(NULL);
    if (!p->hdc) {
        printf("【错误】create_dib：CreateCompatibleDC 失败！错误码：%d\n", GetLastError());
        return -1;
    }

    // 4. 配置 DIB 信息（关键：显式设置 biSizeImage，避免系统计算错误）
    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;                // 宽度（正数值，避免画面颠倒）
    bi.biHeight = -h;              // 高度：负数值表示“从上到下”存储（与视频帧一致，避免翻转）
    bi.biPlanes = 1;               // 必须为 1
    bi.biBitCount = 24;            // RGB24（每个像素 3 字节）
    bi.biCompression = BI_RGB;     // 无压缩
    bi.biSizeImage = w * h * 3;    // 显式计算 DIB 大小（关键！确保分配足够内存）
    bi.biClrUsed = 0;              // 无调色板
    bi.biClrImportant = 0;

    // 5. 创建 DIB 像素缓冲区（核心步骤，必须检查结果）
    p->hbmp = CreateDIBSection(
        p->hdc,
        (BITMAPINFO*)&bi,
        DIB_RGB_COLORS,
        &p->rgb_data,  // 输出：合法的内存地址
        NULL,
        0
    );
    if (!p->hbmp || !p->rgb_data) {
        // 打印详细错误，帮助定位问题
        printf("【严重错误】create_dib：CreateDIBSection 失败！\n");
        printf("  - 分辨率：%dx%d\n", w, h);
        printf("  - 所需内存：%d 字节\n", w * h * 3);
        printf("  - 错误码：%d\n", GetLastError());
        // 清理失败的资源
        if (p->hdc) DeleteDC(p->hdc);
        p->hdc = NULL;
        return -1;
    }

    // 6. 绑定 DIB 到 DC（必须执行，否则后续绘制无效）
    SelectObject(p->hdc, p->hbmp);

    // 7. 打印成功日志（验证内存分配）
    printf("【成功】create_dib：创建 DIB 缓冲区\n");
    printf("  - 分辨率：%dx%d\n", w, h);
    printf("  - 内存地址：0x%p\n", p->rgb_data);
    printf("  - 内存大小：%d 字节\n", w * h * 3);
    return 0;
}


// 修改后的帧回调函数（核心逻辑）
int frame_callback(frame_t* frame)
{
    vcam_param* p = (vcam_param*)frame->param;

    // --------------------------
    // 1. 管道模式处理（无参数时）
    // --------------------------
    if (p->is_pipe_mode)
    {
        // 1.1 管道未初始化：尝试创建 DIB 缓冲区（默认黑屏用）
        if (p->width == 0 || p->height == 0)
        {
            // 初始化为黑屏（可自定义默认分辨率）
            create_dib(p, 1920, 1080);
            frame->width = 1920;
            frame->height = 1080;
        }

        // 1.2 管道已连接：读取数据并显示
        if (p->pipe_connected && p->hPipe != INVALID_HANDLE_VALUE)
        {
            // 第一次连接：先读取宽高（协议：4字节宽 + 4字节高）
            if (!p->pipe_buffer)
            {
                int width, height;
                WaitForSingleObject(p->hPipeMutex, INFINITE);
                DWORD bytesRead;
                // 读取宽度（4字节）
                BOOL readOK = ReadFile(p->hPipe, &width, 4, &bytesRead, NULL);
                if (!readOK || bytesRead != 4)
                {
                    // 读取失败（可能断开连接）
                    p->pipe_connected = false;
                    printf("管道读取宽高失败，重新等待连接...\n");
                    return 0;
                }
                // 读取高度（4字节）
                readOK = ReadFile(p->hPipe, &height, 4, &bytesRead, NULL);
                if (!readOK || bytesRead != 4)
                {
                    p->pipe_connected = false;
                    printf("管道读取宽高失败，重新等待连接...\n");
                    return 0;
                }
                if (width != p->width || height != p->height)
                {
                    create_dib(p, width, height); // 同步为管道分辨率
                    p->width = width;
                    p->height = height;
                    printf("【同步】DIB分辨率更新为：%dx%d\n", width, height);
                }
                // 验证分辨率合理性（避免过大导致内存溢出）
                if (width <= 0 || width > 4096 || height <= 0 || height > 2160)
                {
                    printf("无效分辨率：%dx%d，忽略\n", width, height);
                    return 0;
                }
                // 初始化管道缓存和 DIB 缓冲区
                p->width = width;
                p->height = height;
                // 动态计算1080P所需缓存大小（1920*1080*3=6220800）
                p->pipe_buf_size = width * height * 3;
                // 先释放旧缓存，避免内存泄漏
                if (p->pipe_buffer) {
                    free(p->pipe_buffer);
                    p->pipe_buffer = NULL;
                }
                // 分配缓存并检查结果
                p->pipe_buffer = (char*)malloc(p->pipe_buf_size);
                if (!p->pipe_buffer) {
                    printf("【错误】分配管道缓存失败！需要 %d 字节（%dx%d RGB24）\n",
                        p->pipe_buf_size, width, height);
                    p->pipe_buf_size = 0; // 标记无效，避免后续写入
                    return -1;
                }
                printf("【成功】分配管道缓存：%d 字节（%dx%d RGB24）\n",
                    p->pipe_buf_size, width, height);
            }
            // 读取像素数据（RGB24）
            else
            {
                DWORD bytesRead;
                BOOL readOK = ReadFile(p->hPipe, p->pipe_buffer, p->pipe_buf_size, &bytesRead, NULL);
                if (readOK && bytesRead == p->pipe_buf_size)
                {
                    // 数据有效：复制到 DIB 并转换为 YUY2
                    if (ReadFile(p->hPipe, p->pipe_buffer, p->pipe_buf_size, &bytesRead, NULL) &&
                        bytesRead == p->pipe_buf_size)
                    {
                        // 复制数据到DIB缓冲区（安全写入）
                        memcpy(p->rgb_data, p->pipe_buffer, p->pipe_buf_size);
                    }
                    // 操作完成后解锁
                    ReleaseMutex(p->hPipeMutex);
                    rgb24_yuy2(p->rgb_data, frame->buffer, p->width, p->height);
                    frame->width = p->width;
                    frame->height = p->height;
                    frame->delay_msec = 33; // 约 30fps
                    return 0;
                }
                else
                {
                    // 读取失败（管道断开）
                    p->pipe_connected = false;
                    free(p->pipe_buffer);
                    p->pipe_buffer = nullptr;
                    p->pipe_buf_size = 0;
                    printf("管道断开，重新等待连接...\n");
                }
            }
        }
        // 1.3 管道未连接或无数据：显示黑屏
        if (p->rgb_data)
        {
            // 新增：先验证 width 和 height 是否有效（避免计算出负数/0大小）
            if (p->width <= 0 || p->height <= 0) {
                printf("【错误】无效分辨率：%dx%d（width/height 必须>0）\n", p->width, p->height);
                return -1; // 终止操作，避免后续错误
            }
            // 计算 RGB24 总字节数（1920x1080 应为 6220800）
            int rgb_size = p->width * p->height * 3;
            // 新增：检查 rgb_size 是否合理（避免溢出或过小）
            if (rgb_size <= 0 || rgb_size > 1024 * 1024 * 30) { // 限制最大 30MB（足够支持4K）
                printf("【错误】无效 RGB 数据大小：%d 字节（%dx%d）\n", rgb_size, p->width, p->height);
                return -1;
            }
            // 确认内存合法后再执行 memset
            memset(p->rgb_data, 0, rgb_size);
            rgb24_yuy2(p->rgb_data, frame->buffer, p->width, p->height);
        }
        else
        {
            // 新增：打印错误，明确 rgb_data 未分配
            printf("【严重错误】p->rgb_data 未分配！无法执行 memset\n");
            return -1;
        }
    }

    // --------------------------
    // 2. MP4 模式处理（有参数时）
    // --------------------------
    else
    {
        // 2.1 首次调用：初始化解码器
        if (!p->is_inited)
        {
            if (init_video_decoder(p) != 0)
            {
                printf("MP4 解码器初始化失败\n");
                return -1;
            }
            // 同步视频分辨率到虚拟摄像头
            frame->width = p->width;
            frame->height = p->height;
            create_dib(p, p->width, p->height); // 创建 DIB 缓冲区
        }

        // 2.2 读取并解码一帧 MP4
        int ret = 0;
        while (av_read_frame(p->fmt_ctx, p->pkt) >= 0)
        {
            if (p->pkt->stream_index != p->video_stream_idx)
            {
                av_packet_unref(p->pkt); // 跳过音频包
                continue;
            }

            // 发送数据包到解码器
            ret = avcodec_send_packet(p->codec_ctx, p->pkt);
            if (ret < 0)
            {
                av_packet_unref(p->pkt);
                break;
            }

            // 接收解码后的 YUV 帧
            ret = avcodec_receive_frame(p->codec_ctx, p->yuv_frame);
            if (ret == AVERROR(EAGAIN))
            {
                av_packet_unref(p->pkt);
                continue;
            }
            else if (ret == AVERROR_EOF)
            {
                // 循环播放：重置到视频开头
                av_seek_frame(p->fmt_ctx, p->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(p->codec_ctx);
                av_packet_unref(p->pkt);
                continue;
            }
            else if (ret < 0)
            {
                av_packet_unref(p->pkt);
                break;
            }

            // 2.3 转换 YUV→RGB
            sws_scale(
                p->sws_ctx,
                (const uint8_t* const*)p->yuv_frame->data,
                p->yuv_frame->linesize,
                0,
                p->height,
                p->rgb_frame->data,
                p->rgb_frame->linesize
            );

            // 2.4 复制到 DIB 并转换为 YUY2
            memcpy(p->rgb_data, p->rgb_frame->data[0], p->width * p->height * 3);
            rgb24_yuy2(p->rgb_data, frame->buffer, frame->width, frame->height);

            // 2.5 设置帧率（从视频中获取）
            frame->delay_msec = (int)(1000 / av_q2d(p->fmt_ctx->streams[p->video_stream_idx]->r_frame_rate));
            av_packet_unref(p->pkt);
            break; // 处理完一帧后退出循环
        }

        return 0;
    }
}

// 修改后的 main 函数
// 1. 先声明线程函数（替换 Lambda，兼容所有编译器）
DWORD WINAPI PipeConnectThread(LPVOID param);

int main(int argc, char** argv)
{

    // 2. 初始化第一个虚拟摄像头参数（放在最前面，避免变量未定义）
    uvc_vcam_t uvc1;
    vcam_param p1;
    p1.hPipeMutex = CreateMutex(NULL, FALSE, NULL); // 初始化为“未拥有”状态
    memset(&p1, 0, sizeof(p1));  // 初始化所有成员为0
    p1.is_loop = true;            // MP4 默认循环播放
    p1.is_inited = false;

    // 3. 初始化第二个虚拟摄像头参数（提前定义，避免作用域问题）
    uvc_vcam_t uvc2;
    vcam_param p2;
    memset(&p2, 0, sizeof(p2));
    p2.is_loop = true;
    p2.is_inited = false;
    void* vcam2 = NULL;  // 提前定义第二个摄像头句柄

    // 4. 处理命令行参数
    bool has_mp4_param = false;
    if (argc >= 2)
    {
        // 有参数：使用 MP4 模式
        p1.video_path = argv[1];
        has_mp4_param = true;
        printf("第一个摄像头：使用 MP4 文件 -> %s\n", argv[1]);

        // 初始化第二个摄像头（如果有第二个参数）
        if (argc >= 3)
        {
            p2.video_path = argv[2];
            uvc2.pid = 0xcc11;
            uvc2.vid = 0xbb11;
            uvc2.manu_fact = "Fanxiushu";
            uvc2.product = "MP4 HD Camera 2";
            uvc2.frame_callback = frame_callback;
            uvc2.param = &p2;

            vcam2 = vcam_create(&uvc2);
            if (vcam2)
            {
                printf("第二个摄像头：使用 MP4 文件 -> %s\n", argv[2]);
            }
            else
            {
                printf("警告：创建第二个虚拟摄像头失败\n");
            }
        }
    }
    else
    {
        // 无参数：进入管道模式（只给第一个摄像头用）
        p1.is_pipe_mode = true;
        printf("用法：%s <MP4文件路径1> [MP4文件路径2]（使用MP4输入）\n", argv[0]);
        printf("当前无参数，第一个摄像头等待管道输入（管道名：\\\\.\\pipe\\vcam_pipe）\n");
    }

    // 5. 初始化第一个虚拟摄像头（无论 MP4 还是管道模式）
    uvc1.pid = 0xcc10;
    uvc1.vid = 0xbb10;
    uvc1.manu_fact = "Fanxiushu";
    uvc1.product = has_mp4_param ? "MP4 HD Camera 1" : "Pipe HD Camera 1";
    uvc1.frame_callback = frame_callback;
    uvc1.param = &p1;

    void* vcam1 = vcam_create(&uvc1);
    if (!vcam1)
    {
        printf("错误：创建第一个虚拟摄像头失败，程序退出\n");
        return 1;
    }

    // 6. 管道模式：启动线程等待连接（用传统函数，不依赖 Lambda）
    HANDLE hPipeThread = NULL;
    if (p1.is_pipe_mode)
    {
        // 创建命名管道
        p1.hPipe = CreateNamedPipeA(
            "\\\\.\\pipe\\vcam_pipe",
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,          // 最大实例数
            0,          // 输出缓冲区（不用）
            1024 * 1024,// 输入缓冲区（1MB）
            5000,       // 超时时间
            NULL
        );
        if (p1.hPipe == INVALID_HANDLE_VALUE)
        {
            printf("错误：创建管道失败，错误码：%d\n", GetLastError());
            // 管道创建失败仍保留摄像头（显示黑屏）
        }
        else
        {
            // 启动线程等待客户端连接（传递 p1 指针）
            hPipeThread = CreateThread(
                NULL,           // 默认安全属性
                0,              // 默认栈大小
                PipeConnectThread,  // 传统线程函数
                &p1,            // 传递参数（p1 地址）
                0,              // 立即运行
                NULL            // 不获取线程ID
            );
            if (hPipeThread == NULL)
            {
                printf("错误：启动管道线程失败，错误码：%d\n", GetLastError());
                DisconnectNamedPipe(p1.hPipe);
                CloseHandle(p1.hPipe);
                p1.hPipe = INVALID_HANDLE_VALUE;
            }
        }
    }

    // 7. 主循环：等待退出（统一用 getchar，简单兼容）
    printf("\n按任意键退出程序...\n");
    (void)getchar();  // 显式忽略返回值，避免警告

    // 8. 资源释放：按“创建逆序”释放，避免依赖问题
    // 8.1 释放线程句柄（如果存在）
    if (hPipeThread != NULL)
    {
        WaitForSingleObject(hPipeThread, 1000);  // 等待线程结束
        CloseHandle(hPipeThread);
    }

    // 8.2 释放虚拟摄像头
    if (vcam2 != NULL)
    {
        vcam_destroy(vcam2);
    }
    if (vcam1 != NULL)
    {
        vcam_destroy(vcam1);
    }

    // 8.3 释放第一个摄像头资源（FFmpeg + DIB + 管道）
    if (p1.is_inited)
    {
        // 释放 FFmpeg 资源
        if (p1.sws_ctx != NULL) sws_freeContext(p1.sws_ctx);
        if (p1.rgb_frame != NULL) av_frame_free(&p1.rgb_frame);
        if (p1.yuv_frame != NULL) av_frame_free(&p1.yuv_frame);
        if (p1.pkt != NULL) av_packet_free(&p1.pkt);
        if (p1.codec_ctx != NULL) avcodec_free_context(&p1.codec_ctx);
        if (p1.fmt_ctx != NULL) avformat_close_input(&p1.fmt_ctx);
    }
    // 释放 DIB 资源
    if (p1.hbmp != NULL) DeleteObject(p1.hbmp);
    if (p1.hdc != NULL) DeleteDC(p1.hdc);
    // 释放管道资源
    if (p1.hPipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(p1.hPipe);
        CloseHandle(p1.hPipe);
    }
    if (p1.pipe_buffer != NULL)
    {
        free(p1.pipe_buffer);
    }

    // 8.4 释放第二个摄像头资源（FFmpeg + DIB）
    if (p2.is_inited)
    {
        if (p2.sws_ctx != NULL) sws_freeContext(p2.sws_ctx);
        if (p2.rgb_frame != NULL) av_frame_free(&p2.rgb_frame);
        if (p2.yuv_frame != NULL) av_frame_free(&p2.yuv_frame);
        if (p2.pkt != NULL) av_packet_free(&p2.pkt);
        if (p2.codec_ctx != NULL) avcodec_free_context(&p2.codec_ctx);
        if (p2.fmt_ctx != NULL) avformat_close_input(&p2.fmt_ctx);
    }
    if (p2.hbmp != NULL) DeleteObject(p2.hbmp);
    if (p2.hdc != NULL) DeleteDC(p2.hdc);
    // 释放第一个摄像头的互斥锁
    if (p1.hPipeMutex != NULL) {
        CloseHandle(p1.hPipeMutex);
    }

    printf("程序已退出，所有资源已释放\n");
    return 0;
}

// 9. 实现管道连接线程函数（传统函数，替代 Lambda，兼容所有编译器）
DWORD WINAPI PipeConnectThread(LPVOID param)
{
    if (param == NULL) return 1;  // 参数为空，直接返回

    vcam_param* p = (vcam_param*)param;
    // 等待客户端连接（ConnectNamedPipe 会阻塞直到有连接）
    BOOL connectRet = ConnectNamedPipe(p->hPipe, NULL);
    DWORD lastErr = GetLastError();

    // 判断连接结果：成功 或 已存在连接（ERROR_PIPE_CONNECTED）
    if (connectRet || (lastErr == ERROR_PIPE_CONNECTED))
    {
        p->pipe_connected = true;
        printf("管道连接成功！等待外部数据输入（格式：4字节宽 + 4字节高 + RGB24数据）\n");
    }
    else
    {
        p->pipe_connected = false;
        printf("管道连接失败，错误码：%d\n", lastErr);
        // 连接失败关闭管道
        DisconnectNamedPipe(p->hPipe);
        CloseHandle(p->hPipe);
        p->hPipe = INVALID_HANDLE_VALUE;
    }

    return 0;
}
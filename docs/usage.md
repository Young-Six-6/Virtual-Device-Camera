# 运行与输入协议

## 启动和退出

在程序输出目录运行。按 Enter 或 Ctrl+C 停止；关闭控制台也会触发有序退出。

```powershell
.\virtual-device-camera.exe
.\virtual-device-camera.exe 'D:\Videos\front.mp4'
.\virtual-device-camera.exe 'D:\Videos\front.mp4' 'D:\Videos\rear.mp4'
.\virtual-device-camera.exe --resolution 1280x720 --control-pipe '\\.\pipe\vcam_control'
```

无视频参数时创建一个 RGB24 管道输入摄像头。指定一个或两个 MP4 路径时按顺序创建摄像头实例。相对视频路径相对于 EXE 所在目录；文件播放结束后循环。命令行支持 `--resolution 480|720|1080|WIDTHxHEIGHT`、`--pipe PATH`、`--control-pipe PATH` 和 `--help`。输入参数最多两个视频路径。

应用须能找到已安装的虚拟 USB 总线接口。只构建应用不会在 Windows 中注册摄像头。

启动后会打印 Web UI 地址和本次访问令牌。将该完整地址复制到本机浏览器，即可查看状态、预览和修改设置。页面使用 `camera.ini` 保存配置；详见[本机 Web UI](web-ui.md)。

## 输出分辨率

启动默认值来自 EXE 旁边的 `default_res.txt`，支持 480、720、1080 预设；启动参数 `--resolution` 可指定其他偶数宽度分辨率（宽 2–4096，高 1–2160）。运行后可用[控制管道](control-protocol.md)热更新。

热更新会重新建立 UVC 描述符和摄像头设备，客户端必须重新打开摄像头。应用和输入源保持运行，输入 RGB/TCP 管道不需要重新连接。新描述符或设备启动失败时，程序尝试恢复旧分辨率；响应会说明恢复结果。分辨率只保存在当前进程内存中，重启后仍读取默认配置。

输入尺寸可以不同于 UVC 输出尺寸；应用会缩放到输出尺寸。不同宽高比的图像会拉伸。

## UVC 格式与帧率

应用同时发布未压缩 YUY2（DirectShow 常显示为 `yuyv422`）和 MJPEG。720p YUY2 每帧为 1,843,200 字节；USB 2.0 高速等时端点每微帧最多承载 3×1024 字节，扣除每个 UVC 包头后，720p 的可用上限约为 13.3fps。USB 2.0 的标称 480Mbps 并不能全部用于摄像头图像。端点每微帧限制见 [Microsoft 等时传输说明](https://learn.microsoft.com/windows-hardware/drivers/usbcon/transfer-data-to-isochronous-endpoints)。

MJPEG 每帧先由 FFmpeg 压缩，UVC 描述符提供 1280×720 的 15fps 和 30fps 模式，并在当前协商帧预算内自动调节 JPEG 质量。USB 2.0 测试曾以 30fps 请求抓取 60 帧，约 25fps；实际帧率会随主机、输入源和画面复杂度变化。客户端若默认选 YUY2，可在 USB 2.0 环境选择 MJPEG 以降低未压缩带宽需求。

用 FFmpeg/DirectShow 选择 MJPEG：

```powershell
.\.deps\ffmpeg\bin\ffmpeg.exe -f dshow -video_size 1280x720 -framerate 30 -vcodec mjpeg `
  -i 'video=MP4 HD Camera 1' -frames:v 60 -f null -
```

选择 YUY2 格式时使用 `-pixel_format yuyv422`。USB 2.0 高速等时端点每微帧最多承载 3×1024 字节，扣除 UVC 包头后约为 24.6 MB/s；720p YUY2 单帧 1,843,200 字节，因此理论上限约 13.3fps。重建 HCD `2.0.0.28` 的 SuperSpeed 路径将每 125 微秒的端点预算设为 8,192 字节，可用带宽约 65.5 MB/s，足以传输 720p YUY2 30fps（约 55.3 MB/s）。Windows 测试机用 FFmpeg/DirectShow 抓取 120 帧，耗时约 3.97 秒，约 30fps。该实测使用软件虚拟 USB 总线和测试签名驱动；它验证了当前虚拟摄像头传输链路，不代表物理 USB 3 控制器的吞吐保证。旧版原驱动不支持 SuperSpeed 扩展时会回退到 USB 2.0。

## RGB24 命名管道输入

默认管道为 `\\.\pipe\vcam_pipe`，可用 `--pipe` 更改。每个客户端连接先发送一次 8 字节 little-endian 头部，再连续发送未压缩 RGB24 帧：

| 字段 | 编码 | 说明 |
|---|---|---|
| `width` | 有符号 32 位整数，小端 | 输入宽度，1–4096 |
| `height` | 有符号 32 位整数，小端 | 输入高度，1–2160 |
| 像素数据 | 每像素 R、G、B 各 1 字节 | 每帧恰为 `width × height × 3` 字节 |

每帧没有额外头部、长度、时间戳或填充。每条连接的尺寸固定；需要改变输入尺寸时断开并重新连接，再发送新的尺寸头。服务端持续运行并接受客户端断线重连，无需重启摄像头应用。发送端进程仍需按原方式重连到管道。输入帧最大 30 MiB。新帧只在完整接收后发布；无帧时摄像头输出黑帧。

RGB 输入管道默认只允许创建它的对象所有者和 SYSTEM 访问。控制管道默认 ACL 允许 Everyone；可在 Web UI 选择当前所有者、本机 Users 组或 Everyone。Named pipe 配置均拒绝远程客户端；完整说明见[本机 Web UI](web-ui.md)。

示例：

```powershell
.\.venv\Scripts\python.exe .\tools\python\ColorTest.py
.\.venv\Scripts\python.exe .\tools\python\ObsLocalPine.py
```

## Hyper-V/TCP 转发

在 Hyper-V 虚拟机中运行 `WebVideoReceive.py`，宿主机运行 `WebVideoSend.py`。默认接收地址为 `0.0.0.0:5008`；发送端默认查询名为 `camera` 的虚拟机地址，并从 OBS 场景获取图像。两端使用同一 `<ii` little-endian 尺寸头和固定长度 RGB24 帧。TCP 接收支持碎片读取与客户端重连。

TCP 没有身份验证或加密。仅在可信私有网络使用，并用防火墙限制接入范围；不要将 5008 端口暴露到公网。

## 管道预览

`ReceiveTest.py` 自行创建 `\\.\pipe\vcam_pipe` 并用 OpenCV 显示帧，可验证发送端。预览程序与 C++ 应用不能同时占用同一个输入管道。按 Esc 退出。

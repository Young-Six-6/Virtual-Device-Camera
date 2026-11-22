# Virtual Device Camera

这是一个 Windows x64 虚拟 UVC 摄像头。程序通过虚拟 USB 总线驱动创建 UVC 摄像头，可从 MP4 文件或 RGB24 帧流取图，并提供 YUY2 和 MJPEG 两种 UVC 格式。

## 能做什么

- 传入一个或两个 MP4 路径，分别创建一个或两个摄像头实例。
- 不传 MP4 时，通过 `\\.\pipe\vcam_pipe` 接收 RGB24 帧。
- 将输入缩放到配置的摄像头分辨率，提供兼容性更好的 YUY2 和带宽更省的 MJPEG。
- 通过独立命名管道查询状态，并在进程运行期间热更新输出分辨率。
- 提供本机 Web UI 查看驱动/管道状态、预览画面和持久化运行配置。
- 提供 OBS 本地管道、Hyper-V/TCP 转发、纯色帧发送和管道预览脚本。
- 同时支持 Visual Studio `Debug | x64` 和 `Release | x64` 构建。

## 快速开始

需要 Visual Studio 2022 或 2026 的“使用 C++ 的桌面开发”工作负载、Windows SDK，以及一份完整的 x64 shared FFmpeg SDK。Python 3.11 x64 仅在使用 Python 工具时需要。

```powershell
# 将 D:\SDKs\ffmpeg 替换成已解压的 FFmpeg shared SDK 根目录
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup.ps1 -FFmpegSource 'D:\SDKs\ffmpeg'

# 构建 Release
.\scripts\build.ps1 -Configuration Release
```

可执行文件位于 `build/bin/x64/Release/virtual-device-camera.exe`。构建输出目录中也会复制程序依赖的 FFmpeg DLL 和 `default_res.txt`。

更完整的环境准备、运行方式、帧流格式和故障处理见：

- [构建与环境准备](docs/building.md)
- [运行与输入协议](docs/usage.md)
- [项目结构与数据流](docs/architecture.md)
- [USB 请求 responder API](docs/usb-responder.md)
- [Python 工具](docs/python-tools.md)
- [本机 Web UI](docs/web-ui.md)
- [驱动包与来源记录](docs/driver.md)
- [维护与交接](docs/maintenance.md)

## 运行前要知道

USB 2.0 下 1280×720 未压缩 YUY2 每帧为 1,843,200 字节；高速等时端点的 24.576 MB/s 理论载荷上限折算约 13.3fps。应用也提供 MJPEG，可降低 USB 2.0 带宽需求。当前测试机运行本机测试签名的 HCD `2.0.0.49` 和 USB-tree `1.0.0.25`；无整机重启完成安装，设备均为 `OK`。Release 摄像头通过 DirectShow 在 1.99 秒内抓取 60 帧 1280×720 YUY2/30fps；有序退出后摄像头 PDO 消失，挂起事务与移除工作项为 0。低/全速 HID、WinUSB bulk IN/OUT 和 Microsoft OS 1.0 描述符合成设备探针此前在 HCD `.48` / tree `.24` 上通过。以上是软件虚拟总线结果，不是物理 USB 3 控制器或线缆带宽测试。所有 Root Hub 电源行为、URB 家族以及自定义 bulk/interrupt 设备的实机验证仍未完成；内置应用仍是 UVC 摄像头，库级 `vcam_create_ex` 提供自定义 responder API。测试范围、可替代边界和限制见[驱动文档](docs/driver.md)。

`config/default_res.txt` 支持 `480`、`720`、`1080` 三种预设。也可在启动时使用 `--resolution WIDTHxHEIGHT`，或通过控制管道在运行中修改输出分辨率。分辨率变更会重建 UVC 描述符并重新枚举设备，摄像头客户端需要重新打开设备。详见[控制管道协议](docs/control-protocol.md)。

```powershell
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py status --camera 1
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py resolution --camera 1 --resolution 1280x720
```

OBS 工具从 `OBS_WEBSOCKET_PASSWORD` 环境变量读取密码。旧版上游代码曾把密码直接写进源文件；若曾使用过该值，请在 OBS 中轮换凭据。TCP 转发接收端默认监听所有网卡的 5008 端口，只应在可信网络和受限防火墙规则下使用。

## 项目目录

```text
src/app/                   应用入口和控制服务
src/camera/                UVC 描述符和摄像头会话生命周期
src/media/                 MP4/管道帧源、缩放、像素格式转换
src/protocol/              UVC/USB 协议结构
src/usb/                   SetupAPI 和虚拟总线 IOCTL 传输
config/                    摄像头分辨率配置
drivers/usb-bus-legacy/    原版 x86/x64 驱动包和独立重建的 WDM 源码工程
tools/python/              OBS、TCP、管道测试工具
scripts/                   环境检查和构建脚本
docs/                      当前维护文档
archive/legacy-2016/       历史原文和旧接口代码，仅作溯源参考
```

## 许可与驱动

应用代码沿用仓库中的 MIT 许可文件。仓库保留了上游旧版驱动二进制，并新增了独立的 WDM 重建源码工程；重建源码不是原版驱动源码，也没有替换旧驱动包。驱动来源、签名、许可边界、重建范围和未完成的运行时验证见[驱动文档](docs/driver.md)。

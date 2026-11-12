# Virtual Device Camera

这是一个 Windows x64 虚拟 UVC 摄像头示例。程序通过既有虚拟 USB 总线驱动创建 UVC 摄像头，可从 MP4 文件或 RGB24 帧流取图，并转换为 YUY2 输出。

## 能做什么

- 传入一个或两个 MP4 路径，分别创建一个或两个摄像头实例。
- 不传 MP4 时，通过 `\\.\pipe\vcam_pipe` 接收 RGB24 帧。
- 将输入缩放到配置的摄像头分辨率，并转换成 UVC 使用的 YUY2 格式。
- 通过独立命名管道查询状态，并在进程运行期间热更新输出分辨率。
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
- [Python 工具](docs/python-tools.md)
- [驱动包与来源记录](docs/driver.md)
- [维护与交接](docs/maintenance.md)

## 运行前要知道

应用程序构建成功不等于系统摄像头已经可用。程序通过 SetupAPI 查找虚拟 USB 总线接口；若接口不存在，会报告 `not found virtual usb path`。仓库保留了上游提供的 x86/x64 驱动文件；INF 会把同一个 `xusb_virt.sys` 安装为不同目标名称。驱动来源线索、文件清单和当前限制记录在[驱动文档](docs/driver.md)。此工作区没有安装或验证该内核驱动，因此摄像头设备的端到端运行尚未确认。

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
drivers/usb-bus-legacy/    原仓库附带的旧版 x86/x64 驱动包
tools/python/              OBS、TCP、管道测试工具
scripts/                   环境检查和构建脚本
docs/                      当前维护文档
archive/legacy-2016/       历史原文和旧接口代码，仅作溯源参考
```

## 许可与驱动

应用代码沿用仓库中的 MIT 许可文件。内核驱动是单独的二进制和安装包；其来源、签名信息及许可状态需要与应用代码分别看待。仓库中没有完整的驱动源码，也没有证明该驱动适用于当前 Windows 版本的端到端验证记录。

# 构建与环境准备

本文描述如何从干净检出准备依赖并生成可运行的应用程序目录。C++ 项目使用 C++17；构建只处理用户态程序，不安装内核驱动。

## 必需环境

- Windows x64。
- Visual Studio 2022 或 Visual Studio 2026，安装“使用 C++ 的桌面开发”工作负载和 Windows SDK。
- FFmpeg x64 **shared** 开发包。`include`、`lib`、`bin` 必须来自同一份构建。
- Python 3.11 x64，仅在使用 Python 工具时需要。

FFmpeg 需要同时包含头文件、导入库和 DLL。可以从 [FFmpeg-Builds 发布页](https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases)取得 shared SDK；不要将不同版本的头文件、`.lib` 和 DLL 混用。

## 一次性准备

将 FFmpeg SDK 解压到本机目录，例如 `D:\SDKs\ffmpeg`，然后在仓库根目录运行：

```powershell
# C++ 项目和 Python 工具都需要
.\scripts\setup.ps1 -FFmpegSource 'D:\SDKs\ffmpeg'

# 只需要 C++ 项目时
.\scripts\setup.ps1 -FFmpegSource 'D:\SDKs\ffmpeg' -SkipPython
```

脚本会检查 FFmpeg SDK，将其复制到未纳入 Git 的 `.deps/ffmpeg`；默认还会创建 `.venv`、安装固定版本的 Python 包、检查依赖并构建 Debug。`-SkipPython` 会跳过 Python 环境，仍会执行 Debug 构建。

准备好 `.deps/ffmpeg` 后，后续只需运行构建脚本。若使用外部 SDK 路径，可直接传给构建脚本：

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Rebuild
.\scripts\build.ps1 -Configuration Release -FFmpegRoot 'D:\SDKs\ffmpeg'
```

解决方案也可直接在 Visual Studio 中打开 `xusb_vcam.sln`，选择 `Debug | x64` 或 `Release | x64`。项目按 Visual Studio 版本选择 v143/v145 工具集。验证过的 FFmpeg SDK 版本及下载散列记录在本地准备目录 `.deps/ffmpeg-source.json`；此下载清单不随仓库提交，使用其他来源时请记录其版本和散列。

## 输出与运行时依赖

| 配置 | 输出目录 |
|---|---|
| Debug | `build/bin/x64/Debug/` |
| Release | `build/bin/x64/Release/` |

每个输出目录包含 `virtual-device-camera.exe`、`default_res.txt`、默认配置 `camera.ini` 和程序链接所需的 FFmpeg 运行时 DLL：`avformat`、`avcodec`、`avutil`、`swscale`、`swresample`。运行时设置保存在 EXE 旁；后续构建不会覆盖已存在的 `camera.ini`。程序会切换当前工作目录到自身所在目录，因此从资源管理器、终端或 Visual Studio 启动时都能找到旁边的配置文件；相对 MP4 路径也以该目录为基准。

Debug 使用静态 Debug CRT，Release 使用静态 Release CRT。Release 不依赖 Debug CRT DLL。FFmpeg 仍以 shared DLL 方式分发，不能只复制 EXE。

## 常见问题

### 找不到 FFmpeg SDK

确认 SDK 根目录下有 `include/libavformat/avformat.h`、`lib/avformat.lib` 和 `bin/avformat-*.dll` 等文件；运行时 DLL 和导入库必须来自同一份 x64 shared SDK。可用 `-FFmpegRoot` 指向实际目录。

### 找不到 Visual Studio C++ 工具链

在 Visual Studio Installer 中安装“使用 C++ 的桌面开发”及 Windows SDK，然后重新运行 `scripts/build.ps1`。

### 构建成功，程序却报告找不到虚拟 USB 路径

这表示应用没有发现 `GUID_XUSB_VIRT_INTERFACE` 设备接口，与 FFmpeg 构建无关。请看[驱动包与来源记录](driver.md)；驱动安装和兼容性并不由此构建脚本处理。

### 在另一台电脑运行时找不到 DLL

请复制整个配置输出目录，而不是只复制 EXE。每个配置目录已包含需要的 FFmpeg DLL；不要从 Debug 目录复制 DLL 到 Release。

## 自动化验证

准备 Python 虚拟环境和 FFmpeg SDK 后运行完整 Debug/Release 验证：

```powershell
.\scripts\test-all.ps1
```

也可用 `-Configurations Debug` 或 `-Configurations Release` 单独执行。脚本构建应用、运行媒体分辨率回归、USB/UVC 协议及生命周期测试，并运行 Python 单元和 Python 到 C++ 控制管道联调。`test-resolution.ps1` 还需 FFmpeg SDK 中有 `ffmpeg.exe` 和 `libx264` 编码器。

# 维护与交接

## 修改项目时

- 保持解决方案平台为 `x64`；用户态程序、FFmpeg SDK 和目标 Windows 架构必须一致。
- 把进程/摄像头生命周期留在 `src/app/`，把新媒体输入和帧转换放入 `src/media/`，不要再把所有处理塞回 `main.cpp`。
- 协议结构、GUID、IOCTL 和 UVC 描述符均为运行契约。调整前先找驱动端对应定义；当前仓库没有总线驱动完整源码。
- 如修改帧输入格式，同步更新 `src/media/frame_source.cpp`、发送脚本、`ReceiveTest.py` 和[输入协议文档](usage.md)。
- 如增加 FFmpeg 库，更新 `.vcxproj` 链接项、构建前校验、运行时 DLL 复制列表及构建文档。
- 不提交 `.deps`、`.venv`、`build`、`x64`、`.vs` 或用户特定的 `.vcxproj.user` 文件。
- Python OBS 凭据通过环境变量提供，不把凭据、测试视频或个人 SDK 路径提交到仓库。

## 构建与发布检查

1. 在干净的 x64 Windows 开发环境准备 Visual Studio、Windows SDK 和 FFmpeg shared SDK。
2. 用 `scripts/build.ps1` 分别构建 Debug 和 Release。
3. 检查两个输出目录都含 EXE、五类 FFmpeg DLL 和 `default_res.txt`。
4. 在启动目录不是 EXE 目录时启动程序，确认配置文件和媒体路径的解析符合文档。
5. 在安装了兼容驱动的独立测试机验证设备枚举、UVC 协商、MP4 输入、管道输入和双实例；仅编译成功不代表内核设备可用。
6. 发布时提供完整的单一配置输出目录，并附 FFmpeg 来源/版本及应用许可说明。

## 当前验证边界

本次重整在 Windows x64 环境成功编译过 Debug 和 Release。启动冒烟检查可验证 FFmpeg DLL 加载及配置文件定位；当前机器没有已安装的虚拟 USB 总线接口，因此无法验证真实摄像头设备、UVC 协商和摄像头客户端兼容性。驱动来源和 INF 复制规则列在[驱动包与来源记录](driver.md)。

## 无驱动的分辨率回归测试

`tests/frame_source_resolution_tests.cpp` 直接调用真实帧源回调，覆盖 480p、720p、1080p 的九种输入/输出组合（MP4 和命名管道各一组）、RGB 管道的奇数输入宽高、视频流中途改变尺寸、MP4 循环播放、非法分辨率和过短输出缓冲区。测试检查完整画面的 YUY2 颜色和缓冲区前后的保护字节。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-resolution.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-resolution.ps1 -Configuration Release
```

测试需要 Visual Studio C++ 工具链及包含 `ffmpeg.exe` 和 `libx264` 编码器的 shared SDK，生成的夹具和测试程序存放在 `build/tests/resolution/`。运行时应关闭其他占用 `vcam_pipe` 的程序。此测试不加载内核驱动；涉及描述符或驱动接口的更改，仍需要在有兼容驱动的 Windows 机器上做端到端验证。

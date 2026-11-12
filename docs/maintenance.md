# 维护与交接

## 开发约定

- 用户态程序、FFmpeg SDK 和目标平台保持 x64；C++ 使用项目指定的 C++17 工具集。
- 进程与摄像头生命周期放在 `src/app/`、`src/camera/`；媒体解码和输入放在 `src/media/`；USB 驱动适配放在 `src/usb/`。
- 工作线程停止必须有可等待的取消路径；不要使用强制终止线程或脱离生命周期管理的线程。
- Win32 句柄使用 RAII。重叠 I/O 在操作完成或取消完成前，不释放 OVERLAPPED 结构、事件和缓冲区。
- 协议字段、GUID、IOCTL 和 `src/usb/ioctl.h` 布局属于运行契约。协议调整应增加无硬件测试，并在有兼容驱动的机器上做端到端验证。
- 修改帧协议时同步更新 C++ 读取端、Python 发送/接收端、协议文档和回归测试。
- 新增 FFmpeg 依赖时更新项目链接项、构建脚本、运行时 DLL 复制和文档。
- 不提交 `.deps`、`.venv`、`build`、`x64`、`.vs` 或用户特定的 `.vcxproj.user` 文件。凭据不进入仓库。

## 验证

准备 Visual Studio C++ 工具链、Windows SDK、FFmpeg shared SDK 和 Python 环境后运行：

```powershell
.\scripts\test-all.ps1
```

脚本对 Debug 和 Release 分别构建并运行：

- MP4/管道输入输出分辨率矩阵、动态视频尺寸、缓冲区边界和重复停止测试。
- 描述符长度截断、UVC 协商、ISO 包边界、20,000 条确定性畸形请求、假 USB 传输生命周期、热更新回滚、管道重连和配置文件读写。
- Python 配置、OBS 捕获模拟、分片 TCP、命名管道取消，以及 Python 客户端到 C++ 控制服务/Web UI 的 HTTP 授权、预览和设置联调。

单独运行脚本可限定配置：`scripts/test-resolution.ps1 -Configuration Release`、`scripts/test-usb.ps1 -Configuration Debug`。后者不需要 FFmpeg。

## 验证范围

自动测试使用真实用户态帧处理、协议实现、Win32 命名管道和可注入的假 USB 传输；不安装或模拟内核驱动，不覆盖真实设备枚举、真实 UVC 客户端重开行为、OBS 服务端或 Hyper-V 网络。发布前须在安装了兼容驱动的测试机验证设备枚举、协商、双摄像头和热更新后的客户端重连。驱动包来源和限制见[驱动文档](driver.md)。

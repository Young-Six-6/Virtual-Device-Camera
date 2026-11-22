# 维护与交接

## 开发约定

- 用户态程序、FFmpeg SDK 和目标平台保持 x64；C++ 使用项目指定的 C++17 工具集。
- 进程与摄像头生命周期放在 `src/app/`、`src/camera/`；媒体解码和输入放在 `src/media/`；USB 驱动适配放在 `src/usb/`。
- 工作线程停止必须有可等待的取消路径；不要使用强制终止线程或脱离生命周期管理的线程。
- `UsbTransport::begin()` 返回的事务缓冲区只在 `end()` 调用前有效；调用 `end()` 后只使用预先复制的结果，不再读取事务指针。
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
- 高频噪声 720p MJPEG 编码、USB 2.0 30fps 帧预算、输出缓冲区护栏和 FFmpeg 解码校验。
- USB 2.0/3.0 UVC 描述符、BOS/端点 companion descriptor、UVC 协商、ISO 包边界、20,000 条确定性畸形请求、USB responder 的 MS feature 映射、bulk/interrupt IN/OUT、control `wLength` 与响应容量校验、假 USB 传输生命周期、意外 worker 退出后的重新启动/重配置、热更新回滚、管道重连和配置文件读写。SuperSpeed 自动化检查不代替 Windows PnP/DirectShow 实机抓帧验证。
- Python 配置、OBS 捕获模拟、分片 TCP、命名管道取消，以及 Python 客户端到 C++ 控制服务/Web UI 的 HTTP 授权、预览和设置联调。

单独运行脚本可限定配置：`scripts/test-resolution.ps1 -Configuration Release`、`scripts/test-usb.ps1 -Configuration Debug`。后者不需要 FFmpeg。

## 验证范围

自动测试使用真实用户态帧处理、协议实现、Win32 命名管道和可注入的假 USB 传输；它不安装或模拟内核驱动，也不覆盖 OBS 服务端或 Hyper-V 网络。目标机当前运行本机测试签名的 HCD `2.0.0.49` 和 USB-tree `1.0.0.25`。Release 摄像头程序以 720p YUY2/30fps 抓取 60 帧，用时 1.99 秒；应用有序退出后摄像头 PDO 消失，挂起事务和移除工作项均为 0。USB2/USB3 描述符、UVC 协商、20,000 条畸形协议请求、热更新与回滚、控制管道、异常 callback 后 END 收口均通过 Release 检查。低/全速 HID 和 WinUSB bulk IN/OUT 合成设备探针此前在 `.48/.24` 上通过；KMDF 测试客户端获得有效 Microsoft OS 1.0 描述符，Windows USB 栈将该请求转成 vendor-control 传输后由 HCD 响应。两次 `0x144/3` 蓝屏的转储调用栈定位到测试客户端坏 URB 路径；事故发生时运行的是 HCD `.39` 与 tree `.20`。截至本轮 `.49/.25` 安装与抓帧后，机器没有重启，服务仍运行，HCD pending 数归零。SuperSpeed 数据为软件虚拟总线上的运行结果，不是物理 USB 3 控制器测速。驱动包来源和限制见[驱动文档](driver.md)。

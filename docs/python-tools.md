# Python 工具

所有工具面向 Windows x64 和 Python 3.11。依赖版本固定在 `tools/python/requirements.txt`；使用项目虚拟环境运行：

```powershell
.\.venv\Scripts\python.exe .\tools\python\ColorTest.py
```

| 文件 | 用途 | 运行位置 |
|---|---|---|
| `ColorTest.py` | 以 10 FPS 发送纯绿色 RGB24 帧 | 与 C++ 程序同一 Windows 主机 |
| `ObsLocalPine.py` | 截取 OBS 场景并通过本机命名管道发送 | 与 C++ 程序同一 Windows 主机 |
| `ReceiveTest.py` | 创建命名管道、接收 RGB24 帧并用 OpenCV 预览 | 同一 Windows 主机；不启动 C++ 程序 |
| `WebVideoSend.py` | 从 OBS 截图并经 TCP 发送 RGB24 帧 | 宿主机 |
| `WebVideoReceive.py` | 监听 TCP 并把帧转发到本机命名管道 | Hyper-V 虚拟机 |

## OBS 配置

OBS WebSocket 服务默认使用 `localhost:4458`，场景名为 `test1`。在 OBS 中启用 WebSocket 服务并按需修改脚本顶部的 `OBS_HOST`、`OBS_PORT`、`SCENE_NAME` 和帧率。若 OBS 设置了密码，在 PowerShell 中设置环境变量：

```powershell
$env:OBS_WEBSOCKET_PASSWORD = '填入 OBS WebSocket 密码'
.\.venv\Scripts\python.exe .\tools\python\ObsLocalPine.py
```

环境变量只对当前 PowerShell 进程及其子进程生效。脚本不再把密码写入源文件。

`ObsLocalPine.py` 从 OBS 返回的场景尺寸获取输入分辨率，失败时使用 1920×1080。`WebVideoSend.py` 默认发送 1280×720；要改分辨率、场景或纯色测试模式，编辑脚本顶部常量。

## Hyper-V TCP 转发

1. 在虚拟机内启动 `WebVideoReceive.py`，确认 TCP 5008 入站流量允许来自宿主机。
2. 在宿主机启动 `WebVideoSend.py`。默认 `IfHyper = True`，会用 Hyper-V 查询虚拟机 `camera` 的 IPv4；必要时修改 `VM_NAME` 或 `MANUAL_IP`。
3. 默认由 OBS 提供图像。设置 `TEST_SOLID_COLOR = True` 可发送纯绿色测试画面。

`IfHyper = True` 时，脚本请求管理员权限以查询 Hyper-V 网络适配器地址。TCP 接收端没有认证和加密，务必使用私有网络并设置范围受限的防火墙规则。

## 帧协议和运行边界

本机命名管道与 TCP 都使用同一协议：一次 `<ii` little-endian 头部（宽、高），随后是不带逐帧头的 RGB24 数据。TCP 是字节流，接收端会循环读取，直到取满一个头或一帧。断开后需重新启动接收端和 C++ 管道服务程序建立新会话。

`ReceiveTest.py` 通过 OpenCV 显示窗口预览，按 Esc 可关闭窗口。一个时刻只能有一个服务进程创建并占用 `\\.\pipe\vcam_pipe`。

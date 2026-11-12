# Python 工具

工具面向 Windows x64 和 Python 3.11。依赖版本见 `tools/python/requirements.txt`；推荐使用仓库虚拟环境：

```powershell
.\.venv\Scripts\python.exe .\tools\python\ColorTest.py
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py --help
```

| 入口 | 用途 |
|---|---|
| `ColorTest.py` | 发送纯绿色 RGB24 帧 |
| `ObsLocalPine.py` | 从 OBS 截图并通过本机 RGB 管道发送 |
| `ReceiveTest.py` | 接收 RGB 管道并显示预览 |
| `WebVideoSend.py` | 从 OBS 或纯色源取图，经 TCP 发送 |
| `WebVideoReceive.py` | 接收 TCP 帧并转发到本机 RGB 管道 |
| `CameraControl.py` | 查询状态或发送分辨率热更新命令 |

各入口的 `--help` 列出可用参数。配置采用以下优先级：命令行参数覆盖 `VCAM_*` 环境变量，环境变量覆盖 `config/tools.toml`，最后使用内置默认值。配置文件按脚本所在位置向上查找仓库目录，也可用 `VCAM_CONFIG` 指定其他 TOML 文件。`OBS_WEBSOCKET_PASSWORD` 只从环境变量读取，不写入 TOML。

例如，在应用运行时将摄像头 1 切换为 720p：

```powershell
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py resolution --camera 1 --resolution 720
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py resolution --camera 1 --resolution 1024x768
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py status --camera 1
```

控制命令和重枚举行为见[控制管道协议](control-protocol.md)。

## OBS 和本机管道

OBS WebSocket 默认地址为 `localhost:4458`，场景名为 `test1`。启用 OBS WebSocket 服务并在需要时提供密码：

```powershell
$env:OBS_WEBSOCKET_PASSWORD = 'OBS WebSocket 密码'
.\.venv\Scripts\python.exe .\tools\python\ObsLocalPine.py
```

`ObsLocalPine.py` 默认从 OBS 场景获取画布尺寸。显式指定 `--width` 和 `--height` 可关闭自动尺寸；也可使用 `--auto-size` 恢复自动尺寸。`ColorTest.py` 默认发送绿色画面，`ObsLocalPine.py` 默认使用红色纯色源作为 OBS 不可用时的配置选项。发送端与摄像头输出尺寸可不同，应用会缩放。

## Hyper-V/TCP

1. 在虚拟机内运行 `WebVideoReceive.py`，允许宿主机通过防火墙访问 TCP 5008。
2. 在宿主机运行 `WebVideoSend.py`。默认查询 Hyper-V 虚拟机 `camera` 的 IPv4；若自动发现不可用，可传 `--no-hyper-v --tcp-host <虚拟机IP>`。
3. 发送端默认从 OBS 截图，也可用 `--solid-color` 发送纯色测试帧。

Hyper-V 地址发现默认尝试读取本机 Hyper-V 信息，不会自动弹出管理员授权。需要提升权限时可显式传 `--elevate`。TCP 接收默认绑定所有网卡且没有认证/加密；只应在可信私有网络使用，并限制防火墙规则。

本机管道格式为一次 `<ii` little-endian 宽高头，之后是固定尺寸 RGB24 帧。TCP 是字节流，接收端会循环读取以处理分片。客户端断开后，C++ 输入服务保留并重新打开命名管道；发送脚本也会在限定时间内重试。

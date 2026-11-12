# 控制管道协议

应用默认在 `\\.\pipe\vcam_control` 创建本机命名管道。可通过 `--control-pipe PATH` 更改名称。此管道与 RGB24 输入管道相互独立，不会占用视频帧通道。

客户端以 UTF-8/ASCII 文本按行发送命令；每条命令正文最多 255 字节，后跟 LF，CR 会被忽略。服务端按连接顺序逐条处理，响应也是一行文本并以 LF 结束。每次处理一个本地客户端，断开后接受下一个连接。

## 命令

| 命令 | 说明 |
|---|---|
| `STATUS <camera>` | 查询摄像头编号、当前输出尺寸和运行状态 |
| `RESOLUTION <camera> <width> <height>` | 热更新输出分辨率 |

摄像头编号从 1 开始。允许偶数宽度 2–4096、高度 1–2160。多余参数、非法数字和不支持的命令会返回错误。

## 响应

```text
OK STATUS 1 1920 1080 RUNNING
OK RESOLUTION 1 1280 720 REOPEN_REQUIRED
ERR invalid-resolution
ERR camera-not-running
ERR unplug-failed-camera-stopped
ERR resize-failed-restored-previous
ERR resize-and-rollback-failed
```

`REOPEN_REQUIRED` 表示 UVC 设备已按新描述符重新枚举，摄像头使用方需要关闭旧句柄并重新打开设备。输入文件、应用进程和 RGB24 输入管道不会因为输出分辨率修改而重启。更新失败时，错误码会说明是否恢复了旧设备；`STATUS` 可确认最终状态。

工具调用示例：

```powershell
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py status --camera 1
.\.venv\Scripts\python.exe .\tools\python\CameraControl.py resolution --camera 1 --resolution 1280x720
```

控制管道拒绝远程命名管道客户端。默认 ACL 允许 Everyone、对象所有者和 SYSTEM；Web UI 可切换为对象所有者或本机 BUILTIN Users 组。协议没有额外的应用层身份验证。通过控制管道修改分辨率只作用于当前进程；Web UI 修改的设置会写回 `camera.ini`。

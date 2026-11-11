# 运行与输入协议

## 摄像头分辨率

编辑 `config/default_res.txt` 中的值：

| 值 | 输出分辨率 |
|---|---|
| `480` | 640×480 |
| `720` | 1280×720 |
| `1080` | 1920×1080 |

构建会复制此文件到输出目录。运行时读取 EXE 旁的 `default_res.txt`，所以改好输出目录中的副本后无需重新编译；重新构建会再次覆盖该副本。配置在创建摄像头时读取，修改后需重启程序，并让摄像头客户端重新打开设备。该程序只向 UVC 主机公布这一种分辨率。

MP4、OBS 和管道的输入分辨率可以不同于此配置。例如，摄像头设置为 720p、发送端仍为 1080p 时，会先把输入缩放到 1280×720，再写入对应大小的 YUY2 缓冲区；输入尺寸不会改变 UVC 公布的尺寸。宽高比不同时会拉伸画面。

## MP4 输入

在输出目录运行程序并给出一个或两个 MP4 路径：

```powershell
.\virtual-device-camera.exe 'D:\Videos\front.mp4'
.\virtual-device-camera.exe 'D:\Videos\front.mp4' 'D:\Videos\rear.mp4'
```

第一个文件作为摄像头 1 的输入；第二个文件存在时创建摄像头 2。视频经 FFmpeg 解码、缩放为配置分辨率、转为 YUY2，再通过 UVC 等时传输发送。相对路径相对于 EXE 目录。文件读完后默认从头循环播放。

按任意键退出。程序运行前需有可用的虚拟 USB 总线接口；仅构建 EXE 不会在 Windows 中注册摄像头。

## RGB24 命名管道输入

无参数启动时，程序为一个摄像头等待 `\\.\pipe\vcam_pipe` 客户端：

```powershell
.\virtual-device-camera.exe
```

客户端连接后发送一次 8 字节 little-endian 头部，再连续发送未压缩 RGB24 帧：

| 字段 | 编码 | 说明 |
|---|---|---|
| `width` | 有符号 32 位整数，小端 | 输入帧宽度，1–4096 |
| `height` | 有符号 32 位整数，小端 | 输入帧高度，1–2160 |
| 像素数据 | 每像素 R、G、B 各 1 字节 | 每帧恰为 `width × height × 3` 字节 |

宽高头只发送一次；后续每帧不再带头、长度、时间戳或填充。输入帧由程序缩放到摄像头分辨率，再转换为 YUY2。输入帧最大 30 MiB。一个进程启动期间只接受一个管道连接；客户端断开后需重启应用再连接。

同一管道连接的输入尺寸必须与最初的宽高头保持一致。发送端切换分辨率时，应重启接收程序并重新连接、发送新头部。视频文件解码过程中发生的尺寸变化则由 FFmpeg 帧尺寸驱动重新配置缩放器，UVC 输出尺寸保持固定。

示例：

```powershell
.\.venv\Scripts\python.exe .\tools\python\ColorTest.py
.\.venv\Scripts\python.exe .\tools\python\ObsLocalPine.py
```

`ColorTest.py` 发送纯绿色 1920×1080 帧。`ObsLocalPine.py` 从 OBS 获取场景截图。发送端分辨率可与 UVC 输出分辨率不同，应用会进行缩放。

## Hyper-V/TCP 转发

在 Hyper-V 虚拟机中运行 `WebVideoReceive.py`，宿主机运行 `WebVideoSend.py`。接收端监听 TCP `0.0.0.0:5008`，先收相同的 8 字节分辨率头，再接收固定长度 RGB24 帧并转发给命名管道。发送端默认查找名为 `camera` 的 Hyper-V 虚拟机，并从宿主机的 OBS 场景取图。

TCP 传输没有身份验证或加密。只在受信任的私有网络中使用，并限制 Windows 防火墙入站规则；不要将 5008 端口暴露到公网。

## 预览

`ReceiveTest.py` 自己创建 `\\.\pipe\vcam_pipe` 并显示接收到的 RGB24 帧，可用于验证发送端与帧协议；它不能与 `virtual-device-camera.exe` 同时占用同一个管道名。

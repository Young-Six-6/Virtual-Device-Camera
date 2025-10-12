# Virtual-Device-Camera
 虚拟驱动摄像头 虚拟驱动摄像头 Virtual driving camera
fork自jasonsalex/Virtual-Device-Camera，意图做些修改

2025/10/10
增加了mp4作为输入源

2025/10/12
增加了通过\\.\pipe\vcam_pipe传输画面，此后无错误将不再大修改，靠外部输入画面

缺失dll自行补齐BtbN/FFmpeg-Builds

1. 下载 FFmpeg 预构建版本（推荐 BtbN/FFmpeg-Builds 的 win64-gpl-shared 版本）。
2. 解压后，将 `bin` 目录下的 `avformat-xx.dll`、`avcodec-xx.dll`、`swscale-xx.dll`、`avutil-xx.dll` 复制到项目的 `x64/Debug` 目录。
3. 配置项目的 FFmpeg 头文件和库文件路径



注意：警告不要用在欺诈性活动，后果自负！！！
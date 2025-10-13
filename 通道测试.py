import os
import struct

# 连接到命名管道
pipe = open(r'\\.\pipe\vcam_pipe', 'wb')

# 发送 1920x1080 的 RGB 数据
width, height = 1920, 1080
pipe.write(struct.pack('<ii', width, height))  # 发送宽高
red_data = b'\x00\x00\x00' * (width * height)  # 画面
while True:
    pipe.write(red_data)
    pipe.flush()
    # 可添加延迟控制帧率
import cv2
import numpy as np
import win32pipe
import win32file
import pywintypes
import threading
from queue import Queue
import struct  # 确保导入struct

# 全局配置
FRAME_QUEUE_SIZE = 5  # 队列大小，避免数据堆积
PIPE_NAME = r'\\.\pipe\vcam_pipe'
WINDOW_NAME = "OBS Preview"

# 线程安全队列与停止信号
frame_queue = Queue(maxsize=FRAME_QUEUE_SIZE)
stop_event = threading.Event()

def display_frames():
    """显示线程：处理图像渲染，确保颜色正确"""
    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(WINDOW_NAME, 1280, 720)  # 固定窗口大小，减少卡顿

    while not stop_event.is_set():
        try:
            # 从队列获取帧数据（超时1秒，避免阻塞）
            width, height, rgb_data = frame_queue.get(timeout=1)

            # 1. 验证数据大小（确保是完整的RGB数据）
            expected_frame_size = width * height * 3
            if len(rgb_data) != expected_frame_size:
                print(f"[接收端警告] 帧数据大小不匹配：实际{len(rgb_data)}字节（期望{expected_frame_size}字节）")
                frame_queue.task_done()
                continue

            # 2. RGB转BGR（OpenCV默认显示格式，解决颜色异常）
            frame_rgb = np.frombuffer(rgb_data, dtype=np.uint8).reshape((height, width, 3))
            frame_bgr = cv2.cvtColor(frame_rgb, cv2.COLOR_RGB2BGR)  # 关键：RGB→BGR转换

            # 3. 显示图像
            cv2.imshow(WINDOW_NAME, frame_bgr)

            # 4. 按ESC键退出
            if cv2.waitKey(1) & 0xFF == 27:
                stop_event.set()
                break

            frame_queue.task_done()  # 标记任务完成

        except Exception as e:
            # 超时或其他错误时继续循环，避免线程退出
            continue

    # 清理资源
    cv2.destroyAllWindows()
    print(f"[接收端] 显示窗口已关闭")

def receive_frames():
    """接收线程：每帧都解析宽高，避免数据错位"""
    # 创建管道（1MB缓冲区，适配1920x1080图像）
    pipe = win32pipe.CreateNamedPipe(
        PIPE_NAME,
        win32pipe.PIPE_ACCESS_DUPLEX,
        win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT,
        1, 1024*1024, 1024*1024,
        0, None
    )

    # 等待发送端连接
    try:
        win32pipe.ConnectNamedPipe(pipe, None)
        print(f"[接收端] 已连接到管道：{PIPE_NAME}")
    except Exception as e:
        print(f"[接收端错误] 管道连接失败：{e}")
        win32file.CloseHandle(pipe)
        stop_event.set()
        return

    # 数据接收缓冲区
    buffer = b""

    try:
        while not stop_event.is_set():
            # 1. 读取管道数据（每次最多读1MB）
            result, data = win32file.ReadFile(pipe, 1024*1024)
            if result != 0 or not data:
                time.sleep(0.001)
                continue

            # 2. 累积数据到缓冲区
            buffer += data
            print(f"[接收端] 累计缓冲区大小：{len(buffer)}字节")

            # 3. 循环解析帧（每帧=8字节宽高 + N字节RGB数据）
            while len(buffer) >= 8:  # 先确保有宽高数据（8字节）
                # 解析当前帧的宽高
                width, height = struct.unpack('<ii', buffer[:8])
                # 计算当前帧的RGB数据大小（3字节/像素）
                frame_size = width * height * 3

                # 检查缓冲区是否有完整的一帧数据
                if len(buffer) < 8 + frame_size:
                    print(f"[接收端] 缓冲区数据不足（需{8+frame_size}字节，当前{len(buffer)}字节），等待更多数据")
                    break  # 数据不够，退出循环等待下一次接收

                # 提取当前帧的RGB数据（跳过前8字节宽高）
                frame_rgb_data = buffer[8 : 8 + frame_size]
                # 剩余数据留到下一次解析（避免影响下一帧）
                buffer = buffer[8 + frame_size : ]

                print(f"[接收端] 解析到一帧：{width}x{height}（数据大小：{len(frame_rgb_data)}字节）")

                # 4. 将帧数据放入队列（避免阻塞接收）
                if not frame_queue.full():
                    frame_queue.put((width, height, frame_rgb_data))
                    print(f"[接收端] 帧已加入队列（当前队列长度：{frame_queue.qsize()}）")
                else:
                    print(f"[接收端警告] 队列已满，丢弃当前帧（避免卡顿）")

    except Exception as e:
        print(f"[接收端致命错误] {e}")
    finally:
        # 释放资源
        win32file.CloseHandle(pipe)
        stop_event.set()
        print(f"[接收端] 管道已关闭")

if __name__ == "__main__":
    import time  # 确保导入time

    # 启动接收线程和显示线程
    receive_thread = threading.Thread(target=receive_frames, daemon=True)
    display_thread = threading.Thread(target=display_frames, daemon=True)

    receive_thread.start()
    display_thread.start()

    # 主线程等待停止信号
    try:
        while not stop_event.is_set():
            time.sleep(1)
    except KeyboardInterrupt:
        print(f"\n[接收端] 用户手动中断")
        stop_event.set()

    # 等待线程结束
    receive_thread.join()
    frame_queue.join()  # 等待队列中所有帧处理完成
    display_thread.join()
    print(f"[接收端] 所有资源已释放")
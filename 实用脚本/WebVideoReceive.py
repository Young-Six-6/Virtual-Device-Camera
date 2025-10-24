import socket
import struct
import threading
import queue
import time

PIPE_PATH = r'\\.\pipe\vcam_pipe'
HOST = '0.0.0.0'
PORT = 5008
BUFFER_SIZE = 4096
FLUSH_INTERVAL = 3     # 每隔多少帧 flush 一次

def writer_thread(pipe, q: queue.Queue):
    """独立线程写入虚拟摄像头管道"""
    frame_count = 0
    while True:
        buf = q.get()
        if buf is None:
            break
        pipe.write(buf)
        frame_count += 1
        if frame_count % FLUSH_INTERVAL == 0:
            pipe.flush()
        q.task_done()

def main():
    print(f"[🔌] 等待宿主连接 {HOST}:{PORT} ...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, PORT))
    s.listen(1)
    conn, addr = s.accept()
    print(f"[✅] 已连接宿主：{addr}")

    # 打开虚拟摄像头管道
    pipe = open(PIPE_PATH, 'wb')
    print(f"[✅] 已连接虚拟摄像头管道：{PIPE_PATH}")

    # 接收分辨率头
    hdr = conn.recv(8)
    width, height = struct.unpack('<ii', hdr)
    frame_size = width * height * 3
    print(f"[📐] 分辨率 {width}x{height}, 帧大小 {frame_size} 字节")

    pipe.write(struct.pack('<ii', width, height))
    pipe.flush()

    q = queue.Queue(maxsize=5)
    t = threading.Thread(target=writer_thread, args=(pipe, q), daemon=True)
    t.start()

    last_time = time.perf_counter()
    frames = 0

    try:
        while True:
            buf = b''
            while len(buf) < frame_size:
                chunk = conn.recv(frame_size - len(buf))
                if not chunk:
                    raise ConnectionError("连接中断")
                buf += chunk

            # 把帧交给后台写线程
            try:
                q.put_nowait(buf)
            except queue.Full:
                print("[⚠️] 写入线程忙，丢弃一帧")
                continue

            frames += 1
            now = time.perf_counter()
            if now - last_time >= 5:
                print(f"[📊] 实际接收速率：{frames / (now - last_time):.2f} FPS")
                last_time = now
                frames = 0

    except KeyboardInterrupt:
        print("[⏹️] 手动中止")
    finally:
        q.put(None)
        q.join()
        conn.close()
        s.close()
        pipe.close()
        print("[🔚] 结束")

if __name__ == "__main__":
    main()

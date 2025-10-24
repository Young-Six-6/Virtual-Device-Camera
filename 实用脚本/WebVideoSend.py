import os
import struct
import time
import base64
import numpy as np
import cv2
import socket
import subprocess
import re
import ctypes
import sys
from obswebsocket import obsws, requests
from obswebsocket.exceptions import ConnectionFailure

# -------------------------- 配置项 --------------------------
IfHyper = True
VM_NAME = "camera"            # Hyper-V 虚拟机名称
MANUAL_IP = "192.168.100.2"   # 如果自动检测失败
PORT = 5008

OBS_HOST = "localhost"
OBS_PORT = 4458
OBS_PASSWORD = "DbmlVsxpORqLfdXH"
SCENE_NAME = "test1"
FRAME_RATE = 10
TEST_SOLID_COLOR = False  # True=纯色测试，False=OBS截图
DEFAULT_WIDTH = 1280
DEFAULT_HEIGHT = 720

# -------------------------- 工具函数 --------------------------
def ensure_admin():
    if not ctypes.windll.shell32.IsUserAnAdmin():
        print("[⚠️] 本脚本需要管理员权限以访问 Hyper-V 命令，正在请求UAC权限...")
        ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable, " ".join(sys.argv), None, 1
        )
        sys.exit()

def get_vm_ip(vm_name: str):
    try:
        ps_command = [
            "powershell",
            "-Command",
            f"Get-VMNetworkAdapter -VMName '{vm_name}' | Select-Object -ExpandProperty IPAddresses"
        ]
        result = subprocess.run(ps_command, capture_output=True, text=True, timeout=5)
        output = result.stdout.strip()
        if not output:
            return None
        ips = re.findall(r"\b\d{1,3}(?:\.\d{1,3}){3}\b", output)
        if ips:
            ip = ips[0]
            print(f"[✅] 自动检测到虚拟机 {vm_name} IP：{ip}")
            return ip
        print(f"[⚠️] 未找到 IPv4，输出：\n{output}")
        return None
    except Exception as e:
        print(f"[❌] 获取虚拟机 IP 失败：{e}")
        return None

# -------------------------- 主逻辑 --------------------------
def main():
    TARGET_WIDTH = DEFAULT_WIDTH
    TARGET_HEIGHT = DEFAULT_HEIGHT
    EXPECTED_SIZE = TARGET_WIDTH * TARGET_HEIGHT * 3

    if IfHyper:
        ensure_admin()
        ip = get_vm_ip(VM_NAME) or MANUAL_IP
    else:
        ip = MANUAL_IP

    print(f"[🔌] 正在连接虚拟机 {ip}:{PORT} ...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
    try:
        sock.connect((ip, PORT))
        print("[✅] 已建立 TCP 连接")
    except Exception as e:
        print(f"[❌] 无法连接虚拟机：{e}")
        return

    # 初始化 OBS
    ws = None
    if not TEST_SOLID_COLOR:
        try:
            ws = obsws(OBS_HOST, OBS_PORT, OBS_PASSWORD)
            ws.connect()
            print(f"[✅] 已连接 OBS WebSocket ({OBS_HOST}:{OBS_PORT})")
        except ConnectionFailure:
            print("[❌] 无法连接 OBS WebSocket，请确认 obs-websocket 插件已启用。")
            return

    # 发送分辨率头
    sock.sendall(struct.pack('<ii', TARGET_WIDTH, TARGET_HEIGHT))
    print(f"[📐] 已发送分辨率头：{TARGET_WIDTH}x{TARGET_HEIGHT}")

    # 发送循环
    frame_count = 0
    start_stat = time.perf_counter()
    next_time = time.perf_counter()
    interval = 1.0 / FRAME_RATE

    print(f"[🎥] 开始推流：{FRAME_RATE} FPS | 场景：{SCENE_NAME}")

    try:
        while True:
            frame_count += 1
            frame_rgb = None

            if TEST_SOLID_COLOR:
                frame_rgb = np.zeros((TARGET_HEIGHT, TARGET_WIDTH, 3), np.uint8)
                frame_rgb[:, :, 1] = 255  # 绿色测试帧
            else:
                try:
                    resp = ws.call(requests.GetSourceScreenshot(
                        sourceName=SCENE_NAME,
                        imageFormat="jpeg",
                        width=TARGET_WIDTH,
                        height=TARGET_HEIGHT,
                        quality=80
                    ))
                    img_b64 = resp.datain.get("imageData", "")
                    if not img_b64.startswith("data:image/jpeg;base64,"):
                        print(f"[⚠️] 第{frame_count}帧：OBS返回无效截图")
                        continue
                    img_bytes = base64.b64decode(img_b64.split(",", 1)[1])
                    frame_bgr = cv2.imdecode(np.frombuffer(img_bytes, np.uint8), cv2.IMREAD_COLOR)
                    frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
                except Exception as e:
                    print(f"[⚠️] 第{frame_count}帧获取失败：{e}")
                    continue

            # 校验大小
            if frame_rgb is None or frame_rgb.nbytes != EXPECTED_SIZE:
                continue

            # 发送帧
            sock.sendall(frame_rgb.tobytes())

            # 控制帧率
            next_time += interval
            sleep_time = next_time - time.perf_counter()
            if sleep_time > 0:
                time.sleep(sleep_time)

            # 打印统计
            now = time.perf_counter()
            if now - start_stat >= 5:
                print(f"[📊] 实际发送速率：{frame_count / (now - start_stat):.2f} FPS")
                start_stat = now
                frame_count = 0

    except KeyboardInterrupt:
        print("[⏹️] 手动中止")
    finally:
        sock.close()
        if ws:
            ws.disconnect()
        print("[🔚] 推流结束")

if __name__ == "__main__":
    main()

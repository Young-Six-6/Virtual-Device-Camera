import os
import struct
import time
import base64
import numpy as np
import cv2
from obswebsocket import obsws, requests
from obswebsocket.exceptions import ConnectionFailure

# -------------------------- 配置项（必须根据需求调整） --------------------------
OBS_HOST = "localhost"
OBS_PORT = 4458  # 与obs端口一致
OBS_PASSWORD = "DbmlVsxpORqLfdXH"
PIPE_PATH = r'\\.\pipe\vcam_pipe'  # 与接收端C++管道名保持一致
SCENE_NAME = "test1"  # OBS中实际场景名称
FRAME_RATE = 10       # 建议5-15fps
TEST_SOLID_COLOR = False  # True=发送红色测试帧，False=发送OBS截图
DEFAULT_WIDTH = 1920
DEFAULT_HEIGHT = 1080

# -------------------------- 工具函数：从OBS获取场景分辨率 --------------------------
def get_obs_scene_resolution(ws, target_scene):
    """从OBS获取指定场景的实际分辨率"""
    try:
        scene_list_resp = ws.call(requests.GetSceneList())
        if "scenes" not in scene_list_resp.datain:
            print(f"[错误] OBS返回无scenes字段：{scene_list_resp.datain}")
            return None, None
        
        for scene in scene_list_resp.datain["scenes"]:
            if scene.get("name") == target_scene:
                w = scene.get("width")
                h = scene.get("height")
                if w and h:
                    return w, h
                else:
                    print(f"[错误] 场景{target_scene}无分辨率信息")
                    return None, None
        print(f"[错误] OBS中未找到场景：{target_scene}")
        return None, None
    except Exception as e:
        print(f"[错误] 获取OBS场景分辨率失败：{str(e)}")
        return None, None

# -------------------------- 主程序 --------------------------
def main():
    global TARGET_WIDTH, TARGET_HEIGHT, EXPECTED_SIZE
    TARGET_WIDTH = DEFAULT_WIDTH
    TARGET_HEIGHT = DEFAULT_HEIGHT
    EXPECTED_SIZE = TARGET_WIDTH * TARGET_HEIGHT * 3

    # 1️⃣ 连接OBS WebSocket
    ws = None
    if not TEST_SOLID_COLOR:
        try:
            ws = obsws(OBS_HOST, OBS_PORT, OBS_PASSWORD)
            ws.connect()
            print(f"[成功] 已连接OBS WebSocket ({OBS_HOST}:{OBS_PORT})")

            scene_w, scene_h = get_obs_scene_resolution(ws, SCENE_NAME)
            if scene_w and scene_h:
                TARGET_WIDTH, TARGET_HEIGHT = scene_w, scene_h
                EXPECTED_SIZE = TARGET_WIDTH * TARGET_HEIGHT * 3
                print(f"[成功] 场景分辨率：{TARGET_WIDTH}x{TARGET_HEIGHT}")
            else:
                print(f"[警告] 获取分辨率失败，使用默认：{TARGET_WIDTH}x{TARGET_HEIGHT}")
        except ConnectionFailure as e:
            print(f"[致命错误] OBS连接失败：{str(e)}")
            return
        except Exception as e:
            print(f"[致命错误] OBS初始化异常：{str(e)}")
            return

    # 2️⃣ 连接命名管道
    try:
        pipe = open(PIPE_PATH, 'wb')
        print(f"[成功] 已连接到管道：{PIPE_PATH}")
    except FileNotFoundError:
        print("[错误] 管道不存在，请先启动接收端（main.exe）")
        if ws: ws.disconnect()
        return
    except Exception as e:
        print(f"[错误] 打开管道失败：{str(e)}")
        if ws: ws.disconnect()
        return

    # 3️⃣ 发送一次宽高（C++只在首次接收宽高后分配缓冲）
    pipe.write(struct.pack('<ii', TARGET_WIDTH, TARGET_HEIGHT))
    pipe.flush()
    print(f"[成功] 已发送分辨率头：{TARGET_WIDTH}x{TARGET_HEIGHT}")

    # 4️⃣ 主循环：发送帧数据
    frame_count = 0
    print(f"\n[🔄 开始运行] 模式：{'纯色测试帧' if TEST_SOLID_COLOR else 'OBS截图'} | 分辨率：{TARGET_WIDTH}x{TARGET_HEIGHT} | 帧率：{FRAME_RATE}fps")
    try:
        while True:
            start_time = time.time()
            frame_count += 1

            # 获取帧内容
            if TEST_SOLID_COLOR:
                # 红色帧
                frame_bgr = np.zeros((TARGET_HEIGHT, TARGET_WIDTH, 3), dtype=np.uint8)
                frame_bgr[:, :, 2] = 255
                frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
            else:
                try:
                    screenshot_resp = ws.call(requests.GetSourceScreenshot(
                        sourceName=SCENE_NAME,
                        imageFormat="jpeg",
                        width=TARGET_WIDTH,
                        height=TARGET_HEIGHT,
                        quality=80
                    ))
                    img_b64 = screenshot_resp.datain.get("imageData")
                    if not img_b64 or not img_b64.startswith("data:image/jpeg;base64,"):
                        print(f"[错误] 第{frame_count}帧：OBS返回无效截图")
                        time.sleep(0.1)
                        continue

                    img_bytes = base64.b64decode(img_b64.split(",", 1)[1])
                    frame_bgr = cv2.imdecode(np.frombuffer(img_bytes, np.uint8), cv2.IMREAD_COLOR)
                    if frame_bgr is None:
                        print(f"[错误] 第{frame_count}帧：JPEG解码失败")
                        continue
                    frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
                except Exception as e:
                    print(f"[错误] 第{frame_count}帧：OBS截图失败：{str(e)}")
                    time.sleep(0.1)
                    continue

            # 校验帧数据
            if frame_rgb.nbytes != EXPECTED_SIZE:
                print(f"[警告] 第{frame_count}帧：字节数异常（期望{EXPECTED_SIZE}，实际{frame_rgb.nbytes}）")
                continue

            # 写入像素数据（不再重复发送宽高）
            try:
                pipe.write(frame_rgb.tobytes())
                pipe.flush()
                print(f"[成功] 第{frame_count}帧已发送（大小：{frame_rgb.nbytes}字节）")
            except BrokenPipeError:
                print("[错误] 管道断开，接收端可能已退出。")
                break
            except Exception as e:
                print(f"[错误] 第{frame_count}帧发送失败：{str(e)}")
                time.sleep(0.1)
                continue

            # 控制帧率
            elapsed = time.time() - start_time
            sleep_time = max(0, 1 / FRAME_RATE - elapsed)
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        print("\n[手动中断] 用户终止发送。")
    finally:
        # 清理资源
        try:
            pipe.close()
            print("[🔚] 管道已关闭")
        except:
            pass
        if ws and not TEST_SOLID_COLOR:
            ws.disconnect()
            print("[🔚] 已断开OBS连接")
        print(f"[统计] 共发送 {frame_count} 帧，程序退出。")

# -------------------------- 自动安装依赖并启动 --------------------------
if __name__ == "__main__":
    required = {
        "obswebsocket": "obs-websocket-py==0.7.2",
        "cv2": "opencv-python",
        "numpy": "numpy"
    }
    missing = []
    for k, pkg in required.items():
        try:
            __import__(k)
        except ImportError:
            missing.append(pkg)
    if missing:
        print(f"[安装] 缺少依赖：{' '.join(missing)}")
        os.system("pip install --upgrade pip")
        os.system("pip install " + " ".join(missing))
    else:
        main()

"""Preview the application's RGB24 named-pipe stream without a USB driver."""

import queue
import struct
import threading
import time

import cv2
import numpy as np
import pywintypes
import win32file
import win32pipe
import winerror

PIPE_NAME = r"\\.\pipe\vcam_pipe"
WINDOW_NAME = "Virtual Camera Pipe Preview"
MAX_WIDTH = 4096
MAX_HEIGHT = 2160
MAX_FRAME_BYTES = 30 * 1024 * 1024
READ_CHUNK = 1024 * 1024
QUEUE_SIZE = 3

stop_event = threading.Event()
frame_queue: queue.Queue[bytes] = queue.Queue(maxsize=QUEUE_SIZE)


def read_exact(pipe, size: int) -> bytes:
    """Read an exact byte count from a byte-mode Windows named pipe."""
    data = bytearray()
    while len(data) < size:
        requested = min(READ_CHUNK, size - len(data))
        error_code, chunk = win32file.ReadFile(pipe, requested)
        if error_code not in (0, winerror.ERROR_MORE_DATA):
            raise pywintypes.error(error_code, "ReadFile", "Named-pipe read failed")
        if not chunk:
            raise EOFError("The named-pipe client disconnected")
        data.extend(chunk)
    return bytes(data)


def read_resolution(pipe) -> tuple[int, int, int]:
    width, height = struct.unpack("<ii", read_exact(pipe, 8))
    if width <= 0 or width > MAX_WIDTH or height <= 0 or height > MAX_HEIGHT:
        raise ValueError(f"Unsupported frame size: {width}x{height}")
    frame_size = width * height * 3
    if frame_size > MAX_FRAME_BYTES:
        raise ValueError(f"RGB24 frame is too large: {frame_size} bytes")
    return width, height, frame_size


def enqueue_latest(frame: bytes) -> None:
    try:
        frame_queue.put_nowait(frame)
        return
    except queue.Full:
        pass

    # Keep preview latency bounded when image conversion falls behind.
    try:
        frame_queue.get_nowait()
        frame_queue.task_done()
    except queue.Empty:
        pass
    try:
        frame_queue.put_nowait(frame)
    except queue.Full:
        pass


def receive_frames() -> None:
    global frame_dimensions
    pipe = None
    try:
        pipe = win32pipe.CreateNamedPipe(
            PIPE_NAME,
            win32pipe.PIPE_ACCESS_INBOUND,
            win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT,
            1,
            READ_CHUNK,
            READ_CHUNK,
            0,
            None,
        )
        try:
            win32pipe.ConnectNamedPipe(pipe, None)
        except pywintypes.error as exc:
            if exc.winerror != winerror.ERROR_PIPE_CONNECTED:
                raise

        width, height, frame_size = read_resolution(pipe)
        frame_dimensions = (width, height)
        print(f"Connected: {width}x{height} RGB24, {frame_size} bytes per frame.")
        while not stop_event.is_set():
            enqueue_latest(read_exact(pipe, frame_size))
    except (EOFError, OSError, ValueError, pywintypes.error) as exc:
        if not stop_event.is_set():
            print(f"Pipe preview stopped: {exc}")
    finally:
        stop_event.set()
        if pipe is not None:
            win32file.CloseHandle(pipe)


def display_frames() -> None:
    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(WINDOW_NAME, 1280, 720)
    frame_count = 0
    interval_started = time.perf_counter()

    try:
        while not stop_event.is_set() or not frame_queue.empty():
            try:
                rgb_data = frame_queue.get(timeout=0.2)
            except queue.Empty:
                if cv2.waitKey(1) & 0xFF == 27:
                    stop_event.set()
                continue

            try:
                # The header is sent once when the pipe connects; frames are raw RGB24.
                # Resolution is stored by the receiver thread before frame data arrives.
                width, height = frame_dimensions
                rgb_frame = np.frombuffer(rgb_data, dtype=np.uint8).reshape((height, width, 3))
                cv2.imshow(WINDOW_NAME, cv2.cvtColor(rgb_frame, cv2.COLOR_RGB2BGR))
                frame_count += 1
                if cv2.waitKey(1) & 0xFF == 27:
                    stop_event.set()

                now = time.perf_counter()
                if now - interval_started >= 5:
                    print(f"Preview rate: {frame_count / (now - interval_started):.1f} FPS")
                    frame_count = 0
                    interval_started = now
            finally:
                frame_queue.task_done()
    finally:
        cv2.destroyAllWindows()


def main() -> None:
    global frame_dimensions
    frame_dimensions = (0, 0)

    receiver = threading.Thread(target=receive_frames, name="pipe-receiver", daemon=True)
    receiver.start()
    try:
        display_frames()
    except KeyboardInterrupt:
        stop_event.set()
    finally:
        stop_event.set()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

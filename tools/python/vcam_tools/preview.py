import struct
import threading
import time
import cv2
import numpy as np
import pywintypes
import win32con
import win32event
import win32pipe
import winerror
from .protocol import HEADER, frame_size
from .win_io import connect_server, transfer


class PipePreview:
    def __init__(self, path):
        self.stop_event = win32event.CreateEvent(None, True, False, None)
        try:
            self.pipe = win32pipe.CreateNamedPipe(
                path, win32pipe.PIPE_ACCESS_INBOUND | win32con.FILE_FLAG_OVERLAPPED |
                0x00080000,  # FILE_FLAG_FIRST_PIPE_INSTANCE
                win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT | getattr(win32pipe, 'PIPE_REJECT_REMOTE_CLIENTS', 8),
                1, 0, 1024 * 1024, 0, None)
        except BaseException:
            self.stop_event.Close()
            raise
        self.lock = threading.Lock()
        self.latest = None
        self.thread = threading.Thread(target=self._receive, name='pipe-preview')
        try:
            self.thread.start()
        except BaseException:
            self.pipe.Close()
            self.stop_event.Close()
            raise

    def _receive(self):
        while win32event.WaitForSingleObject(self.stop_event, 0) != win32event.WAIT_OBJECT_0:
            try:
                connect_server(self.pipe, self.stop_event)
                width, height = HEADER.unpack(transfer(self.pipe, self.stop_event, HEADER.size))
                size = frame_size(width, height)
                while True:
                    data = transfer(self.pipe, self.stop_event, size)
                    with self.lock:
                        self.latest = width, height, data
            except InterruptedError:
                break
            except (OSError, EOFError, ValueError, pywintypes.error) as error:
                if win32event.WaitForSingleObject(self.stop_event, 0) != win32event.WAIT_OBJECT_0:
                    print(f'Preview client disconnected: {error}')
            finally:
                with self.lock:
                    self.latest = None
                try:
                    win32pipe.DisconnectNamedPipe(self.pipe)
                except pywintypes.error as error:
                    if error.winerror != winerror.ERROR_PIPE_NOT_CONNECTED:
                        raise

    def snapshot(self):
        with self.lock:
            return self.latest

    def close(self):
        if self.stop_event is not None:
            win32event.SetEvent(self.stop_event)
            self.thread.join()
            self.pipe.Close()
            self.stop_event.Close()
            self.stop_event = None


def preview(options):
    receiver = PipePreview(options.pipe)
    name = 'Virtual Camera Pipe Preview'
    shown = 0
    try:
        cv2.namedWindow(name, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(name, 1280, 720)
        while not options.frames or shown < options.frames:
            frame = receiver.snapshot()
            if frame:
                width, height, data = frame
                image = np.frombuffer(data, np.uint8).reshape(height, width, 3)
                cv2.imshow(name, cv2.cvtColor(image, cv2.COLOR_RGB2BGR))
                shown += 1
            if cv2.waitKey(10) & 0xff == 27 or cv2.getWindowProperty(name, cv2.WND_PROP_VISIBLE) < 1:
                break
            time.sleep(0.001)
    finally:
        receiver.close()
        cv2.destroyAllWindows()

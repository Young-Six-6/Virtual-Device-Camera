import time
import pywintypes
import win32con
import win32event
import win32file
import winerror
from .win_io import transfer


def send_command(path: str, command: str, timeout: float = 10) -> str:
    if '\n' in command or '\r' in command or len(command) > 240:
        raise ValueError('Control command must be one short ASCII line')
    deadline = time.monotonic() + timeout
    while True:
        try:
            handle = win32file.CreateFile(path, win32con.GENERIC_READ | win32con.GENERIC_WRITE,
                                          0, None, win32con.OPEN_EXISTING,
                                          win32con.FILE_FLAG_OVERLAPPED, None)
            break
        except pywintypes.error as error:
            if error.winerror not in (winerror.ERROR_FILE_NOT_FOUND, winerror.ERROR_PIPE_BUSY) or time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
    stop = win32event.CreateEvent(None, True, False, None)
    try:
        transfer(handle, stop, (command + '\n').encode('ascii'), write=True, timeout=max(0.001, deadline-time.monotonic()))
        response = bytearray()
        while len(response) < 256:
            byte = transfer(handle, stop, 1, timeout=max(0.001, deadline-time.monotonic()))
            if byte == b'\n':
                return response.decode('ascii')
            response.extend(byte)
        raise ValueError('Camera control response exceeds maximum size')
    finally:
        stop.Close()
        handle.Close()

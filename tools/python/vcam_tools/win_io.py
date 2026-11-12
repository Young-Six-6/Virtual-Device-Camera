"""Overlapped pipe operations with bounded waits and cooperative cancellation."""
import time
import pywintypes
import win32event
import win32file
import win32pipe
import winerror


def _wait(handle, operation, stop, timeout):
    wait = win32event.WaitForMultipleObjects(
        [stop, operation.hEvent], False, win32event.INFINITE if timeout is None else max(1, int(timeout * 1000)))
    if wait != win32event.WAIT_OBJECT_0 + 1:
        try:
            # All operations on this handle are issued by this same worker.
            win32file.CancelIo(handle)
        except pywintypes.error as error:
            if error.winerror != winerror.ERROR_NOT_FOUND:
                raise
        try:
            win32file.GetOverlappedResult(handle, operation, True)
        except pywintypes.error as error:
            if error.winerror != winerror.ERROR_OPERATION_ABORTED:
                raise
        if wait == win32event.WAIT_TIMEOUT:
            raise TimeoutError('Named-pipe operation timed out')
        raise InterruptedError('Named-pipe operation cancelled')
    return win32file.GetOverlappedResult(handle, operation, False)


def connect_server(handle, stop):
    operation = pywintypes.OVERLAPPED()
    operation.hEvent = win32event.CreateEvent(None, True, False, None)
    try:
        try:
            result = win32pipe.ConnectNamedPipe(handle, operation)
            if result == winerror.ERROR_IO_PENDING:
                _wait(handle, operation, stop, None)
        except pywintypes.error as error:
            if error.winerror == winerror.ERROR_IO_PENDING:
                _wait(handle, operation, stop, None)
            elif error.winerror != winerror.ERROR_PIPE_CONNECTED:
                raise
    finally:
        operation.hEvent.Close()


def transfer(handle, stop, data_or_size, *, write=False, timeout=None):
    deadline = None if timeout is None else time.monotonic() + timeout
    result = bytearray()
    data = memoryview(data_or_size) if write else None
    remaining = len(data) if write else data_or_size
    offset = 0
    while remaining:
        if win32event.WaitForSingleObject(stop, 0) == win32event.WAIT_OBJECT_0:
            raise InterruptedError('Named-pipe operation cancelled')
        duration = None if deadline is None else deadline - time.monotonic()
        if duration is not None and duration <= 0:
            raise TimeoutError('Named-pipe operation timed out')
        operation = pywintypes.OVERLAPPED()
        operation.hEvent = win32event.CreateEvent(None, True, False, None)
        try:
            if write:
                code, _ = win32file.WriteFile(handle, data[offset:offset + remaining], operation)
                buffer = None
            else:
                buffer = win32file.AllocateReadBuffer(min(remaining, 1024 * 1024))
                code, _ = win32file.ReadFile(handle, buffer, operation)
            count = _wait(handle, operation, stop, duration) if code == winerror.ERROR_IO_PENDING else win32file.GetOverlappedResult(handle, operation, False)
            if not count:
                raise EOFError('Named-pipe peer disconnected')
            if not write:
                result.extend(buffer[:count])
            offset += count
            remaining -= count
        finally:
            operation.hEvent.Close()
    return bytes(result)

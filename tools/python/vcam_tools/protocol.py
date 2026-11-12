import struct

HEADER = struct.Struct('<ii')


def frame_size(width: int, height: int) -> int:
    if not (1 <= width <= 4096 and 1 <= height <= 2160):
        raise ValueError(f'Invalid RGB24 dimensions: {width}x{height}')
    return width * height * 3


def receive_exact(connection, size: int) -> bytes:
    data = bytearray(size)
    view = memoryview(data)
    offset = 0
    while offset < size:
        received = connection.recv_into(view[offset:])
        if not received:
            raise EOFError('Sender disconnected before completing the stream')
        offset += received
    return bytes(data)


class PipeWriter:
    def __init__(self, path, width, height):
        self.path, self.header = path, HEADER.pack(width, height)
        self.size = frame_size(width, height)
        self.stream = None

    def __enter__(self):
        self.stream = open(self.path, 'wb', buffering=0)
        try:
            self._write(self.header)
        except BaseException:
            self.stream.close()
            raise
        return self

    def _write(self, data):
        remaining = memoryview(data)
        while remaining:
            written = self.stream.write(remaining)
            if not written:
                raise BrokenPipeError('Camera pipe stopped accepting data')
            remaining = remaining[written:]

    def write(self, frame):
        if len(frame) != self.size:
            raise ValueError('Frame size changed within a pipe connection')
        self._write(frame)

    def __exit__(self, *_):
        self.stream.close()

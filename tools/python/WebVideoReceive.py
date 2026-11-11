"""Forward one framed RGB24 TCP stream into the camera application's named pipe."""

import socket
import struct

PIPE_PATH = r"\\.\pipe\vcam_pipe"
HOST = "0.0.0.0"
PORT = 5008
MAX_WIDTH = 4096
MAX_HEIGHT = 2160
MAX_FRAME_BYTES = 30 * 1024 * 1024


def recv_exact(connection: socket.socket, size: int) -> bytes:
    """Read exactly size bytes from TCP, which does not preserve message boundaries."""
    data = bytearray(size)
    view = memoryview(data)
    offset = 0
    while offset < size:
        received = connection.recv_into(view[offset:])
        if received == 0:
            raise ConnectionError("TCP peer closed the connection")
        offset += received
    return bytes(data)


def parse_resolution(header: bytes) -> tuple[int, int, int]:
    width, height = struct.unpack("<ii", header)
    if width <= 0 or width > MAX_WIDTH or height <= 0 or height > MAX_HEIGHT:
        raise ValueError(f"Unsupported frame size: {width}x{height}")
    frame_size = width * height * 3
    if frame_size > MAX_FRAME_BYTES:
        raise ValueError(f"RGB24 frame is too large: {frame_size} bytes")
    return width, height, frame_size


def main() -> None:
    print(f"Listening on {HOST}:{PORT}; waiting for the host sender...")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((HOST, PORT))
        server.listen(1)
        connection, address = server.accept()
        with connection:
            print(f"Connected to host sender {address}.")
            header = recv_exact(connection, 8)
            width, height, frame_size = parse_resolution(header)
            print(f"Receiving {width}x{height} RGB24 frames ({frame_size} bytes each).")

            # The camera app's named pipe uses one header followed by raw frames.
            # Open it only after validating the network header.
            with open(PIPE_PATH, "wb") as pipe:
                pipe.write(header)
                pipe.flush()
                frame_count = 0
                try:
                    while True:
                        frame = recv_exact(connection, frame_size)
                        pipe.write(frame)
                        pipe.flush()
                        frame_count += 1
                        if frame_count % 50 == 0:
                            print(f"Forwarded {frame_count} frames.")
                except ConnectionError as exc:
                    print(f"Sender disconnected: {exc}")
                finally:
                    print(f"Forwarded {frame_count} complete frames.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as exc:
        print(f"Receiver stopped: {exc}")

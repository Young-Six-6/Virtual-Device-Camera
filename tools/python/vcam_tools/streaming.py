import base64
import ctypes
import ipaddress
import socket
import subprocess
import sys
import time
from .capture import capture_session, FrameCaptureError
from .protocol import HEADER, PipeWriter, frame_size, receive_exact


def stream_frames(options, capture, write):
    interval = 1 / options.fps
    next_frame = time.perf_counter()
    failures = sent = 0
    statistic_started = time.perf_counter()
    statistic_frames = 0
    while not options.frames or sent < options.frames:
        try:
            frame = capture()
            if len(frame) != frame_size(options.width, options.height):
                raise FrameCaptureError('Unexpected RGB24 frame length')
            failures = 0
            write(frame)
            sent += 1
        except FrameCaptureError as error:
            failures += 1
            if failures >= options.capture_retries:
                raise RuntimeError(f'OBS capture failed {failures} times: {error}') from error
            print(f'OBS capture retry {failures}: {error}', file=sys.stderr)
        finally:
            next_frame = max(next_frame + interval, time.perf_counter())
            time.sleep(max(0, next_frame - time.perf_counter()))
        elapsed = time.perf_counter() - statistic_started
        if elapsed >= 5:
            print(f'Sent {sent} frames ({(sent - statistic_frames) / elapsed:.1f} FPS).')
            statistic_frames = sent
            statistic_started = time.perf_counter()
    return sent


def local_sender(options):
    with capture_session(options) as capture:
        with PipeWriter(options.pipe, options.width, options.height) as pipe:
            print(f'RGB24 {options.width}x{options.height} -> {options.pipe}')
            stream_frames(options, capture, pipe.write)


def vm_address(options):
    if not options.hyper_v:
        return options.tcp_host
    if options.elevate and not ctypes.windll.shell32.IsUserAnAdmin():
        result = ctypes.windll.shell32.ShellExecuteW(
            None, 'runas', sys.executable, subprocess.list2cmdline(sys.argv), None, 1)
        if result <= 32:
            raise OSError(f'Elevation failed: {result}')
        raise SystemExit(0)
    escaped = options.vm.replace("'", "''")
    command = f"Get-VMNetworkAdapter -VMName '{escaped}' -ErrorAction Stop | Select-Object -ExpandProperty IPAddresses"
    encoded = base64.b64encode(command.encode('utf-16le')).decode('ascii')
    try:
        response = subprocess.run(['powershell', '-NoProfile', '-EncodedCommand', encoded],
                                  capture_output=True, text=True, timeout=options.timeout,
                                  creationflags=subprocess.CREATE_NO_WINDOW, check=True)
        for line in response.stdout.splitlines():
            try:
                address = ipaddress.ip_address(line.strip())
                if address.version == 4 and not address.is_loopback:
                    return str(address)
            except ValueError:
                continue
    except (OSError, subprocess.SubprocessError) as error:
        print(f'Hyper-V discovery unavailable; using {options.tcp_host}: {error}', file=sys.stderr)
    return options.tcp_host


def network_sender(options):
    host = vm_address(options)
    with capture_session(options) as capture:
        with socket.create_connection((host, options.tcp_port), options.timeout) as connection:
            connection.settimeout(options.timeout)
            connection.sendall(HEADER.pack(options.width, options.height))
            print(f'RGB24 {options.width}x{options.height} -> {host}:{options.tcp_port}')
            stream_frames(options, capture, connection.sendall)


def network_receiver(options):
    received = 0
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((options.bind, options.tcp_port))
        server.listen(1)
        server.settimeout(0.25)
        print(f'Listening on {options.bind}:{options.tcp_port}')
        while not options.frames or received < options.frames:
            try:
                connection, address = server.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(options.timeout)
                try:
                    width, height = HEADER.unpack(receive_exact(connection, HEADER.size))
                    size = frame_size(width, height)
                    with PipeWriter(options.pipe, width, height) as pipe:
                        print(f'{address}: {width}x{height} -> {options.pipe}')
                        while not options.frames or received < options.frames:
                            pipe.write(receive_exact(connection, size))
                            received += 1
                except (EOFError, OSError, ValueError) as error:
                    print(f'Connection ended: {error}', file=sys.stderr)

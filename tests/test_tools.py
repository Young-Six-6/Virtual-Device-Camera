import base64
import contextlib
import io
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'python'))
from vcam_tools.config import load_options
from vcam_tools.protocol import HEADER, PipeWriter, frame_size, receive_exact
from vcam_tools.control import send_command
from vcam_tools.preview import PipePreview
from vcam_tools.capture import ObsCapture, FrameCaptureError
from vcam_tools.win_io import connect_server, transfer
import cv2
import numpy as np
import win32con
import win32event
import win32pipe


def pipe_name():
    return r'\\.\pipe\vcam_test_' + uuid.uuid4().hex


class ConfigTests(unittest.TestCase):
    def test_precedence_and_credentials(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'tools.toml'
            config.write_text('[common]\nwidth=640\nheight=480\n[tools.local]\nfps=15\n', encoding='utf-8')
            with patch.dict(os.environ, {'VCAM_WIDTH': '1280', 'OBS_WEBSOCKET_PASSWORD': 'test-only-value'}):
                options = load_options('local', ['--config', str(config), '--width', '1920'])
            self.assertEqual((options.width, options.height, options.fps), (1920, 480, 15))
            self.assertFalse(options.auto_size)
            self.assertEqual(options.obs_password, 'test-only-value')
            with patch.dict(os.environ, {'VCAM_CONFIG': str(config)}):
                self.assertEqual(load_options('color', []).width, 640)

    def test_invalid_settings_fail(self):
        for args in (['--width', '0'], ['--fps', 'nan'], ['--tcp-port', '99999']):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                load_options('send', args)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.toml'
            path.write_text('[common]\nwidth="720"\n', encoding='utf-8')
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                load_options('color', ['--config', str(path)])

    def test_legacy_defaults_and_control_preset(self):
        self.assertEqual(load_options('send', []).width, 1280)
        self.assertEqual(load_options('local', []).width, 1920)
        self.assertTrue(load_options('color', []).solid_color)
        options = load_options('control', ['resolution', '--resolution', '720'])
        self.assertEqual((options.width, options.height), (1280, 720))
        custom = load_options('control', ['resolution', '--resolution', '1024x768'])
        self.assertEqual((custom.width, custom.height), (1024, 768))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            load_options('control', ['resolution', '--resolution', '1023x768'])


class FrameTests(unittest.TestCase):
    def test_fragmented_tcp_and_eof(self):
        first, second = socket.socketpair()
        data = HEADER.pack(4, 2) + bytes([0,255,0]) * 8
        def send():
            with first:
                for byte in data:
                    first.sendall(bytes([byte]))
        sender = threading.Thread(target=send)
        sender.start()
        try:
            width, height = HEADER.unpack(receive_exact(second, 8))
            self.assertEqual(receive_exact(second, frame_size(width,height)), data[8:])
            with self.assertRaises(EOFError):
                receive_exact(second, 1)
        finally:
            second.close(); sender.join()

    def test_obs_capture_resizes_rgb_and_releases(self):
        image = np.zeros((3, 5, 3), dtype=np.uint8)
        image[:,:,2] = 255
        _, encoded = cv2.imencode('.png', image)
        class Fake:
            closed = False
            calls = 0
            def connect(self): pass
            def disconnect(self): self.closed = True
            def call(self, _):
                self.calls += 1
                data = {'baseWidth': 4, 'baseHeight': 2} if self.calls == 1 else {
                    'imageData': 'data:image/png;base64,' + base64.b64encode(encoded).decode()}
                return type('Response', (), {'datain': data})()
        client = Fake()
        options = load_options('local', [])
        with ObsCapture(options, factory=lambda *args, **kwargs: client) as capture:
            self.assertEqual(capture.frame(), bytes([255,0,0])*8)
        self.assertTrue(client.closed)


class WindowsPipeTests(unittest.TestCase):
    def test_preview_stream_and_pending_read_cancellation(self):
        name = pipe_name()
        receiver = PipePreview(name)
        try:
            with PipeWriter(name, 4, 2) as writer:
                writer.write(bytes([0,255,0])*8)
                deadline = time.monotonic()+3
                while receiver.snapshot() is None and time.monotonic() < deadline:
                    time.sleep(0.005)
                self.assertEqual(receiver.snapshot(), (4,2,bytes([0,255,0])*8))
                started = time.monotonic()
                receiver.close()
                self.assertLess(time.monotonic()-started, 1)
        finally:
            receiver.close()

    def test_partial_header_and_pending_connect_cancel(self):
        for partial in (False, True):
            name = pipe_name()
            receiver = PipePreview(name)
            client = open(name, 'wb', buffering=0) if partial else None
            try:
                if client:
                    client.write(b'\x80')
                started = time.monotonic()
                receiver.close()
                self.assertLess(time.monotonic()-started, 1)
            finally:
                if client: client.close()
                receiver.close()

    def test_control_timeout_cancels_overlapped_read(self):
        name = pipe_name()
        stop = win32event.CreateEvent(None, True, False, None)
        pipe = win32pipe.CreateNamedPipe(name, win32pipe.PIPE_ACCESS_DUPLEX | win32con.FILE_FLAG_OVERLAPPED,
            win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT, 1, 4096, 4096, 0, None)
        errors = []
        def server():
            try:
                connect_server(pipe, stop)
                transfer(pipe, stop, len(b'STATUS 1\n'))
                win32event.WaitForSingleObject(stop, 3000)
            except BaseException as error:
                errors.append(error)
        worker = threading.Thread(target=server)
        worker.start()
        try:
            with self.assertRaises(TimeoutError):
                send_command(name, 'STATUS 1', timeout=0.15)
        finally:
            win32event.SetEvent(stop); worker.join(); pipe.Close(); stop.Close()
        self.assertFalse(errors)

    def test_python_client_against_cpp_camera_control(self):
        executable = os.environ.get('VCAM_TEST_CONTROL_EXE')
        if not executable:
            self.skipTest('Build USB tests and set VCAM_TEST_CONTROL_EXE for cross-language integration')
        name = pipe_name()
        process = subprocess.Popen([executable, '--control-fixture', name], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.assertEqual(send_command(name, 'STATUS 1', 3), 'OK STATUS 1 640 480 RUNNING')
            self.assertEqual(send_command(name, 'RESOLUTION 1 1920 1080', 3), 'OK RESOLUTION 1 1920 1080 REOPEN_REQUIRED')
            self.assertEqual(send_command(name, 'STATUS 1', 3), 'OK STATUS 1 1920 1080 RUNNING')
            self.assertEqual(send_command(name, 'RESOLUTION 1 641 480', 3), 'ERR invalid-resolution')
        finally:
            try:
                output, _ = process.communicate('\n', timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); output, _ = process.communicate()
            self.assertEqual(process.returncode, 0, output)


if __name__ == '__main__':
    unittest.main()

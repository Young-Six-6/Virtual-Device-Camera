import base64
from contextlib import contextmanager
import cv2
import numpy as np
from obswebsocket import obsws, requests
from .protocol import frame_size


class FrameCaptureError(RuntimeError):
    pass


class ObsCapture:
    def __init__(self, options, factory=obsws):
        self.options = options
        self.client = factory(options.obs_host, options.obs_port, options.obs_password, timeout=options.timeout)
        self.connected = False

    def __enter__(self):
        try:
            self.client.connect()
            self.connected = True
            if self.options.auto_size:
                try:
                    settings = self.client.call(requests.GetVideoSettings()).datain
                    width, height = settings['baseWidth'], settings['baseHeight']
                    frame_size(width, height)
                    self.options.width, self.options.height = width, height
                except Exception as error:
                    print(f'OBS canvas size unavailable; using configured dimensions: {error}')
            return self
        except BaseException:
            self.client.disconnect()
            raise

    def __exit__(self, *_):
        if self.connected:
            self.client.disconnect()

    def frame(self):
        options = self.options
        try:
            response = self.client.call(requests.GetSourceScreenshot(
                sourceName=options.scene, imageFormat='jpeg', width=options.width,
                height=options.height, quality=80)).datain
            text = response.get('imageData', '')
            prefix, encoded = text.split(',', 1)
            if not prefix.startswith('data:image/') or not prefix.endswith(';base64'):
                raise ValueError('OBS returned an invalid screenshot data URL')
            data = base64.b64decode(encoded, validate=True)
            image = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                raise ValueError('OBS screenshot could not be decoded')
            if image.shape[:2] != (options.height, options.width):
                image = cv2.resize(image, (options.width, options.height))
            return cv2.cvtColor(image, cv2.COLOR_BGR2RGB).tobytes()
        except Exception as error:
            # The OBS library raises several transport-specific exception types.
            # Convert them at this boundary; retry policy lives in the sender.
            raise FrameCaptureError(str(error)) from error


@contextmanager
def capture_session(options):
    if options.solid_color:
        pixels = bytes(options.color) * (options.width * options.height)
        yield lambda: pixels
    else:
        with ObsCapture(options) as capture:
            yield capture.frame

import struct
import time

PIPE_PATH = r'\\.\pipe\vcam_pipe'
WIDTH, HEIGHT = 1920, 1080
FRAME_RATE = 10
RGB_COLOR = (0, 255, 0)  # Green test frame.

def main():
    frame = bytes(RGB_COLOR) * (WIDTH * HEIGHT)
    interval = 1 / FRAME_RATE
    with open(PIPE_PATH, 'wb') as pipe:
        pipe.write(struct.pack('<ii', WIDTH, HEIGHT))
        pipe.flush()
        print(f'Sending a {WIDTH}x{HEIGHT} RGB test frame at {FRAME_RATE} FPS.')
        try:
            while True:
                started = time.perf_counter()
                pipe.write(frame)
                pipe.flush()
                time.sleep(max(0, interval - (time.perf_counter() - started)))
        except KeyboardInterrupt:
            print('\nStopped.')


if __name__ == '__main__':
    main()

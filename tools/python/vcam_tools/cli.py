import sys
from .config import load_options


def main(mode, argv=None):
    options = load_options(mode, argv)
    try:
        if mode in ('local', 'color'):
            from .streaming import local_sender
            local_sender(options)
        elif mode == 'send':
            from .streaming import network_sender
            network_sender(options)
        elif mode == 'receive':
            from .streaming import network_receiver
            network_receiver(options)
        elif mode == 'preview':
            from .preview import preview
            preview(options)
        elif mode == 'control':
            from .control import send_command
            command = f'STATUS {options.camera}' if options.command == 'status' else (
                f'RESOLUTION {options.camera} {options.width} {options.height}')
            response = send_command(options.control_pipe, command, options.timeout)
            print(response)
            return 0 if response.startswith('OK ') else 1
        return 0
    except KeyboardInterrupt:
        print('\nStopped.')
        return 0
    except Exception as error:
        # Command boundary: library-specific failures get one concise error;
        # --debug retains full tracebacks for diagnosis.
        if options.debug:
            raise
        print(f'{mode}: {error}', file=sys.stderr)
        return 1

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
import tomllib

DEFAULT_CONFIG = Path(__file__).resolve().parents[3] / 'config' / 'tools.toml'
DEFAULTS = dict(pipe=r'\\.\pipe\vcam_pipe', control_pipe=r'\\.\pipe\vcam_control',
                width=1920, height=1080, fps=10.0, timeout=10.0, frames=0,
                solid_color=False, color='0,255,0', obs_host='localhost', obs_port=4458,
                scene='test1', auto_size=False, capture_retries=5, tcp_host='192.168.100.2',
                bind='0.0.0.0', tcp_port=5008, vm='camera', hyper_v=True)


def color_value(text: str) -> tuple[int, int, int]:
    try:
        color = tuple(int(part) for part in text.split(','))
        if len(color) != 3 or any(value < 0 or value > 255 for value in color):
            raise ValueError
        return color
    except (ValueError, AttributeError) as exc:
        raise argparse.ArgumentTypeError('color must be R,G,B with values 0..255') from exc


def load_options(mode: str, argv=None):
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument('--config', type=Path, default=Path(os.environ.get('VCAM_CONFIG', DEFAULT_CONFIG)))
    known, _ = pre.parse_known_args(argv)
    defaults = DEFAULTS.copy()
    if mode == 'send':
        defaults.update(width=1280, height=720)
    if mode == 'local':
        defaults.update(auto_size=True, color='255,0,0')
    if mode == 'color':
        defaults['solid_color'] = True
    parser = argparse.ArgumentParser(parents=[pre], description=f'Virtual Camera {mode} utility')
    try:
        if known.config.exists():
            with known.config.open('rb') as stream:
                data = tomllib.load(stream)
            if set(data) - {'common', 'obs', 'network', 'tools'}:
                raise ValueError('Unknown configuration section')
            tables = [data.get(section, {}) for section in ('common', 'obs', 'network')]
            tool_tables = data.get('tools', {})
            if not isinstance(tool_tables, dict):
                raise ValueError('The tools configuration must be a table')
            tables.append(tool_tables.get(mode, {}))
            if any(not isinstance(table, dict) for table in tables):
                raise ValueError('Configuration sections must be tables')
            for table in tables:
                if set(table) - DEFAULTS.keys():
                    raise ValueError('Unknown configuration keys: ' + ', '.join(set(table) - DEFAULTS.keys()))
                defaults.update(table)
        elif known.config != DEFAULT_CONFIG:
            raise ValueError(f'Configuration file does not exist: {known.config}')
        for key, fallback in DEFAULTS.items():
            value = os.environ.get('VCAM_' + key.upper())
            if value is not None:
                if isinstance(fallback, bool):
                    if value.lower() not in ('true', 'false', '1', '0'):
                        raise ValueError(f'VCAM_{key.upper()} must be true/false')
                    value = value.lower() in ('true', '1')
                else:
                    value = type(fallback)(value)
                defaults[key] = value
        for key, fallback in DEFAULTS.items():
            value = defaults[key]
            valid = isinstance(value, bool) if isinstance(fallback, bool) else (
                isinstance(value, (int, float)) and not isinstance(value, bool) if isinstance(fallback, float)
                else isinstance(value, type(fallback)) and not (isinstance(fallback, int) and isinstance(value, bool)))
            if not valid:
                raise ValueError(f'Invalid type for configuration key {key}')
    except (OSError, ValueError, tomllib.TOMLDecodeError) as error:
        parser.error(str(error))
    parser.set_defaults(**defaults)
    names = {'timeout', 'frames'}
    if mode in ('local', 'color', 'receive', 'preview'):
        names.add('pipe')
    if mode in ('local', 'color', 'send', 'control'):
        names.update(('width', 'height'))
    if mode in ('local', 'color', 'send'):
        names.update(('fps', 'color'))
    if mode in ('local', 'send'):
        names.update(('obs_host', 'obs_port', 'scene', 'auto_size', 'solid_color', 'capture_retries'))
    if mode in ('send', 'receive'):
        names.add('tcp_port')
        names.add('tcp_host' if mode == 'send' else 'bind')
    if mode == 'send':
        names.update(('vm', 'hyper_v'))
        parser.add_argument('--elevate', action='store_true', help='Request elevation for Hyper-V IP discovery')
    if mode == 'control':
        names = {'control_pipe', 'width', 'height', 'timeout'}
    for name in names & {'pipe', 'control_pipe', 'obs_host', 'scene', 'tcp_host', 'bind', 'vm'}:
        parser.add_argument('--' + name.replace('_', '-'), default=defaults[name])
    for name in sorted(names & {'width', 'height', 'obs_port', 'tcp_port', 'frames', 'capture_retries'}):
        parser.add_argument('--' + name.replace('_', '-'), type=int, default=defaults[name])
    for name in sorted(names & {'fps', 'timeout'}):
        parser.add_argument('--' + name, type=float, default=defaults[name])
    for name in sorted(names & {'solid_color', 'auto_size', 'hyper_v'}):
        parser.add_argument('--' + name.replace('_', '-'), action=argparse.BooleanOptionalAction, default=defaults[name])
    if 'color' in names:
        parser.add_argument('--color', type=color_value, default=defaults['color'])
    parser.add_argument('--debug', action='store_true', help='Show exception tracebacks')
    if mode == 'control':
        parser.add_argument('command', choices=('status', 'resolution'))
        parser.add_argument('--camera', type=int, default=1)
        parser.add_argument('--resolution', help='Preset (480/720/1080) or WIDTHxHEIGHT')
    options = parser.parse_args(argv)
    if isinstance(options.color, str):
        try:
            options.color = color_value(options.color)
        except argparse.ArgumentTypeError as error:
            parser.error(str(error))
    if mode == 'color':
        options.solid_color = True
    tokens = sys.argv[1:] if argv is None else argv
    if any(token.split('=', 1)[0] in ('--width', '--height') for token in tokens) and '--auto-size' not in tokens:
        options.auto_size = False
    if mode == 'control' and options.resolution:
        presets = {'480': (640, 480), '720': (1280, 720), '1080': (1920, 1080)}
        try:
            if options.resolution in presets:
                options.width, options.height = presets[options.resolution]
            else:
                options.width, options.height = (int(part) for part in options.resolution.lower().split('x'))
        except (ValueError, TypeError):
            parser.error('--resolution must be 480, 720, 1080, or WIDTHxHEIGHT')
    if not (1 <= options.width <= 4096 and 1 <= options.height <= 2160):
        parser.error('frame dimensions must be within 1..4096 by 1..2160')
    if mode == 'control' and (options.width < 2 or options.width % 2 or options.camera < 1):
        parser.error('camera output width must be even and at least 2; camera index must be positive')
    if not (0 < options.fps <= 120 and 0 < options.timeout <= 60):
        parser.error('fps must be within (0,120]; timeout within (0,60] seconds')
    if any(not 1 <= port <= 65535 for port in (options.obs_port, options.tcp_port)):
        parser.error('ports must be within 1..65535')
    if options.frames < 0 or options.capture_retries < 1:
        parser.error('frames cannot be negative; capture-retries must be positive')
    options.obs_password = os.environ.get('OBS_WEBSOCKET_PASSWORD', '')
    return options

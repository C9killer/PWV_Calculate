"""Capture existing firmware without flashing/resetting it; preserve binary UART data."""
import argparse
import json
import re
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import serial


def printable_lines(data):
    # The console multiplexes 10-byte binary sample frames and ESP-IDF text.
    # Only retain printable runs that look like actual ESP-IDF log records.
    for run in re.findall(rb'[\x20-\x7e]{10,}', data):
        match = re.search(rb'[IWEVD] \(\d+\) [A-Za-z0-9_]+: .*', run)
        if match:
            yield match.group().decode('ascii')


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM22')
    parser.add_argument('--baud', type=int, default=921600)
    parser.add_argument('--seconds', type=float, default=120)
    parser.add_argument('--condition', default='unspecified')
    args = parser.parse_args()
    zone = timezone(timedelta(hours=8))
    started = datetime.now(zone)
    folder = Path(__file__).resolve().parents[1] / 'reports' / ('esp32_profile_' + started.strftime('%Y%m%d_%H%M%S'))
    folder.mkdir(parents=True, exist_ok=False)
    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.2
    port.dtr = False
    port.rts = False
    total = 0
    with port, (folder / 'serial_raw.bin').open('wb') as output:
        # Discard the USB driver's pending bytes; do not replay them as fresh windows.
        port.reset_input_buffer()
        begin = time.monotonic()
        last_report = begin
        print('CAPTURE_DIR=' + str(folder), flush=True)
        while time.monotonic() - begin < args.seconds:
            chunk = port.read(max(1, min(port.in_waiting, 32768)))
            output.write(chunk)
            total += len(chunk)
            if time.monotonic() - last_report >= 20:
                output.flush()
                print(f'CAPTURE elapsed={time.monotonic()-begin:.1f}s bytes={total}', flush=True)
                last_report = time.monotonic()
        elapsed = time.monotonic() - begin
    ended = datetime.now(zone)
    records = list(printable_lines((folder / 'serial_raw.bin').read_bytes()))
    (folder / 'device_log.txt').write_text('\n'.join(records) + '\n', encoding='utf-8')
    metadata = {
        'started_at': started.isoformat(), 'ended_at': ended.isoformat(),
        'capture_seconds': elapsed, 'port': args.port, 'baud': args.baud,
        'raw_bytes': total, 'text_log_records': len(records),
        'reset_requested': False, 'firmware_flashed': False,
        'initial_pending_bytes_discarded': True,
        'input_condition_user_reported': args.condition,
    }
    (folder / 'capture_metadata.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
    print('CAPTURE_DONE=' + str(folder), flush=True)
    print(json.dumps(metadata, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()

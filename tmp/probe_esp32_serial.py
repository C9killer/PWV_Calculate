import time
import re
import sys
from pathlib import Path

import serial

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
output = Path(__file__).with_name('esp32_serial_probe.log')
if '--read-only' in sys.argv:
    data = output.read_bytes()
    print('bytes:', len(data))
    for text in re.findall(rb'[\x20-\x7e]{18,}', data):
        print(text.decode('ascii'))
    raise SystemExit(0)
port = serial.Serial()
port.port = 'COM22'
port.baudrate = 921600
port.timeout = 0.25
port.dtr = False
port.rts = False
with port, output.open('wb') as log:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        chunk = port.read(max(1, min(port.in_waiting, 16384)))
        if chunk:
            log.write(chunk)
            log.flush()
print(output)
print(output.read_bytes()[-16000:].decode('utf-8', errors='replace'))

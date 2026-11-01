"""Create complete ESP32-S3 images for flashing at 0x0; includes private pair keys."""
import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import re

root = Path(__file__).resolve().parents[1]
pio = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
out = root / 'firmware'
out.mkdir(exist_ok=True)
key_source = (root / 'include/secrets.h').read_text()
key = bytes(map(int, re.search(r'PAIR_KEY\[32\]\s*=\s*\{([^}]+)', key_source).group(1).split(',')))
assert len(key) == 32
manifest = {'chip': 'ESP32-S3', 'flash_size': '8MB', 'write_address': '0x0',
            'version': re.search(r'FIRMWARE_VERSION = "([^"]+)"', (root / 'src/main.cpp').read_text(encoding='utf-8')).group(1),
            'web_sha256': hashlib.sha256((root / 'web/index.html').read_bytes()).hexdigest(),
            'private_pair_key_embedded': True, 'files': {}}
for node in ('a', 'b'):
    build = root / '.pio/build' / ('node_' + node)
    output = out / f'LoRaChat-{node.upper()}.factory.bin'
    parts = [build / 'bootloader.bin', build / 'partitions.bin',
             pio / 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin',
             build / 'firmware.bin']
    assert all(p.is_file() for p in parts), 'Build both node environments first'
    command = [sys.executable, str(pio / 'packages/tool-esptoolpy/esptool.py'),
               '--chip', 'esp32s3', 'merge_bin', '-o', str(output),
               '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '8MB']
    for address, path in zip(('0x0', '0x8000', '0xe000', '0x10000'), parts):
        command += [address, str(path)]
    subprocess.run(command, check=True)
    manifest['files'][output.name] = {'size': output.stat().st_size,
                                    'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
    app = (build / 'firmware.bin').read_bytes()
    header = struct.pack('<8sBBBBI32s', b'LCHATOTA', 1, 1 if node == 'a' else 2, 1, 0,
                         len(app), hashlib.sha256(app).digest())
    signature = hmac.digest(key, header, 'sha256')
    ota = out / f'LoRaChat-{node.upper()}.ota'
    ota.write_bytes(header + signature + app)
    manifest['files'][ota.name] = {'size': ota.stat().st_size,
                                 'sha256': hashlib.sha256(ota.read_bytes()).hexdigest(),
                                 'format': 'Authenticated web OTA package, not a raw flash image'}
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print('Packaged both private firmware images in ' + str(out))

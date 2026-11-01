"""Verify both private OTA/factory packages without printing their signing key."""
from pathlib import Path
import gzip
import hashlib
import hmac
import json
import re
import struct

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'firmware/manifest.json').read_text(encoding='utf-8'))
version = re.search(r'FIRMWARE_VERSION = "([^"]+)"', (root / 'src/main.cpp').read_text(encoding='utf-8')).group(1)
assert manifest['version'] == version
sha = lambda data: hashlib.sha256(data).digest()
web = (root / 'web/index.html').read_bytes()
assert sha(web).hex() == manifest['web_sha256']
packed = bytes(map(int, re.search(r'\{([^}]+)', (root / 'include/web_gz.h').read_text()).group(1).split(',')))
assert gzip.decompress(packed) == web
key = bytes(map(int, re.search(r'PAIR_KEY\[32\]\s*=\s*\{([^}]+)',
                             (root / 'include/secrets.h').read_text()).group(1).split(',')))
for filename, entry in manifest['files'].items():
    data = (root / 'firmware' / filename).read_bytes()
    assert len(data) == entry['size'] and sha(data).hex() == entry['sha256']
for role, node in enumerate(('A', 'B'), 1):
    package = (root / f'firmware/LoRaChat-{node}.ota').read_bytes()
    magic, fmt, actual_role, protocol, reserved, length, digest = struct.unpack('<8sBBBBI32s', package[:48])
    assert (magic, fmt, actual_role, protocol, reserved) == (b'LCHATOTA', 1, role, 1, 0)
    assert hmac.compare_digest(hmac.digest(key, package[:48], 'sha256'), package[48:80])
    app = package[80:]
    assert length == len(app) <= 0x300000 and sha(app) == digest
    build = root / f'.pio/build/node_{node.lower()}'
    assert app == (build / 'firmware.bin').read_bytes() and packed in app
    assert version.encode() + b'\0' in app
    factory = (root / f'firmware/LoRaChat-{node}.factory.bin').read_bytes()
    assert factory[0x10000:0x10000+len(app)] == app
    for offset, filename in ((0, 'bootloader.bin'), (0x8000, 'partitions.bin')):
        part = (build / filename).read_bytes()
        assert factory[offset:offset+len(part)] == part
result = {'ok': True, 'version': version, 'web_sha256': manifest['web_sha256'],
          'checks': ['roles', 'HMAC', 'app SHA256', 'manifest', 'build match', 'embedded web', 'factory parts']}
(root / f'test-output/packages-{version}-check.json').write_text(json.dumps(result), encoding='utf-8')
print(f'PASS: {version} A/B OTA signatures, hashes, factory parts, build and web match')

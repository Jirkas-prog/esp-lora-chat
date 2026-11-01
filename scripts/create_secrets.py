"""Create a private pair once; never rotate a deployed pair implicitly."""
import secrets
from pathlib import Path

root = Path(__file__).resolve().parents[1]
target = root / "include/secrets.h"
if target.exists():
    print("Existing pair retained.")
else:
    key = secrets.token_bytes(32)
    password = secrets.token_urlsafe(12)
    target.parent.mkdir(exist_ok=True)
    target.write_text('#pragma once\nstatic const uint8_t PAIR_KEY[32] = {' +
                      ','.join(str(n) for n in key) + '};\n' +
                      f'static const char AP_PASSWORD[] = "{password}";\n')
    (root / "PRIVATE-pristup.txt").write_text(
        f"Wi-Fi: LoRaChat-A / LoRaChat-B\nHeslo obou siti: {password}\n"
        "Chat: http://192.168.4.1\n"
        "Soukromy klic paru je v include/secrets.h. Nesdilet firmware ani tento soubor.\n",
        encoding="utf-8")
    print("Private key and Wi-Fi credentials created locally.")

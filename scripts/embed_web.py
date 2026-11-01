Import("env")
import gzip
from pathlib import Path

root = Path(env["PROJECT_DIR"])
packed = gzip.compress((root / "web/index.html").read_bytes(), mtime=0)
target = root / "include/web_gz.h"
source = "#pragma once\n#include <Arduino.h>\nstatic const uint8_t WEB_GZ[] PROGMEM = {\n"
source += ",".join(str(n) for n in packed) + "\n};\n"
if not target.exists() or target.read_text() != source:
    target.write_text(source)

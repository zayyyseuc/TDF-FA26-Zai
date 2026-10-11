#!/usr/bin/env python3
"""Generate an Arduino header that embeds the frontend; no LittleFS needed.
Usage: python3 tools/build_web_assets.py
"""
from pathlib import Path
import gzip
import json
ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'frontend'
OUT = ROOT / 'iss_control' / 'web_assets.h'

def raw_str(s):
    # Use safe delimiter, avoid Arduino's function prototype parser issues.
    return 'R"ISSWEB_' + s + '_ISSWEB"'

def gz_array(name, file):
    payload=gzip.compress((WEB/file).read_bytes(), compresslevel=9, mtime=0)
    rows=['  '+', '.join('0x%02X'%c for c in payload[i:i+16])+',' for i in range(0,len(payload),16)]
    return '\n'.join([f'const uint8_t {name}[] PROGMEM = {{',*rows, '};',f'constexpr size_t {name}_LEN = sizeof({name});'])

s=['// AUTO-GENERATED. Edit frontend files then run tools/build_web_assets.py',
   '#pragma once', '#include <Arduino.h>',
   gz_array('INDEX_HTML_GZ','index.html'),
   gz_array('ICON192_GZ','icon-192.png'),
   gz_array('ICON512_GZ','icon-512.png'),
   'const char MANIFEST_JSON[] PROGMEM = R"ISSWEB('+ (WEB/'manifest.webmanifest').read_text(encoding='utf-8') +')ISSWEB";',
   'const char SERVICE_WORKER_JS[] PROGMEM = R"ISSWEB('+ (WEB/'sw.js').read_text(encoding='utf-8') +')ISSWEB";',
   '']
OUT.write_text('\n\n'.join(s), encoding='utf-8')
print('Generated', OUT, 'size', OUT.stat().st_size)

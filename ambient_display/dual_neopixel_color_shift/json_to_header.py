#!/usr/bin/env python3
"""Convert EXISTING iss_data.json into iss_data.h without fetching data again.
Usage: python3 json_to_header.py [path/to/iss_data.json]
"""
import json
import sys
from pathlib import Path

source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).with_name("iss_data.json")
if not source.is_file():
    sys.exit(f"Cannot find {source}. Place your previously saved iss_data.json here, or pass its path.")

payload = json.loads(source.read_text(encoding="utf-8"))
records = payload.get("records")
if not isinstance(records, list) or len(records) < 2:
    sys.exit("Expected the original ISS JSON format with at least two entries in records[].")

points = []
for record in records:
    t = int(record["timestamp"])
    lat = float(record["latitude"])
    lon = float(record["longitude"])
    if not (0 <= t <= 4294967295 and -90 <= lat <= 90 and -180 <= lon <= 180):
        sys.exit(f"Invalid ISS record: {record}")
    points.append((t, lat, lon))
points.sort(key=lambda p: p[0])
if points[-1][0] <= points[0][0]:
    sys.exit("Recording needs at least two distinct timestamps.")

lines = [
    "// Generated from your existing ISS JSON; no new API request.",
    "#pragma once",
    "#include <stdint.h>",
    "#include <stddef.h>",
    "struct ISSPoint { uint32_t timestamp; float latitude; float longitude; };",
    "const ISSPoint ISS_DATA[] = {",
]
for timestamp, latitude, longitude in points:
    lines.append(f"  {{{timestamp}UL, {latitude:.6f}f, {longitude:.6f}f}},")
lines.extend([
    "};",
    "constexpr size_t ISS_COUNT = sizeof(ISS_DATA) / sizeof(ISS_DATA[0]);",
    "",
])
out = Path(__file__).with_name("iss_data.h")
out.write_text("\n".join(lines), encoding="utf-8")
print(f"Wrote {out} with {len(points)} samples. No network request was made.")

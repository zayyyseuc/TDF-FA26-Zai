#!/usr/bin/env python3
"""Convert EXISTING ISS JSON to a header for the ISS Control sketch.
Usage from project root: python3 tools/json_to_header.py [path/to/iss_data.json]
"""
import json
import sys
from pathlib import Path

project = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).expanduser().resolve() if len(sys.argv) > 1 else project / "iss_data.json"
if not source.is_file():
    sys.exit(f"Cannot find {source}. Place your previously saved iss_data.json here, or pass its path.")

payload = json.loads(source.read_text(encoding="utf-8"))
records = payload.get("records")
if not isinstance(records, list) or len(records) < 2:
    sys.exit("Expected the original ISS JSON format with at least two entries in records[].")

points = []
weather = []
for record in records:
    t = int(record["timestamp"])
    lat = float(record["latitude"])
    lon = float(record["longitude"])
    if not (0 <= t <= 4294967295 and -90 <= lat <= 90 and -180 <= lon <= 180):
        sys.exit(f"Invalid ISS record: {record}")
    points.append((t, lat, lon))
    sample = record.get("weather", record)
    raw_code = sample.get("weather_code")
    if raw_code is None:
        weather.append((t, -1, 0.0, 0.0, 0, 1, False))
        continue
    code = int(raw_code)
    temperature = float(sample.get("temperature_2m", sample.get("temperature", 0.0)))
    precipitation = float(sample.get("precipitation", 0.0))
    cloud = int(sample.get("cloud_cover", sample.get("cloudCover", 0)))
    is_day = int(sample.get("is_day", sample.get("isDay", 1)))
    if not (0 <= code <= 99 and -100 <= temperature <= 100 and
            precipitation >= 0 and 0 <= cloud <= 100 and is_day in (0, 1)):
        sys.exit(f"Invalid weather record: {record}")
    weather.append((t, code, temperature, precipitation, cloud, is_day, True))
points.sort(key=lambda p: p[0])
weather.sort(key=lambda p: p[0])
if points[-1][0] <= points[0][0]:
    sys.exit("Recording needs at least two distinct timestamps.")

lines = [
    "// Generated from your existing ISS JSON; no new API request.",
    "#pragma once",
    "#include <stdint.h>",
    "#include <stddef.h>",
    "struct ISSPoint { uint32_t timestamp; float latitude; float longitude; };",
    "struct WeatherPoint { uint32_t timestamp; int16_t code; float temperature; float precipitation; uint8_t cloudCover; bool isDay; bool valid; };",
    "const ISSPoint ISS_DATA[] = {",
]
for timestamp, latitude, longitude in points:
    lines.append(f"  {{{timestamp}UL, {latitude:.6f}f, {longitude:.6f}f}},")
lines.extend([
    "};",
    "constexpr size_t ISS_COUNT = sizeof(ISS_DATA) / sizeof(ISS_DATA[0]);",
    "const WeatherPoint WEATHER_DATA[] = {",
])
for timestamp, code, temperature, precipitation, cloud, is_day, valid in weather:
    lines.append(
        f"  {{{timestamp}UL, {code}, {temperature:.3f}f, {precipitation:.3f}f, "
        f"{cloud}, {is_day}, {'true' if valid else 'false'}}},"
    )
lines.extend([
    "};",
    "constexpr size_t WEATHER_COUNT = sizeof(WEATHER_DATA) / sizeof(WEATHER_DATA[0]);",
    "",
])
out = project / "iss_control" / "iss_data.h"
out.write_text("\n".join(lines), encoding="utf-8")
print(f"Wrote {out} with {len(points)} samples. No network request was made.")

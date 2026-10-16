#!/usr/bin/env python3
"""Add historical Open-Meteo weather samples to an ISS replay JSON.

Usage:
  python3 tools/fetch_weather_replay.py
  python3 tools/fetch_weather_replay.py iss_control/iss_data.json iss_control/iss_weather_data.json

The source file is not modified. Weather is fetched in coordinate batches and
sampled from the nearest hourly historical data point.
"""
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import Request, urlopen

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = PROJECT / "iss_control" / "iss_data.json"
DEFAULT_OUTPUT = PROJECT / "iss_control" / "iss_weather_data.json"
API_URL = "https://archive-api.open-meteo.com/v1/archive"
CHUNK_SIZE = 32


def utc_date(timestamp):
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).date()


def epoch_from_iso(value):
    return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp())


def fetch_batch(records):
    latitudes = ",".join(f"{float(r['latitude']):.5f}" for r in records)
    longitudes = ",".join(f"{float(r['longitude']):.5f}" for r in records)
    days = [utc_date(int(r["timestamp"])) for r in records]
    start_date = min(days).isoformat()
    end_date = max(days).isoformat()
    params = {
        "latitude": latitudes,
        "longitude": longitudes,
        "start_date": start_date,
        "end_date": end_date,
        "hourly": "weather_code,temperature_2m,precipitation,cloud_cover,is_day",
        "timezone": "UTC",
    }
    url = f"{API_URL}?{urlencode(params)}"
    request = Request(url, headers={"User-Agent": "ISS-Control-replay-weather/1.0"})
    for attempt in range(3):
        try:
            with urlopen(request, timeout=45) as response:
                payload = json.load(response)
            return payload if isinstance(payload, list) else [payload]
        except Exception:
            if attempt == 2:
                raise
            time.sleep(2 ** attempt)


def nearest_hour(weather, timestamp):
    times = [epoch_from_iso(value) for value in weather["hourly"]["time"]]
    if not times:
        raise ValueError("Open-Meteo returned no hourly timestamps")
    index = min(range(len(times)), key=lambda i: abs(times[i] - timestamp))
    hourly = weather["hourly"]
    return {
        "weather_code": int(hourly["weather_code"][index]),
        "temperature_2m": float(hourly["temperature_2m"][index]),
        "precipitation": float(hourly["precipitation"][index] or 0),
        "cloud_cover": int(hourly["cloud_cover"][index]),
        "is_day": int(hourly["is_day"][index]),
    }


def main():
    source = Path(sys.argv[1]).expanduser().resolve() if len(sys.argv) > 1 else DEFAULT_SOURCE
    output = Path(sys.argv[2]).expanduser().resolve() if len(sys.argv) > 2 else DEFAULT_OUTPUT
    if not source.is_file():
        raise SystemExit(f"Cannot find source JSON: {source}")
    payload = json.loads(source.read_text(encoding="utf-8"))
    records = payload.get("records")
    if not isinstance(records, list) or len(records) < 2:
        raise SystemExit("Expected at least two records[] in the ISS JSON.")

    enriched = [dict(record) for record in records]
    total = len(enriched)
    for start in range(0, total, CHUNK_SIZE):
        batch = enriched[start:start + CHUNK_SIZE]
        print(f"Fetching weather {start + 1}-{start + len(batch)} / {total} ...", flush=True)
        responses = fetch_batch(batch)
        if len(responses) != len(batch):
            raise SystemExit(
                f"Open-Meteo returned {len(responses)} locations for {len(batch)} records"
            )
        for record, response in zip(batch, responses):
            record["weather"] = nearest_hour(response, int(record["timestamp"]))
        time.sleep(0.25)

    result = dict(payload)
    result["weather_source"] = API_URL
    result["weather_resolution"] = "nearest hourly historical sample"
    result["weather_variables"] = [
        "weather_code", "temperature_2m", "precipitation", "cloud_cover", "is_day"
    ]
    result["records"] = enriched
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    valid = sum("weather" in record for record in enriched)
    print(f"Wrote {output} with {valid}/{total} weather records.")


if __name__ == "__main__":
    main()

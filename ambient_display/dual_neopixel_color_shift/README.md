# Dual NeoPixel: colors change with position

Hardware: Adafruit ESP32 Feather V2; RGB NeoPixel strips (`NEO_GRB`).

- **Outer orbit:** A0/GPIO26 -> outer DIN. At most one LED lit at a time. The moving star fades down between pixels and its hue slowly travels through a six-color palette as it moves.
- **Inner scattered lights:** A1/GPIO25 -> inner DIN. Two independently breathing LEDs; each selects a new location **and** a new palette color after fading out.
- Both strips' GND connected to ESP32 GND and a suitable regulated external 5 V power supply's GND. Both 5V pads to that supply. Size the PSU for your actual LED count. Keep USB 5V isolated from an independently powered 5V rail; provide data level shifting (3.3V -> 5V) when possible.

## Quick start

1. Install the Arduino **Adafruit NeoPixel** library; select **Adafruit Feather ESP32 V2**.
2. Open `dual_neopixel_color_shift.ino` and set `OUTER_LEDS` and `INNER_LEDS` to the number of physical LEDs.
3. `USE_ISS_REPLAY 0`: animated 12-second mock orbit, no other files needed.
4. `USE_ISS_REPLAY 1`: put your **existing** `iss_data.h` in the same folder as the `.ino`. Alternatively place `iss_data.json` in this folder and run `python3 json_to_header.py` first; it produces `iss_data.h` **offline**. You do not need to re-fetch data.
5. Both animations run concurrently, independent of Wi-Fi.

Tune `TEST_ORBIT_MS` (lap time), `ISS_REPLAY_MS` (track replay length), `OUTER_BRIGHTNESS`, `INNER_BRIGHTNESS`, `SPARK_MIN_MS` and `SPARK_MAX_MS`. Edit `PALETTE[]` to choose your desired six RGB colors. `REVERSE_ORBIT` reverses orbit direction.

**Upload issue:** if the Arduino IDE's esptool fails to verify flash chip connection, the upload/USB issue is independent of the animations. Check Upload Speed 115200, board selection, and a test with LEDs disconnected.

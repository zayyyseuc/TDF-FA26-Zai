// ESP32 Feather V2 + two RGB NeoPixel strips + live ISS API
// Outer: one moving, fading, color-shifting ISS star
// Inner: two random stars breathing independently and changing color
// No iss_data.json / iss_data.h required.
// Requires: Adafruit NeoPixel, ArduinoJson v7, SparkFun Qwiic Haptic Driver DA7280, and secrets.h

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include "Haptic_Driver.h" // SparkFun DA7280 library (not DRV2605L)
#include <math.h>
#include "secrets.h"   // Defines SECRET_SSID and SECRET_PASSWORD

// ---------- LED HARDWARE ----------
#define OUTER_PIN   26   // A0 -> outer strip DIN
#define INNER_PIN   25   // A1 -> inner strip DIN
#define OUTER_LEDS  30   // CHANGE to the actual number of outer LEDs
#define INNER_LEDS   8   // CHANGE to the actual number of inner LEDs

Adafruit_NeoPixel outerStrip(OUTER_LEDS, OUTER_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel innerStrip(INNER_LEDS, INNER_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t OUTER_BRIGHTNESS = 70;  // 0..255
const uint8_t INNER_BRIGHTNESS = 55;  // 0..255
const bool REVERSE_ORBIT = false;

// ---------- DA7280 HAPTIC SETTINGS ----------
// Feather V2 SDA/SCL -> DA7280 SDA/SCL via STEMMA QT / Qwiic cable.
// DA7280 power is 3.3V; NeoPixel power remains separate 5V.
Haptic_Driver haptic;
bool hapticReady = false;

const float LONGITUDE_STEP_DEG = 0.5f;     // Vibrate after each 5 degrees traveled
const uint8_t HAPTIC_INTENSITY = 25;       // 0..127, start gently
const uint32_t HAPTIC_PULSE_MS = 120;      // How long each pulse lasts
const uint32_t HAPTIC_MIN_GAP_MS = 700;    // Limit back-to-back pulses
const bool HAPTIC_STARTUP_TEST = false;   // Set true for one test pulse at boot

bool hapticActive = false;
uint32_t hapticStartedMs = 0;
uint32_t lastHapticStartMs = 0;
float longitudeProgressDeg = 0.0f;       // Only modified by network task

// ---------- LIVE ISS API ----------
const char* ISS_URL = "https://api.wheretheiss.at/v1/satellites/25544";
const uint32_t FETCH_INTERVAL_MS = 15000UL;
const uint32_t WIFI_RETRY_MS = 10000UL;
const uint32_t PREDICTION_LIMIT_MS = 20000UL;

// Network calls run on a background FreeRTOS task so LED breathing
// stays smooth even if Wi-Fi or the API takes a few seconds.
portMUX_TYPE issMutex = portMUX_INITIALIZER_UNLOCKED;

struct ISSState {
  float latitude;
  float longitude;
  float position;     // last real longitude mapped onto the outer strip
  float velocity;     // estimated position change in LED units per second
  uint32_t timestamp; // API's UNIX timestamp
  uint32_t receivedMs;
  bool valid;
};

ISSState iss = {0, 0, 0, 0, 0, 0, false};
uint8_t pendingHapticPulses = 0; // Shared between network task and main loop

// Find the signed shortest angular difference (-180..+180 degrees).
// e.g. +179.5 -> -179.5 is +1 degree, not -359 degrees.
float wrappedLongitudeDelta(float degrees) {
  while (degrees > 180.0f) degrees -= 360.0f;
  while (degrees < -180.0f) degrees += 360.0f;
  return degrees;
}

// Start/stop gentle pulses without delay() so both LED strips keep animating.
void updateHaptic(uint32_t now) {
  if (!hapticReady) return;

  if (hapticActive && (uint32_t)(now - hapticStartedMs) >= HAPTIC_PULSE_MS) {
    haptic.setVibrate(0);
    hapticActive = false;
  }

  if (hapticActive || (uint32_t)(now - lastHapticStartMs) < HAPTIC_MIN_GAP_MS) {
    return;
  }

  bool shouldPulse = false;
  portENTER_CRITICAL(&issMutex);
  if (pendingHapticPulses > 0) {
    --pendingHapticPulses;
    shouldPulse = true;
  }
  portEXIT_CRITICAL(&issMutex);

  if (shouldPulse) {
    if (haptic.setVibrate(HAPTIC_INTENSITY)) {
      hapticActive = true;
      hapticStartedMs = now;
      lastHapticStartMs = now;
      Serial.printf("HAPTIC pulse: %u/127 for %lu ms\n",
        (unsigned)HAPTIC_INTENSITY, (unsigned long)HAPTIC_PULSE_MS);
    } else {
      Serial.println("HAPTIC: unable to start pulse (check driver/fault)");
    }
  }
}


// ---------- LIGHT COLORS ----------
// 0xRRGGBB color values avoid custom return types in Arduino's auto-generated prototypes.
const uint32_t PALETTE[] = {
  0x4B96FF, // blue
  0x4BE6E6, // cyan
  0xB982FF, // violet
  0xFF7DB9, // pink
  0xFFC37D, // warm amber
  0xB9D7FF  // cool white
};
const uint8_t COLOR_COUNT = sizeof(PALETTE) / sizeof(PALETTE[0]);

// ---------- INNER STAR SETTINGS ----------
const uint8_t SPARK_COUNT = 2;
const uint32_t SPARK_MIN_MS = 3000UL;
const uint32_t SPARK_MAX_MS = 6500UL;

struct Spark {
  int pixel;
  uint32_t start;
  uint32_t duration;
  float strength;
  uint8_t colorIndex;
};

Spark sparks[SPARK_COUNT] = {
  {-1, 0, 4000, 1.0f, 0},
  {-1, 0, 4000, 1.0f, 1}
};

float displayedPosition = 0.0f;
bool displayedPositionInitialized = false;
uint32_t lastLogMs = 0;

// ---------- POSITION + COLOR HELPERS ----------
float wrapPosition(float pos) {
  while (pos < 0.0f) pos += OUTER_LEDS;
  while (pos >= OUTER_LEDS) pos -= OUTER_LEDS;
  return pos;
}

float wrappedDelta(float delta) {
  float half = OUTER_LEDS * 0.5f;
  while (delta > half) delta -= OUTER_LEDS;
  while (delta < -half) delta += OUTER_LEDS;
  return delta;
}

float longitudeToPosition(float longitude) {
  float pos = (longitude + 180.0f) / 360.0f * OUTER_LEDS;
  if (REVERSE_ORBIT) pos = OUTER_LEDS - pos;
  return wrapPosition(pos);
}

float ease01(float x) {
  x = constrain(x, 0.0f, 1.0f);
  return 0.5f - 0.5f * cosf(PI * x);
}

uint32_t blendColor(uint32_t a, uint32_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  const uint8_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
  const uint8_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
  const uint8_t r = (uint8_t)(ar + ((float)br - ar) * t + 0.5f);
  const uint8_t g = (uint8_t)(ag + ((float)bg - ag) * t + 0.5f);
  const uint8_t bl = (uint8_t)(ab + ((float)bb - ab) * t + 0.5f);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

uint32_t orbitColor(float pos) {
  float colorPos = wrapPosition(pos) / OUTER_LEDS * COLOR_COUNT;
  int first = (int)floorf(colorPos);
  float mix = ease01(colorPos - first);
  return blendColor(PALETTE[first], PALETTE[(first + 1) % COLOR_COUNT], mix);
}

uint32_t scaledColor(Adafruit_NeoPixel& strip, uint32_t c, float level) {
  level = constrain(level, 0.0f, 1.0f);
  return strip.Color(
    (uint8_t)(((c >> 16) & 0xFF) * level + 0.5f),
    (uint8_t)(((c >> 8) & 0xFF) * level + 0.5f),
    (uint8_t)((c & 0xFF) * level + 0.5f)
  );
}

// ---------- INNER: TWO RANDOM BREATHING STARS ----------
void restartSpark(uint8_t which, uint32_t now) {
  int other = (which == 0) ? 1 : 0;
  int choice;
  do {
    choice = random(INNER_LEDS);
  } while (INNER_LEDS > 1 && choice == sparks[other].pixel);

  uint8_t newColor;
  do {
    newColor = random(COLOR_COUNT);
  } while (newColor == sparks[which].colorIndex);

  sparks[which].pixel = choice;
  sparks[which].start = now;
  sparks[which].duration = random(SPARK_MIN_MS, SPARK_MAX_MS + 1);
  sparks[which].strength = random(55, 101) / 100.0f;
  sparks[which].colorIndex = newColor;
}

void drawInnerScattered(uint32_t now) {
  innerStrip.clear();

  for (uint8_t i = 0; i < SPARK_COUNT; ++i) {
    Spark& s = sparks[i];
    if (now - s.start >= s.duration) restartSpark(i, now);

    float phase = (now - s.start) / (float)s.duration;
    float level = (0.5f - 0.5f * cosf(2.0f * PI * phase)) * s.strength;
    innerStrip.setPixelColor(s.pixel,
      scaledColor(innerStrip, PALETTE[s.colorIndex], level));
  }
  innerStrip.show();
}

// ---------- OUTER: ONE REAL-TIME ISS STAR ----------
void drawOuterOrbit(uint32_t now) {
  ISSState latest;
  portENTER_CRITICAL(&issMutex);
  latest = iss;                  // copy all live data atomically
  portEXIT_CRITICAL(&issMutex);

  if (!latest.valid) {
    outerStrip.clear();         // no fake ISS position before first API success
    outerStrip.show();
    return;
  }

  // Estimate motion between API updates using the latest two samples.
  // Stop extrapolating if network data becomes stale.
  uint32_t ageMs = now - latest.receivedMs;
  float ageSeconds = fminf(ageMs, PREDICTION_LIMIT_MS) / 1000.0f;
  float predicted = wrapPosition(latest.position + latest.velocity * ageSeconds);

  // Smooth small corrections when new API data arrives.
  if (!displayedPositionInitialized) {
    displayedPosition = predicted;
    displayedPositionInitialized = true;
  } else {
    displayedPosition = wrapPosition(
      displayedPosition + wrappedDelta(predicted - displayedPosition) * 0.08f
    );
  }

  // Exactly ONE LED lit at any moment; fade to zero halfway between pixels.
  int led = (int)floorf(displayedPosition + 0.5f) % OUTER_LEDS;
  float distance = fabsf(wrappedDelta(displayedPosition - led));
  float travelFade = 0.5f + 0.5f * cosf(2.0f * PI * distance);

  // Gentle independent breathing
  float breathPhase = (now % 5000UL) / 5000.0f;
  float breath = 0.82f +
    0.18f * (0.5f - 0.5f * cosf(2.0f * PI * breathPhase));

  uint32_t c = orbitColor(displayedPosition);
  outerStrip.clear();
  outerStrip.setPixelColor(led, scaledColor(outerStrip, c, travelFade * breath));
  outerStrip.show();
}

// ---------- FETCH LIVE ISS DATA ----------
bool fetchISS() {
  WiFiClientSecure client;
  client.setInsecure();  // prototype only: TLS certificate is NOT verified
  HTTPClient http;
  http.setConnectTimeout(3500);
  http.setTimeout(5000);

  if (!http.begin(client, ISS_URL)) {
    Serial.println("ISS: HTTPS begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ISS HTTP error: %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc; // ArduinoJson v7
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    Serial.printf("ISS JSON error: %s\n", error.c_str());
    return false;
  }
  if (doc["latitude"].isNull() || doc["longitude"].isNull() ||
      doc["timestamp"].isNull()) {
    Serial.println("ISS response missing fields");
    return false;
  }

  float lat = doc["latitude"].as<float>();
  float lon = doc["longitude"].as<float>();
  uint32_t timestamp = doc["timestamp"].as<uint32_t>();
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    Serial.println("ISS response has invalid coordinates");
    return false;
  }

  float newPosition = longitudeToPosition(lon);
  float speed = 0.0f;
  bool freshSample = true;
  int stepsCrossed = 0;
  float remainingDeg = 0.0f;

  portENTER_CRITICAL(&issMutex);
  // Ignore duplicate or out-of-order API samples.
  if (iss.valid && timestamp <= iss.timestamp) {
    freshSample = false;
  } else {
    if (iss.valid) {
      uint32_t deltaSec = timestamp - iss.timestamp;
      if (deltaSec > 0 && deltaSec <= 120) {
        // Movement is based on actual API samples, NOT LED interpolation.
        float deltaLon = wrappedLongitudeDelta(lon - iss.longitude);
        longitudeProgressDeg += fabsf(deltaLon);

        // Preserve the leftover degrees instead of losing progress.
        stepsCrossed = (int)floorf(longitudeProgressDeg / LONGITUDE_STEP_DEG);
        if (stepsCrossed > 0) {
          longitudeProgressDeg -= stepsCrossed * LONGITUDE_STEP_DEG;
          // Cap the queue so a large jump cannot cause endless buzzing.
          int queued = (int)pendingHapticPulses + stepsCrossed;
          pendingHapticPulses = (uint8_t)((queued > 3) ? 3 : queued);
        }

        speed = wrappedDelta(newPosition - iss.position) / (float)deltaSec;
      } else {
        // Network was stale for too long: reset distance to avoid a fake jump.
        longitudeProgressDeg = 0.0f;
      }
    }

    iss.latitude = lat;
    iss.longitude = lon;
    iss.position = newPosition;
    iss.velocity = speed;
    iss.timestamp = timestamp;
    iss.receivedMs = millis();
    iss.valid = true;
    remainingDeg = longitudeProgressDeg;
  }
  portEXIT_CRITICAL(&issMutex);

  if (!freshSample) {
    Serial.println("ISS: duplicate/older timestamp, ignoring sample");
    return false;
  }

  Serial.printf(
    "LIVE ISS lat=%.2f lon=%.2f -> LED pos=%.2f speed=%.4f LEDs/s\n",
    lat, lon, newPosition, speed);
  Serial.printf("Longitude progress: %.2f / %.2f deg (crossed %d thresholds)\n",
    remainingDeg, LONGITUDE_STEP_DEG, stepsCrossed);

  return true;
}

// A separate task prevents HTTP from freezing the NeoPixel animations.
void issNetworkTask(void* ignored) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SECRET_SSID, SECRET_PASSWORD);

  Serial.println("Connecting to Wi-Fi...");
  bool wasConnected = false;
  uint32_t lastRetry = millis();
  uint32_t lastFetch = millis() - FETCH_INTERVAL_MS;

  while (true) {
    uint32_t now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;

    if (connected && !wasConnected) {
      Serial.print("Wi-Fi connected, IP: ");
      Serial.println(WiFi.localIP());
      lastFetch = now - FETCH_INTERVAL_MS;  // first fetch immediately
    } else if (!connected && wasConnected) {
      Serial.println("Wi-Fi disconnected; retrying in background");
    }
    wasConnected = connected;

    if (!connected && (uint32_t)(now - lastRetry) >= WIFI_RETRY_MS) {
      WiFi.reconnect();
      lastRetry = now;
    }

    if (connected && (uint32_t)(now - lastFetch) >= FETCH_INTERVAL_MS) {
      lastFetch = now;
      fetchISS();
    }

    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);

  outerStrip.begin();
  outerStrip.setBrightness(OUTER_BRIGHTNESS);
  outerStrip.clear();
  outerStrip.show();

  innerStrip.begin();
  innerStrip.setBrightness(INNER_BRIGHTNESS);
  innerStrip.clear();
  innerStrip.show();

  // Power the Feather V2 STEMMA QT port, if defined by board variant.
#ifdef NEOPIXEL_I2C_POWER
  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
  delay(15);
#endif

  Wire.begin(SDA, SCL);  // Feather V2: SDA GPIO22, SCL GPIO20
  Wire.setClock(100000);
  if (!haptic.begin()) {
    Serial.println("DA7280 not found at I2C 0x4A. LEDs still work.");
    Serial.println("Check 3V, GND, SDA/SCL or Qwiic cable.");
  } else if (!haptic.defaultMotor()) {
    Serial.println("DA7280 found, but default LRA setup failed.");
  } else {
    haptic.enableFreqTrack(false); // SparkFun recommendation for constrained LRA
    uint8_t irq = (uint8_t)haptic.getIrqEvent();
    if (irq != 0) haptic.clearIrq(irq);
    hapticReady = haptic.setOperationMode(DRO_MODE); // Direct I2C operation
    haptic.setVibrate(0);
    Serial.println(hapticReady ? "DA7280 ready" : "DA7280 mode setup failed");
  }

  if (hapticReady && HAPTIC_STARTUP_TEST) {
    portENTER_CRITICAL(&issMutex);
    pendingHapticPulses = 1;
    portEXIT_CRITICAL(&issMutex);
  }

  randomSeed(micros());
  uint32_t now = millis();
  restartSpark(0, now);
  restartSpark(1, now - 1700UL);

  Serial.printf("LEDs: outer=%d (A0), inner=%d (A1)\n", OUTER_LEDS, INNER_LEDS);
  Serial.println("Mode: REAL ISS API (no saved JSON needed)");

  BaseType_t result = xTaskCreatePinnedToCore(
    issNetworkTask, "issNetwork", 10240, nullptr, 1, nullptr, 0
  );
  if (result != pdPASS) {
    Serial.println("ERROR: could not start ISS network task");
  }
}

// ---------- MAIN LOOP (~50 FPS) ----------
void loop() {
  uint32_t now = millis();
  updateHaptic(now);
  drawInnerScattered(now);
  drawOuterOrbit(now);

  if (now - lastLogMs >= 5000UL) {
    Serial.printf("Inner stars: %d and %d\n",
      sparks[0].pixel + 1, sparks[1].pixel + 1);
    lastLogMs = now;
  }
  delay(20);
}

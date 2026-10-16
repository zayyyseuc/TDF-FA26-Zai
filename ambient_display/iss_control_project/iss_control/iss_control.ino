// ISS Control — Adafruit Feather ESP32 V2
// Based on user's working neopixel_test_fixed(1).ino
// Outer strip: one ISS star with location-based color + breathing
// Inner strip: weather color with continuous breathing brightness
// DA7280: subtle pulse per accumulated longitude change
// Control: ESP32 SoftAP Web UI /api/status and /api/config
// Live ISS HTTPS, or locally recorded ISS data (optional iss_data.h)
// Libraries: Adafruit NeoPixel, ArduinoJson 7, SparkFun Qwiic Haptic Driver DA7280

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include "Haptic_Driver.h"
#include <math.h>
#include "secrets.h"  // SECRET_SSID, SECRET_PASSWORD (upstream Internet Wi-Fi)
#include "web_assets.h"

#if __has_include("iss_data.h")
  #include "iss_data.h"
  #define HAS_REPLAY 1
#else
  #define HAS_REPLAY 0
#endif

// ---- Physical wiring (same as your existing sketch) ----
#define OUTER_PIN 26          // A0 / GPIO26, outer NeoPixel DIN
#define INNER_PIN 25          // A1 / GPIO25, inner NeoPixel DIN
#define OUTER_LEDS 17         // CHANGE to physical number of OUTER LEDs
#define INNER_LEDS 8          // CHANGE to physical number of INNER LEDs (>=2)
const uint8_t OUTER_BRIGHTNESS = 125;
const uint8_t INNER_BRIGHTNESS = 255;
const bool REVERSE_ORBIT = false;

// Change this password (>=8 chars) before presenting publicly.
const char *AP_SSID = "ISS-Control";
const char *AP_PASSWORD = "OrbitDemo26";
const char *ISS_URL = "https://api.wheretheiss.at/v1/satellites/25544";
// Weather is fetched for the ISS ground-track position. Replace the two
// arguments at fetchWeather() if the installation should use a fixed place.
const char *WEATHER_API = "https://api.open-meteo.com/v1/forecast";
const uint32_t FETCH_INTERVAL_MS = 1200UL;  // respect approximately 1 req/s API rate
const uint32_t WEATHER_FETCH_INTERVAL_MS = 60000UL;
const uint32_t WIFI_RETRY_MS = 10000UL;
const uint32_t PREDICTION_LIMIT_MS = 4000UL;
const uint32_t BASE_REPLAY_DURATION_MS = 120000UL; // 1x = 120 s to replay full recording

Adafruit_NeoPixel outerStrip(OUTER_LEDS, OUTER_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel innerStrip(INNER_LEDS, INNER_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);
Haptic_Driver haptic;
bool hapticReady = false;
bool hapticActive = false;
bool hapticEnabled = true;
uint8_t hapticIntensity = 25;                // 0-127 for DA7280
uint32_t hapticStartedMs = 0;
uint32_t lastHapticStartMs = 0;
const uint32_t HAPTIC_PULSE_MS = 120UL;
const uint32_t HAPTIC_MIN_GAP_MS = 700UL;
uint8_t pendingHapticPulses = 0;             // main loop only, capped to 2
float longitudeThresholdDeg = 0.5f;
float longitudeAccumulatedDeg = 0.0f;
float previousLongitude = 0.0f;
bool havePreviousLongitude = false;
uint32_t previousMovementTimestamp = 0;

// ACTIVE_MODE is only a byte, safe to read on the other core.
enum DataMode : uint8_t { MODE_LIVE = 0, MODE_REPLAY = 1 };
volatile uint8_t activeMode = MODE_LIVE;
bool lightsEnabled = true;
float replaySpeed = 1.0f;                    // 1x .. 12x
float replayLightSpeed = 1.0f;               // 0.25x .. 4x, replay lighting animation only
uint32_t replayStartMs = 0;
uint32_t lastReplayCycle = UINT32_MAX;
uint32_t lastReplayTrackMs = 0;
uint32_t lastLiveAppliedTimestamp = 0;
float displayLat = 0.0f, displayLon = 0.0f, currentOrbitPos = 0.0f;
bool haveOrbitPosition = false;
float displayedPosition = 0.0f;
bool displayedPositionInitialized = false;
uint32_t lastLogMs = 0;

portMUX_TYPE issMutex = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE weatherMutex = portMUX_INITIALIZER_UNLOCKED;
struct ISSState {
  float latitude;
  float longitude;
  float position;
  float velocity;
  uint32_t timestamp;
  uint32_t receivedMs;
  bool valid;
};
ISSState iss = {0, 0, 0, 0, 0, 0, false};
struct WeatherState {
  int16_t code;
  float temperature;
  float precipitation;
  uint8_t cloudCover;
  bool isDay;
  uint32_t receivedMs;
  bool valid;
};
WeatherState liveWeather = {-1, 0, 0, 0, true, 0, false};
WeatherState displayedWeather = {-1, 0, 0, 0, true, 0, false};

// Colors identical to your working sketch.
const uint32_t PALETTE[] = {
  0x4B96FF, 0x4BE6E6, 0xB982FF, 0xFF7DB9, 0xFFC37D, 0xB9D7FF
};
const uint8_t COLOR_COUNT = sizeof(PALETTE) / sizeof(PALETTE[0]);
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

float wrappedLongitudeDelta(float degrees) {
  while (degrees > 180.0f) degrees -= 360.0f;
  while (degrees < -180.0f) degrees += 360.0f;
  return degrees;
}
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
float longitudeToPosition(float lon) {
  float p = (lon + 180.0f) / 360.0f * OUTER_LEDS;
  if (REVERSE_ORBIT) p = OUTER_LEDS - p;
  return wrapPosition(p);
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
  float p = wrapPosition(pos) / OUTER_LEDS * COLOR_COUNT;
  int first = (int)floorf(p);
  return blendColor(PALETTE[first], PALETTE[(first + 1) % COLOR_COUNT], ease01(p - first));
}
uint32_t scaledColor(Adafruit_NeoPixel &strip, uint32_t c, float level) {
  level = constrain(level, 0.0f, 1.0f);
  return strip.Color(
    (uint8_t)(((c >> 16) & 0xFF) * level + 0.5f),
    (uint8_t)(((c >> 8) & 0xFF) * level + 0.5f),
    (uint8_t)((c & 0xFF) * level + 0.5f)
  );
}

void restartSpark(uint8_t which, uint32_t now) {
  int other = which == 0 ? 1 : 0;
  int choice;
  do { choice = random(INNER_LEDS); }
  while (INNER_LEDS > 1 && choice == sparks[other].pixel);
  uint8_t newColor;
  do { newColor = random(COLOR_COUNT); }
  while (newColor == sparks[which].colorIndex);
  sparks[which].pixel = choice;
  sparks[which].start = now;
  sparks[which].duration = random(SPARK_MIN_MS, SPARK_MAX_MS + 1);
  sparks[which].strength = random(55, 101) / 100.0f;
  sparks[which].colorIndex = newColor;
}
void drawInnerScattered(uint32_t now) {
  if (!lightsEnabled) {
    innerStrip.clear();
    innerStrip.show();
    return;
  }
  innerStrip.clear();
  for (uint8_t i = 0; i < SPARK_COUNT; i++) {
    Spark &s = sparks[i];
    if ((uint32_t)(now - s.start) >= s.duration) restartSpark(i, now);
    float phase = (now - s.start) / (float)s.duration;
    float level = (0.5f - 0.5f * cosf(2.0f * PI * phase)) * s.strength;
    innerStrip.setPixelColor(s.pixel, scaledColor(innerStrip, PALETTE[s.colorIndex], level));
  }
  innerStrip.show();
}

uint32_t lightAnimationNow(uint32_t now) {
  if (activeMode != MODE_REPLAY) return now;
  return (uint32_t)((double)now * replayLightSpeed);
}

uint32_t weatherColor(int16_t code) {
  if (code == 0) return 0xFFD166;                   // clear
  if (code == 1 || code == 2) return 0xB9D7FF;      // partly cloudy
  if (code == 3) return 0x8D99AE;                   // overcast
  if (code == 45 || code == 48) return 0xC0CAD6;    // fog
  if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82)) return 0x4B96FF; // drizzle/rain
  if (code >= 61 && code <= 67) return 0x3987D6;    // rain
  if (code >= 71 && code <= 77) return 0xB9F2FF;    // snow
  if (code >= 95) return 0xB982FF;                 // thunderstorm
  return 0x6E7F99;                                 // unknown
}
float weatherPulsePeriod(int16_t code) {
  if (code >= 95) return 420.0f;                   // storm flicker
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 2200.0f;
  if (code >= 71 && code <= 77) return 3200.0f;
  return 5200.0f;
}
void drawInnerWeather(uint32_t now) {
  if (!lightsEnabled) {
    innerStrip.clear();
    innerStrip.show();
    return;
  }
  WeatherState weather;
  portENTER_CRITICAL(&weatherMutex);
  weather = displayedWeather;
  portEXIT_CRITICAL(&weatherMutex);
  innerStrip.clear();
  if (!weather.valid) {
    uint32_t neutral = 0x506070;
    uint32_t animationNow = lightAnimationNow(now);
    float level = 0.25f + 0.12f * (0.5f - 0.5f * cosf(2.0f * PI * (animationNow % 5000UL) / 5000.0f));
    for (uint8_t i = 0; i < INNER_LEDS; i++) {
      innerStrip.setPixelColor(i, scaledColor(innerStrip, neutral, level));
    }
    innerStrip.show();
    return;
  }
  // The color stays tied to the weather code, while brightness keeps
  // breathing continuously even when the weather code does not change.
  uint32_t animationNow = lightAnimationNow(now);
  float period = weatherPulsePeriod(weather.code);
  float phase = (animationNow % (uint32_t)period) / period;
  float level = 0.18f + 0.72f * (0.5f - 0.5f * cosf(2.0f * PI * phase));
  if (!weather.isDay) level *= 0.72f;
  if (weather.code >= 95 && (animationNow % 900UL) < 90UL) level = 1.0f;
  uint32_t color = weatherColor(weather.code);
  for (uint8_t i = 0; i < INNER_LEDS; i++) {
    float offset = (i % 2) ? 0.92f : 1.0f;
    innerStrip.setPixelColor(i, scaledColor(innerStrip, color, level * offset));
  }
  innerStrip.show();
}

// Accept longitude samples from the CURRENT source only.
// Prevents +/-180 degree wrap glitches and reset jumps when changing mode.
void trackLongitude(float newLon, uint32_t sampleTimestamp, bool replay) {
  bool reasonable = havePreviousLongitude;
  if (!replay && reasonable) {
    reasonable = sampleTimestamp > previousMovementTimestamp &&
      sampleTimestamp - previousMovementTimestamp <= 120;
  }
  if (reasonable) {
    float delta = fabsf(wrappedLongitudeDelta(newLon - previousLongitude));
    longitudeAccumulatedDeg += delta;
    int steps = (int)floorf(longitudeAccumulatedDeg / longitudeThresholdDeg);
    if (steps > 0) {
      longitudeAccumulatedDeg -= steps * longitudeThresholdDeg;
      // Avoid a long haptic backlog during fast replay.
      pendingHapticPulses = (uint8_t)min(2, (int)pendingHapticPulses + steps);
    }
  } else {
    longitudeAccumulatedDeg = 0.0f;
  }
  previousLongitude = newLon;
  previousMovementTimestamp = sampleTimestamp;
  havePreviousLongitude = true;
}

void resetMotionTracking() {
  havePreviousLongitude = false;
  longitudeAccumulatedDeg = 0.0f;
  pendingHapticPulses = 0;
  if (hapticActive) {
    haptic.setVibrate(0);
    hapticActive = false;
  }
  displayedPositionInitialized = false;
  haveOrbitPosition = false;
  lastLiveAppliedTimestamp = 0;
  lastReplayTrackMs = 0;
  lastReplayCycle = UINT32_MAX;
}

void updateHaptic(uint32_t now) {
  if (!hapticReady || !hapticEnabled) {
    if (hapticReady && hapticActive) haptic.setVibrate(0);
    hapticActive = false;
    pendingHapticPulses = 0;
    return;
  }
  if (hapticActive && (uint32_t)(now - hapticStartedMs) >= HAPTIC_PULSE_MS) {
    haptic.setVibrate(0);
    hapticActive = false;
  }
  if (hapticActive || (uint32_t)(now - lastHapticStartMs) < HAPTIC_MIN_GAP_MS) return;
  if (pendingHapticPulses > 0) {
    pendingHapticPulses--;
    if (haptic.setVibrate(hapticIntensity)) {
      hapticActive = true;
      hapticStartedMs = now;
      lastHapticStartMs = now;
      Serial.printf("DA7280 pulse: %u/127\n", (unsigned)hapticIntensity);
    } else {
      Serial.println("DA7280 vibrate command failed");
    }
  }
}

void drawOuterOrbit(uint32_t now) {
  if (!lightsEnabled) {
    outerStrip.clear();
    outerStrip.show();
    return;
  }
  if (!haveOrbitPosition) {
    outerStrip.clear();
    outerStrip.show();
    return;
  }
  if (!displayedPositionInitialized) {
    displayedPosition = currentOrbitPos;
    displayedPositionInitialized = true;
  } else {
    displayedPosition = wrapPosition(
      displayedPosition + wrappedDelta(currentOrbitPos - displayedPosition) * 0.08f
    );
  }
  int led = (int)floorf(displayedPosition + 0.5f) % OUTER_LEDS;
  float distance = fabsf(wrappedDelta(displayedPosition - led));
  float travelFade = 0.5f + 0.5f * cosf(2.0f * PI * distance);
  uint32_t animationNow = lightAnimationNow(now);
  float breathPhase = (animationNow % 5000UL) / 5000.0f;
  float breath = 0.82f + 0.18f * (0.5f - 0.5f * cosf(2.0f * PI * breathPhase));
  outerStrip.clear();
  outerStrip.setPixelColor(led,
    scaledColor(outerStrip, orbitColor(displayedPosition), travelFade * breath));
  outerStrip.show();
}

// ---- Network task: only network task writes to shared 'iss' ----
bool fetchISS() {
  WiFiClientSecure client;
  client.setInsecure(); // DEMO ONLY: doesn't verify HTTPS certificate
  HTTPClient http;
  http.setConnectTimeout(3500);
  http.setTimeout(5000);
  if (!http.begin(client, ISS_URL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ISS fetch HTTP error: %d\n", code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err || doc["latitude"].isNull() || doc["longitude"].isNull() || doc["timestamp"].isNull()) {
    Serial.println("ISS data invalid");
    return false;
  }
  float lat = doc["latitude"].as<float>();
  float lon = doc["longitude"].as<float>();
  uint32_t ts = doc["timestamp"].as<uint32_t>();
  if (!isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;

  float newPos = longitudeToPosition(lon);
  float velocity = 0;
  bool accepted = true;
  portENTER_CRITICAL(&issMutex);
  if (iss.valid && ts <= iss.timestamp) {
    accepted = false;
  } else {
    if (iss.valid && ts > iss.timestamp && ts - iss.timestamp <= 120) {
      velocity = wrappedDelta(newPos - iss.position) / (float)(ts - iss.timestamp);
    }
    iss.latitude = lat;
    iss.longitude = lon;
    iss.position = newPos;
    iss.velocity = velocity;
    iss.timestamp = ts;
    iss.receivedMs = millis();
    iss.valid = true;
  }
  portEXIT_CRITICAL(&issMutex);
  if (accepted) Serial.printf("LIVE ISS: lat=%.3f lon=%.3f\n", lat, lon);
  return accepted;
}
bool fetchWeather(float latitude, float longitude) {
  WiFiClientSecure client;
  client.setInsecure(); // DEMO ONLY: doesn't verify HTTPS certificate
  HTTPClient http;
  http.setConnectTimeout(3500);
  http.setTimeout(5000);
  String url = String(WEATHER_API) +
    "?latitude=" + String(latitude, 4) +
    "&longitude=" + String(longitude, 4) +
    "&current=weather_code,temperature_2m,precipitation,cloud_cover,is_day&timezone=UTC";
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather fetch HTTP error: %d\n", code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  JsonObject current = doc["current"];
  if (err || current.isNull() || current["weather_code"].isNull()) {
    Serial.println("Weather data invalid");
    return false;
  }
  int weatherCode = current["weather_code"].as<int>();
  float temperature = current["temperature_2m"] | 0.0f;
  float precipitation = current["precipitation"] | 0.0f;
  int cloud = current["cloud_cover"] | 0;
  bool isDay = current["is_day"] | 1;
  if (weatherCode < 0 || weatherCode > 99 || cloud < 0 || cloud > 100) return false;
  portENTER_CRITICAL(&weatherMutex);
  liveWeather.code = (int16_t)weatherCode;
  liveWeather.temperature = temperature;
  liveWeather.precipitation = precipitation;
  liveWeather.cloudCover = (uint8_t)cloud;
  liveWeather.isDay = isDay;
  liveWeather.receivedMs = millis();
  liveWeather.valid = true;
  portEXIT_CRITICAL(&weatherMutex);
  Serial.printf("LIVE WEATHER: code=%d temp=%.1fC cloud=%d%%\n",
    weatherCode, temperature, cloud);
  return true;
}
void issNetworkTask(void *ignored) {
  bool wasConnected = false;
  bool wasLive = false;
  uint32_t lastRetry = millis();
  uint32_t lastFetch = 0;
  uint32_t lastWeatherFetch = 0;
  while (true) {
    uint32_t now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;
    bool isLive = activeMode == MODE_LIVE;
    if (connected && !wasConnected) {
      Serial.print("STA Wi-Fi connected: ");
      Serial.println(WiFi.localIP());
    } else if (!connected && wasConnected) {
      Serial.println("STA Wi-Fi disconnected (AP control remains active)");
    }
    if (!connected && (uint32_t)(now - lastRetry) >= WIFI_RETRY_MS) {
      WiFi.reconnect();
      lastRetry = now;
    }
    if (connected && isLive && (!wasLive || !wasConnected ||
          (uint32_t)(now - lastFetch) >= FETCH_INTERVAL_MS)) {
      lastFetch = now;
      fetchISS();
    }
    if (connected && isLive && (uint32_t)(now - lastWeatherFetch) >= WEATHER_FETCH_INTERVAL_MS) {
      ISSState latest;
      portENTER_CRITICAL(&issMutex);
      latest = iss;
      portEXIT_CRITICAL(&issMutex);
      if (latest.valid) {
        lastWeatherFetch = now;
        fetchWeather(latest.latitude, latest.longitude);
      }
    }
    wasLive = isLive;
    wasConnected = connected;
    vTaskDelay(pdMS_TO_TICKS(120));
  }
}

// ---- Replay sampled positions from REAL historical ISS data ----
#if HAS_REPLAY
void replayAt(uint32_t now) {
  if (ISS_COUNT < 2) {
    haveOrbitPosition = false;
    portENTER_CRITICAL(&weatherMutex);
    displayedWeather.valid = false;
    portEXIT_CRITICAL(&weatherMutex);
    return;
  }
  uint32_t first = ISS_DATA[0].timestamp;
  uint32_t last = ISS_DATA[ISS_COUNT - 1].timestamp;
  if (last <= first) {
    haveOrbitPosition = false;
    portENTER_CRITICAL(&weatherMutex);
    displayedWeather.valid = false;
    portEXIT_CRITICAL(&weatherMutex);
    return;
  }
  uint32_t duration = (uint32_t)(BASE_REPLAY_DURATION_MS / replaySpeed);
  if (duration < 2000) duration = 2000;
  uint32_t elapsed = now - replayStartMs;
  uint32_t cycle = elapsed / duration;
  uint32_t inCycle = elapsed % duration;
  if (cycle != lastReplayCycle) {
    havePreviousLongitude = false;
    longitudeAccumulatedDeg = 0;
    pendingHapticPulses = 0;
    lastReplayCycle = cycle;
    lastReplayTrackMs = 0;
    displayedPositionInitialized = false;
  }
  float ratio = inCycle / (float)duration;
  float virtualT = (last - first) * ratio;
  size_t lo = 0, hi = ISS_COUNT - 1;
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) / 2;
    if ((float)(ISS_DATA[mid].timestamp - first) <= virtualT) lo = mid;
    else hi = mid;
  }
  const ISSPoint &a = ISS_DATA[lo];
  const ISSPoint &b = ISS_DATA[lo + 1];
  float dt = (float)(b.timestamp - a.timestamp);
  float f = dt > 0 ? ((virtualT - (a.timestamp - first)) / dt) : 0.0f;
  f = constrain(f, 0.0f, 1.0f);
  displayLon = a.longitude + wrappedLongitudeDelta(b.longitude - a.longitude) * f;
  displayLon = wrappedLongitudeDelta(displayLon);
  displayLat = a.latitude + (b.latitude - a.latitude) * f;
  currentOrbitPos = longitudeToPosition(displayLon);
  haveOrbitPosition = true;
  WeatherPoint wa = WEATHER_DATA[lo];
  WeatherPoint wb = WEATHER_DATA[lo + 1];
  WeatherState replayWeather;
  if (wa.valid && wb.valid) {
    replayWeather.code = f < 0.5f ? wa.code : wb.code;
    replayWeather.temperature = wa.temperature + (wb.temperature - wa.temperature) * f;
    replayWeather.precipitation = wa.precipitation + (wb.precipitation - wa.precipitation) * f;
    replayWeather.cloudCover = (uint8_t)(wa.cloudCover + (wb.cloudCover - wa.cloudCover) * f + 0.5f);
    replayWeather.isDay = f < 0.5f ? wa.isDay : wb.isDay;
  } else {
    const WeatherPoint &nearest = wa.valid ? wa : wb;
    replayWeather.code = nearest.code;
    replayWeather.temperature = nearest.temperature;
    replayWeather.precipitation = nearest.precipitation;
    replayWeather.cloudCover = nearest.cloudCover;
    replayWeather.isDay = nearest.isDay;
  }
  replayWeather.receivedMs = now;
  replayWeather.valid = wa.valid || wb.valid;
  portENTER_CRITICAL(&weatherMutex);
  displayedWeather = replayWeather;
  portEXIT_CRITICAL(&weatherMutex);
  if ((uint32_t)(now - lastReplayTrackMs) >= 100) {
    trackLongitude(displayLon, 0, true);
    lastReplayTrackMs = now;
  }
}
#endif
void liveAt(uint32_t now) {
  ISSState latest;
  portENTER_CRITICAL(&issMutex);
  latest = iss;
  portEXIT_CRITICAL(&issMutex);
  if (!latest.valid || (uint32_t)(now - latest.receivedMs) > 30000UL) {
    haveOrbitPosition = false;
    portENTER_CRITICAL(&weatherMutex);
    displayedWeather.valid = false;
    portEXIT_CRITICAL(&weatherMutex);
    return;
  }
  if (latest.timestamp != lastLiveAppliedTimestamp) {
    trackLongitude(latest.longitude, latest.timestamp, false);
    lastLiveAppliedTimestamp = latest.timestamp;
  }
  float seconds = fminf((now - latest.receivedMs), PREDICTION_LIMIT_MS) / 1000.0f;
  currentOrbitPos = wrapPosition(latest.position + latest.velocity * seconds);
  displayLat = latest.latitude;
  displayLon = latest.longitude;
  haveOrbitPosition = true;
  WeatherState currentWeather;
  portENTER_CRITICAL(&weatherMutex);
  currentWeather = liveWeather;
  portEXIT_CRITICAL(&weatherMutex);
  if (currentWeather.valid && (uint32_t)(now - currentWeather.receivedMs) <= 180000UL) {
    portENTER_CRITICAL(&weatherMutex);
    displayedWeather = currentWeather;
    portEXIT_CRITICAL(&weatherMutex);
  } else {
    portENTER_CRITICAL(&weatherMutex);
    displayedWeather.valid = false;
    portEXIT_CRITICAL(&weatherMutex);
  }
}

// ---- REST endpoints. Same origin as page, no CORS required. ----
void respondJSON(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", json);
}
void handleStatus() {
  JsonDocument doc;
  doc["mode"] = activeMode == MODE_LIVE ? "live" : "replay";
  doc["hasReplay"] = (bool)HAS_REPLAY;
  doc["staConnected"] = WiFi.status() == WL_CONNECTED;
  doc["staIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["hasPosition"] = haveOrbitPosition;
  if (haveOrbitPosition) {
    doc["latitude"] = displayLat;
    doc["longitude"] = displayLon;
    doc["led"] = ((int)floorf(displayedPosition + 0.5f) % OUTER_LEDS) + 1;
  }
  doc["outerLeds"] = OUTER_LEDS;
  doc["innerLeds"] = INNER_LEDS;
  doc["lightsEnabled"] = lightsEnabled;
  doc["hapticReady"] = hapticReady;
  doc["hapticEnabled"] = hapticEnabled;
  doc["hapticThreshold"] = longitudeThresholdDeg;
  doc["hapticIntensity"] = hapticIntensity;
  doc["hapticProgress"] = longitudeAccumulatedDeg;
  doc["replaySpeed"] = replaySpeed;
  doc["replayLightSpeed"] = replayLightSpeed;
  WeatherState weather;
  portENTER_CRITICAL(&weatherMutex);
  weather = displayedWeather;
  portEXIT_CRITICAL(&weatherMutex);
  doc["weatherValid"] = weather.valid;
  doc["weatherCode"] = weather.valid ? weather.code : -1;
  doc["weatherTemperature"] = weather.valid ? weather.temperature : 0;
  doc["weatherCloudCover"] = weather.valid ? weather.cloudCover : 0;
#if HAS_REPLAY
  doc["samples"] = ISS_COUNT;
  uint32_t duration = (uint32_t)(BASE_REPLAY_DURATION_MS / replaySpeed);
  doc["replayProgress"] = activeMode == MODE_REPLAY && duration > 0 ?
    ((millis() - replayStartMs) % duration) / (float)duration : 0;
#else
  doc["samples"] = 0;
  doc["replayProgress"] = 0;
#endif
  ISSState latest;
  portENTER_CRITICAL(&issMutex);
  latest = iss;
  portEXIT_CRITICAL(&issMutex);
  doc["liveDataAgeSec"] = latest.valid ? (int)((millis() - latest.receivedMs) / 1000) : -1;
  String body;
  serializeJson(doc, body);
  respondJSON(200, body);
}
void handleConfig() {
  if (!server.hasArg("plain")) { respondJSON(400, "{\"error\":\"Missing JSON body\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    respondJSON(400, "{\"error\":\"Invalid JSON\"}"); return;
  }
  if (!doc["mode"].isNull()) {
    String mode = doc["mode"].as<String>();
    if (mode == "replay" && !HAS_REPLAY) {
      respondJSON(409, "{\"error\":\"Replay data missing. Generate iss_data.h first.\"}");
      return;
    }
    if (mode != "live" && mode != "replay") {
      respondJSON(400, "{\"error\":\"mode must be live or replay\"}"); return;
    }
    uint8_t m = mode == "live" ? MODE_LIVE : MODE_REPLAY;
    if (m != activeMode) {
      activeMode = m;
      resetMotionTracking();
      portENTER_CRITICAL(&weatherMutex);
      displayedWeather.valid = false;
      portEXIT_CRITICAL(&weatherMutex);
      replayStartMs = millis();
      Serial.printf("DATA MODE: %s\n", mode.c_str());
    }
  }
  if (!doc["speed"].isNull()) {
    float value = doc["speed"].as<float>();
    if (!isfinite(value) || value < 1 || value > 12) {
      respondJSON(400, "{\"error\":\"speed must be 1..12\"}"); return;
    }
    // Restart at the beginning when changing replay speed.
    replaySpeed = value;
    replayStartMs = millis();
    resetMotionTracking();
  }
  if (!doc["lightSpeed"].isNull()) {
    float value = doc["lightSpeed"].as<float>();
    if (!isfinite(value) || value < 0.25f || value > 4.0f) {
      respondJSON(400, "{\"error\":\"lightSpeed must be 0.25..4\"}"); return;
    }
    replayLightSpeed = value;
  }
  if (!doc["threshold"].isNull()) {
    float value = doc["threshold"].as<float>();
    if (!isfinite(value) || value < 0.1f || value > 10.0f) {
      respondJSON(400, "{\"error\":\"threshold must be 0.1..10 degrees\"}"); return;
    }
    longitudeThresholdDeg = value;
    longitudeAccumulatedDeg = 0;
    pendingHapticPulses = 0;
  }
  if (!doc["intensity"].isNull()) {
    int value = doc["intensity"].as<int>();
    if (value < 1 || value > 127) {
      respondJSON(400, "{\"error\":\"intensity must be 1..127\"}"); return;
    }
    hapticIntensity = (uint8_t)value;
  }
  if (!doc["hapticEnabled"].isNull()) {
    hapticEnabled = doc["hapticEnabled"].as<bool>();
    if (!hapticEnabled) pendingHapticPulses = 0;
  }
  if (!doc["lightsEnabled"].isNull()) {
    lightsEnabled = doc["lightsEnabled"].as<bool>();
    Serial.printf("LIGHTS: %s\n", lightsEnabled ? "ON" : "OFF");
  }
  handleStatus();
}
void handleHapticTest() {
  if (!hapticReady) { respondJSON(409, "{\"error\":\"DA7280 not detected\"}"); return; }
  hapticEnabled = true;
  pendingHapticPulses = 1;
  respondJSON(200, "{\"ok\":true}");
}
void startWebServer() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html; charset=utf-8", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  server.on("/manifest.webmanifest", HTTP_GET, []() {
    server.send(200, "application/manifest+json", MANIFEST_JSON);
  });
  server.on("/sw.js", HTTP_GET, []() {
    server.send(200, "application/javascript", SERVICE_WORKER_JS);
  });
  server.on("/icon-192.png", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "image/png", (PGM_P)ICON192_GZ, ICON192_GZ_LEN);
  });
  server.on("/icon-512.png", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "image/png", (PGM_P)ICON512_GZ, ICON512_GZ_LEN);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/haptic-test", HTTP_POST, handleHapticTest);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  outerStrip.begin();
  outerStrip.setBrightness(OUTER_BRIGHTNESS);
  outerStrip.clear(); outerStrip.show();
  innerStrip.begin();
  innerStrip.setBrightness(INNER_BRIGHTNESS);
  innerStrip.clear(); innerStrip.show();

#ifdef NEOPIXEL_I2C_POWER
  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
  delay(15);
#endif
  Wire.begin(SDA, SCL);
  Wire.setClock(100000);
  if (!haptic.begin()) {
    Serial.println("DA7280 not found, LEDs and Web UI still work");
  } else if (!haptic.defaultMotor()) {
    Serial.println("DA7280 found but motor init failed");
  } else {
    haptic.enableFreqTrack(false);
    uint8_t irq = (uint8_t)haptic.getIrqEvent();
    if (irq != 0) haptic.clearIrq(irq);
    hapticReady = haptic.setOperationMode(DRO_MODE);
    haptic.setVibrate(0);
    Serial.println(hapticReady ? "DA7280 ready" : "DA7280 init mode failed");
  }

  randomSeed(micros());
  uint32_t now = millis();
  restartSpark(0, now);
  restartSpark(1, now - 1700UL);
  replayStartMs = now;

  // Dual network mode: phone always connects to ESP32 AP; STA fetches ISS via Internet.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("Control Wi-Fi: %s\n", AP_SSID);
  Serial.print("Web UI: http://"); Serial.println(WiFi.softAPIP());
  WiFi.setAutoReconnect(true);
  WiFi.begin(SECRET_SSID, SECRET_PASSWORD);
  startWebServer();

  BaseType_t result = xTaskCreatePinnedToCore(
    issNetworkTask, "issNetwork", 10240, nullptr, 1, nullptr, 0);
  if (result != pdPASS) Serial.println("ERROR: network task could not start");

  Serial.printf("NeoPixels: outer=%d (A0), inner=%d (A1)\n", OUTER_LEDS, INNER_LEDS);
  Serial.printf("Recorded replay data: %s\n", HAS_REPLAY ? "yes" : "missing — Live only");
}

void loop() {
  server.handleClient();
  uint32_t now = millis();
  if (activeMode == MODE_LIVE) liveAt(now);
#if HAS_REPLAY
  else replayAt(now);
#endif
  updateHaptic(now);
  drawInnerWeather(now);
  drawOuterOrbit(now);
  if ((uint32_t)(now - lastLogMs) >= 5000UL) {
    Serial.printf("Mode=%s, lon=%.2f, haptic=%.2f/%.2f deg\n",
      activeMode == MODE_LIVE ? "LIVE" : "REPLAY", displayLon,
      longitudeAccumulatedDeg, longitudeThresholdDeg);
    lastLogMs = now;
  }
  delay(20);
}

#include <Adafruit_NeoPixel.h>
#include <math.h>

// 0 = simulated orbit (no data file needed)
// 1 = replay saved ISS track from iss_data.h
#define USE_ISS_REPLAY 0

#if USE_ISS_REPLAY
#include "iss_data.h"
#endif

// ---------- Wiring: Adafruit Feather ESP32 V2 ----------
#define OUTER_PIN 26     // A0 -> outer orbit DIN
#define INNER_PIN 25     // A1 -> inner scattered LEDs DIN
#define OUTER_LEDS 30    // CHANGE to actual outer strip LED count
#define INNER_LEDS 8     // CHANGE to actual inner strip LED count

Adafruit_NeoPixel outerStrip(OUTER_LEDS, OUTER_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel innerStrip(INNER_LEDS, INNER_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t OUTER_BRIGHTNESS = 70;
const uint8_t INNER_BRIGHTNESS = 55;
const uint32_t TEST_ORBIT_MS = 12000UL;
const uint32_t ISS_REPLAY_MS = 60000UL;
const bool REVERSE_ORBIT = false;

// Scattered stars breathe independently and change hue when relocating.
const uint8_t SPARK_COUNT = 2;
const uint32_t SPARK_MIN_MS = 3000UL;
const uint32_t SPARK_MAX_MS = 6500UL;

// Restrained, atmospheric palette: cool blue -> cyan -> lilac ->
// warm pink -> soft amber -> bluish white. The outer star blends
// smoothly between these colors as it moves around the orbit.
struct RGBColor { uint8_t r, g, b; };
const RGBColor PALETTE[] = {
  { 75, 150, 255 },  // blue
  { 75, 230, 230 },  // cyan
  { 185, 130, 255 }, // violet
  { 255, 125, 185 }, // pink
  { 255, 195, 125 }, // warm amber
  { 185, 215, 255 }  // cool white
};
const uint8_t COLOR_COUNT = sizeof(PALETTE) / sizeof(PALETTE[0]);

struct Spark {
  int pixel;
  uint32_t start;
  uint32_t duration;
  float strength;
  uint8_t colorIndex;
};

Spark sparks[SPARK_COUNT] = {
  {-1, 0UL, 4000UL, 1.0f, 0},
  {-1, 0UL, 4000UL, 1.0f, 1}
};

uint32_t startMs = 0;
uint32_t lastLogMs = 0;

float wrapPosition(float pos) {
  while (pos < 0.0f) pos += OUTER_LEDS;
  while (pos >= OUTER_LEDS) pos -= OUTER_LEDS;
  return pos;
}

float wrappedDelta(float delta) {
  const float half = OUTER_LEDS * 0.5f;
  while (delta > half) delta -= OUTER_LEDS;
  while (delta < -half) delta += OUTER_LEDS;
  return delta;
}

float ease01(float x) {
  x = constrain(x, 0.0f, 1.0f);
  return 0.5f - 0.5f * cosf(PI * x);
}

RGBColor blendColor(const RGBColor &a, const RGBColor &b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  return {
    (uint8_t)(a.r + (b.r - a.r) * t + 0.5f),
    (uint8_t)(a.g + (b.g - a.g) * t + 0.5f),
    (uint8_t)(a.b + (b.b - a.b) * t + 0.5f)
  };
}

// Cyclic palette: no abrupt change between the last and first color.
RGBColor orbitColor(float pos) {
  float palettePos = wrapPosition(pos) / OUTER_LEDS * COLOR_COUNT;
  int first = (int)floorf(palettePos);
  float t = ease01(palettePos - first);
  return blendColor(PALETTE[first], PALETTE[(first + 1) % COLOR_COUNT], t);
}

uint32_t scaledColor(Adafruit_NeoPixel &strip, RGBColor color, float level) {
  level = constrain(level, 0.0f, 1.0f);
  return strip.Color(
    (uint8_t)(color.r * level + 0.5f),
    (uint8_t)(color.g * level + 0.5f),
    (uint8_t)(color.b * level + 0.5f)
  );
}

// ---------- Inner: two random LEDs, new color every move ----------
void restartSpark(uint8_t which, uint32_t now) {
  int other = which == 0 ? 1 : 0;
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
  sparks[which].duration = random(SPARK_MIN_MS, SPARK_MAX_MS + 1UL);
  sparks[which].strength = random(55, 101) / 100.0f;
  sparks[which].colorIndex = newColor;
}

void drawInnerScattered(uint32_t now) {
  innerStrip.clear();

  for (uint8_t i = 0; i < SPARK_COUNT; i++) {
    Spark &s = sparks[i];
    if (now - s.start >= s.duration) restartSpark(i, now);

    float phase = (now - s.start) / (float)s.duration;
    // Zero -> full brightness -> zero, then jump to a new location/color.
    float level = 0.5f - 0.5f * cosf(2.0f * PI * phase);
    level *= s.strength;
    innerStrip.setPixelColor(s.pixel,
      scaledColor(innerStrip, PALETTE[s.colorIndex], level));
  }
  innerStrip.show();
}

// ---------- Outer: single moving star, smoothly shifting color ----------
#if USE_ISS_REPLAY
float longitudeToPosition(float longitude) {
  float pos = (longitude + 180.0f) / 360.0f * OUTER_LEDS;
  if (REVERSE_ORBIT) pos = OUTER_LEDS - pos;
  return wrapPosition(pos);
}

float positionFromISS(uint32_t elapsedMs) {
  if (ISS_COUNT < 2) return 0.0f;
  uint32_t first = ISS_DATA[0].timestamp;
  uint32_t last = ISS_DATA[ISS_COUNT - 1].timestamp;
  if (last <= first) return 0.0f;

  float replayPhase = (elapsedMs % ISS_REPLAY_MS) / (float)ISS_REPLAY_MS;
  float virtualSeconds = replayPhase * (last - first);

  size_t i = 0;
  while (i + 1 < ISS_COUNT - 1 &&
    (ISS_DATA[i + 1].timestamp - first) < virtualSeconds) ++i;

  const ISSPoint &a = ISS_DATA[i];
  const ISSPoint &b = ISS_DATA[i + 1];
  float dt = (float)(b.timestamp - a.timestamp);
  float mix = dt > 0.0f
    ? (virtualSeconds - (a.timestamp - first)) / dt : 0.0f;
  mix = constrain(mix, 0.0f, 1.0f);

  float pa = longitudeToPosition(a.longitude);
  float pb = longitudeToPosition(b.longitude);
  return wrapPosition(pa + wrappedDelta(pb - pa) * mix);
}
#endif

void drawOuterOrbit(uint32_t now) {
  uint32_t elapsed = now - startMs;
  float pos;
  float loopFade = 1.0f;

#if USE_ISS_REPLAY
  pos = positionFromISS(elapsed);
  // Fade at recording boundary to hide discontinuity.
  uint32_t phaseMs = elapsed % ISS_REPLAY_MS;
  const float fadeMs = 650.0f;
  loopFade = fminf(ease01(phaseMs / fadeMs),
    ease01((ISS_REPLAY_MS - phaseMs) / fadeMs));
#else
  pos = (elapsed % TEST_ORBIT_MS) / (float)TEST_ORBIT_MS * OUTER_LEDS;
  if (REVERSE_ORBIT) pos = wrapPosition(OUTER_LEDS - pos);
#endif

  // Exactly one LED active; it fades out before the next lights up.
  int led = ((int)floorf(pos + 0.5f)) % OUTER_LEDS;
  float d = fabsf(wrappedDelta(pos - led));
  float travelFade = 0.5f + 0.5f * cosf(2.0f * PI * d);

  float breathPhase = (now % 5000UL) / 5000.0f;
  float breath = 0.82f +
    0.18f * (0.5f - 0.5f * cosf(2.0f * PI * breathPhase));

  float level = constrain(travelFade * breath * loopFade, 0.0f, 1.0f);
  RGBColor color = orbitColor(pos);

  outerStrip.clear();
  outerStrip.setPixelColor(led, scaledColor(outerStrip, color, level));
  outerStrip.show();
}

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

  randomSeed(micros());
  startMs = millis();
  restartSpark(0, startMs);
  restartSpark(1, startMs - 1700UL); // Offset the two breathing phases.

  Serial.printf("NeoPixel ready | outer: %d | inner: %d\n", OUTER_LEDS, INNER_LEDS);
#if USE_ISS_REPLAY
  Serial.printf("Orbit: saved ISS data (%u samples)\n", (unsigned)ISS_COUNT);
#else
  Serial.println("Orbit: simulated 12-second lap");
#endif
}

void loop() {
  uint32_t now = millis();
  drawInnerScattered(now);
  drawOuterOrbit(now);

  if (now - lastLogMs >= 2000UL) {
    Serial.printf("Inner LEDs: %d, %d\n",
      sparks[0].pixel + 1, sparks[1].pixel + 1);
    lastLogMs = now;
  }
  delay(20); // Two independent animations at ~50 frames/s.
}

/*
  Spotify Album-Art Vinyl — Raspberry Pi Pico W + HUB75 64x64

  Hardware:
    Panel: Waveshare P2.5-2020-32S64X64-S2
    Scan:   1/32
    Board:  Raspberry Pi Pico W
    Core:   Earle Philhower arduino-pico

  IMPORTANT:
    Tools -> CPU Speed -> 175 MHz

  HUB75 wiring:
    R1 G1 B1 = GPIO 0, 1, 2
    R2 G2 B2 = GPIO 3, 4, 5
    A B C D E = GPIO 6, 7, 8, 9, 10
    CLK LAT OE = GPIO 11, 12, 13

  This first version is the renderer/animation stage.
  It uses generated artwork so the HUB75 side can be verified
  before adding Spotify authentication and HTTPS artwork download.
*/

#include <Adafruit_Protomatter.h>
#include <math.h>

// ============================================================
// PANEL
// ============================================================

#define WIDTH  64
#define HEIGHT 64

// RGB data pins
uint8_t rgbPins[] = {
  0, 1, 2,   // R1 G1 B1
  3, 4, 5    // R2 G2 B2
};

// Address pins A-E
uint8_t addrPins[] = {
  6, 7, 8, 9, 10
};

// Clock, latch, output-enable
uint8_t clockPin = 11;
uint8_t latchPin = 12;
uint8_t oePin    = 13;

// 4-bit color depth, 64x64, 1/32 scan, 5 address pins,
// double buffering enabled.
Adafruit_Protomatter matrix(
  WIDTH,
  4,              // color depth
  1,              // number of RGB pin groups
  rgbPins,
  5,              // address pins
  addrPins,
  clockPin,
  latchPin,
  oePin,
  true            // double buffering
);

// ============================================================
// ANIMATION SETTINGS
// ============================================================

float angle = 0.0f;

// Start slower. Increase to 20.0 if you want the original
// Python program's default 20 RPM.
float rpm = 8.0f;

unsigned long lastFrameTime = 0;

// ============================================================
// SIMPLE ALBUM-ART DEMO
// ============================================================
//
// This generates fake album artwork directly in the framebuffer.
// Later this function will be replaced by the real Spotify
// JPEG artwork renderer.
//
// The artwork is intentionally colorful so you can immediately
// verify the panel and rotation.
//

uint16_t hsvTo565(float h, float s, float v) {
  while (h < 0)   h += 360.0f;
  while (h >= 360.0f) h -= 360.0f;

  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;

  float r = 0, g = 0, b = 0;

  if (h < 60) {
    r = c; g = x;
  } else if (h < 120) {
    r = x; g = c;
  } else if (h < 180) {
    g = c; b = x;
  } else if (h < 240) {
    g = x; b = c;
  } else if (h < 300) {
    r = x; b = c;
  } else {
    r = c; b = x;
  }

  uint8_t R = (uint8_t)((r + m) * 255.0f);
  uint8_t G = (uint8_t)((g + m) * 255.0f);
  uint8_t B = (uint8_t)((b + m) * 255.0f);

  return matrix.color565(R, G, B);
}

// ============================================================
// DRAW ROTATING RECORD
// ============================================================

void drawRecord(float rotationDegrees) {
  // Clear the framebuffer.
  matrix.fillScreen(0);

  const float cx = 31.5f;
  const float cy = 31.5f;

  // Record radius.
  const float radius = 30.0f;

  // Draw the circular album surface.
  //
  // Instead of rotating an image buffer, each destination pixel
  // is inverse-rotated into our procedural "album art".
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {

      float dx = x - cx;
      float dy = y - cy;

      float distance = sqrtf(dx * dx + dy * dy);

      if (distance > radius) {
        continue;
      }

      // Leave a thin outer border.
      if (distance > radius - 1.5f) {
        matrix.drawPixel(x, y, matrix.color565(8, 8, 8));
        continue;
      }

      // Convert destination position back through rotation.
      float a = atan2f(dy, dx);
      float rotatedAngle = a + rotationDegrees * DEG_TO_RAD;

      float sourceX = cosf(rotatedAngle) * distance;
      float sourceY = sinf(rotatedAngle) * distance;

      // Convert polar position into a colorful procedural artwork.
      float hue =
        atan2f(sourceY, sourceX) * 180.0f / PI
        + 180.0f;

      hue += distance * 3.5f;

      // Add several broad artwork bands.
      float pattern =
        sinf(sourceX * 0.32f) *
        cosf(sourceY * 0.21f);

      hue += pattern * 45.0f;

      float saturation = 0.90f;
      float value = 0.95f;

      // Slight shading toward the outside of the record.
      value *= 0.72f + 0.28f * (1.0f - distance / radius);

      uint16_t color = hsvTo565(hue, saturation, value);
      matrix.drawPixel(x, y, color);
    }
  }

  // ==========================================================
  // VINYL OUTER RING
  // ==========================================================

  // Draw a few dark rings to make the display look more like
  // a physical record.
  for (int ring = 0; ring < 3; ring++) {
    int r = 29 - ring * 2;

    for (int deg = 0; deg < 360; deg += 2) {
      float a = deg * DEG_TO_RAD;

      int x = (int)roundf(cx + cosf(a) * r);
      int y = (int)roundf(cy + sinf(a) * r);

      if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
        matrix.drawPixel(x, y, matrix.color565(5, 5, 5));
      }
    }
  }

  // ==========================================================
  // CENTER LABEL
  // ==========================================================

  const float labelRadius = 7.0f;

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - cx;
      float dy = y - cy;

      if ((dx * dx + dy * dy) <=
          (labelRadius * labelRadius)) {

        float h =
          atan2f(dy, dx) * 180.0f / PI
          + 180.0f;

        uint16_t labelColor =
          hsvTo565(h + 80.0f, 0.85f, 0.90f);

        matrix.drawPixel(x, y, labelColor);
      }
    }
  }

  // Center hole.
  const float holeRadius = 2.0f;

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - cx;
      float dy = y - cy;

      if ((dx * dx + dy * dy) <=
          (holeRadius * holeRadius)) {
        matrix.drawPixel(x, y, 0);
      }
    }
  }

  // Send the completed framebuffer to the panel.
  matrix.show();
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("Pico W HUB75 Album-Art Renderer");
  Serial.println("================================");

  Serial.println("Initializing Protomatter...");

  ProtomatterStatus status = matrix.begin();

  Serial.print("Protomatter status: ");
  Serial.println((int)status);

  if (status != PROTOMATTER_OK) {
    Serial.println("Protomatter initialization FAILED.");
    Serial.println("Check the HUB75 wiring and configuration.");

    while (true) {
      delay(1000);
    }
  }

  // Start with the display blank.
  matrix.fillScreen(0);
  matrix.show();

  lastFrameTime = micros();

  Serial.println("Protomatter initialized.");
  Serial.println("Animation started.");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  unsigned long now = micros();

  // Delta time in seconds.
  float deltaTime =
    (now - lastFrameTime) / 1000000.0f;

  lastFrameTime = now;

  // Protect against a huge jump after startup/debugging.
  if (deltaTime > 0.1f) {
    deltaTime = 0.1f;
  }

  // Same basic rotation calculation as the Python program:
  //
  // angle += degrees_per_second * deltaTime
  //
  // RPM -> revolutions per second -> degrees per second.
  float degreesPerSecond =
    360.0f * (rpm / 60.0f);

  angle += degreesPerSecond * deltaTime;

  while (angle >= 360.0f) {
    angle -= 360.0f;
  }

  drawRecord(angle);

  // The matrix refresh is handled by Protomatter/PIO.
  // We deliberately do not use delay() here so animation
  // stays as smooth as possible.
}

/*
  ================================================================
  SPOTIFY HUB75 CLOCK/DISPLAY - STAGE 8 / v11_2 (bilinear disc sampling)
  Raspberry Pi Pico 2 W + HUB75 64x64 1/32 scan
  Arduino-Pico / Adafruit Protomatter

  WHAT CHANGED FOR v11_2
    The disc appeared to "pause" for a frame right as it crossed
    0/90/180/270 degrees. That wasn't a timing hiccup - the old
    sampler (sampleRecordPixelFast) was nearest-neighbor: each
    screen pixel just rounds to the single closest source pixel.
    At exactly 0/90/180/270 the rotation math has no cross-term
    (cos/sin land on exactly 0 or +-1), so the mapping lines up
    perfectly with the source pixel grid with zero rounding error.
    At every OTHER angle, that same rounding constantly flips
    individual pixels between two source candidates as the angle
    sweeps through, causing a subtle continuous shimmer. The eye
    reads the sudden calm at the cardinal angles as a "pause",
    when really those are the only angles that were ever fully
    stable - everywhere else had a small persistent jitter.

    Fixed by switching the disc's per-pixel sampling from nearest-
    neighbor to bilinear interpolation (sampleRecordPixelBilinear):
    each screen pixel now blends the 4 nearest source pixels by
    fractional distance instead of snapping to one. This removes
    the sudden-clarity spike entirely since there's no longer a
    "perfectly aligned" moment to stand out against - every angle
    now looks equally smooth. Costs roughly 4x the per-pixel sampling
    work (4 reads + weighted blend vs 1 read), which the RP2350's
    FPU handles fine at the current 80 FPS; drop DISPLAY_FPS if it
    ever looks strained on your panel.

  IMPORTANT
    Tools -> Board: Raspberry Pi Pico 2 W
    Tools -> Flash Size: pick one with filesystem space (2MB/2MB)

  HUB75 PINS
    R1,G1,B1,R2,G2,B2 = 0,1,2,3,4,5
    A,B,C,D,E         = 6,7,8,9,10
    CLK,LAT,OE        = 11,12,13
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Crypto.h>
#include <SHA256.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <Adafruit_Protomatter.h>
#include <math.h>
#include <time.h>
#include <Fonts/Picopixel.h>

// ================================================================
// USER SETTINGS
// ================================================================
 
const char* WIFI_SSID     = "yourssid";
const char* WIFI_PASSWORD = "yourpass";
 

const char* SPOTIFY_CLIENT_ID = "your client id";
const char* SPOTIFY_REDIRECT_URI = "http://127.0.0.1:8888/callback";
const char* SPOTIFY_SCOPE = "user-read-currently-playing";

const uint32_t SPOTIFY_POLL_MS = 5000;
const uint16_t DISPLAY_FPS = 80;
const uint16_t TRANSITION_MS = 1300;
const float DISC_DEGREES_PER_SECOND = 22.0f;

const uint16_t TLS_RX_BUF = 4096;
const uint16_t TLS_TX_BUF = 1024;

enum SceneType { SCENE_IDLE, SCENE_DISC, SCENE_WIFI, SCENE_CLOCK };

#define WIDTH 64
#define HEIGHT 64

uint8_t rgbPins[]  = {0,1,2,3,4,5};
uint8_t addrPins[] = {6,7,8,9,10};

Adafruit_Protomatter matrix(
  WIDTH, 4, 1, rgbPins, 5, addrPins,
  11, 12, 13, true
);

uint16_t bufferA[WIDTH * HEIGHT];
uint16_t bufferB[WIDTH * HEIGHT];

uint16_t* currentArtworkPtr = nullptr;
uint16_t currentDominantColor = 0;
bool currentArtworkValid = false;

volatile bool stagingReady = false;
uint16_t* stagingBufferPtr = nullptr;
volatile uint16_t stagingDominantColor = 0;

uint16_t* decodeTargetBuffer = nullptr;

struct DiscPixel {
  int16_t dx256;
  int16_t dy256;
  uint16_t radius256;
};
DiscPixel discPixels[WIDTH * HEIGHT];
bool discGeometryReady = false;

uint16_t jpegW = 0, jpegH = 0;
uint8_t jpegScale = 1;
int16_t jpegCropX = 0, jpegCropY = 0;

bool decodingToNext = false;
bool jpegDecodeOK = false;
uint16_t decodedDominantColor = 0;

volatile bool systemReady = false;
volatile bool wifiReconnecting = false;
volatile bool hasConnectedOnce = false;
volatile bool internetDown = false;
volatile bool currentlyPlaying = false;
volatile bool spotifyAuthenticated = false;

const char* CLOCK_TZ = "IST-5:30";
const float WEATHER_LAT = 26.7271f;
const float WEATHER_LON = 88.3953f;
const uint32_t WEATHER_POLL_MS = 10UL * 60 * 1000;
const uint32_t IDLE_TO_CLOCK_MS = 2UL * 60 * 1000;

volatile float sharedWeatherTempC = 0.0f;
volatile int   sharedWeatherCode  = 3;
volatile bool  sharedWeatherValid = false;

String accessToken;
String refreshToken;
String codeVerifier;

String currentTrack;
String currentArtist;
String currentAlbum;
String currentArtworkURL;
String displayedArtworkURL;

bool artworkDownloadPending = false;

uint32_t lastSpotifyPoll = 0;
uint32_t lastWeatherPoll = 0;

const char* REFRESH_TOKEN_FILE = "/spotify_refresh.txt";

const size_t JPEG_RAM_MAX = 32768;
uint8_t jpegRAMBuffer[JPEG_RAM_MAX];
size_t jpegRAMSize = 0;

WiFiClientSecure spotifyPollClient;
bool spotifyPollClientReady = false;

void initPollClientIfNeeded() {
  if (spotifyPollClientReady) return;
  spotifyPollClient.setInsecure();
  spotifyPollClient.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  spotifyPollClientReady = true;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return matrix.color565(r,g,b); }
uint16_t black() { return rgb565(0,0,0); }

uint16_t spotifyGreen(uint8_t brightness = 255) {
  return rgb565(
    (uint16_t)30 * brightness / 255,
    (uint16_t)215 * brightness / 255,
    (uint16_t)96 * brightness / 255
  );
}

const uint8_t ARTWORK_MAX_BRIGHTNESS = 200;
const float ARTWORK_SHADE_MIN = 0.62f;
const float ARTWORK_SHADE_EDGE = 0.18f;

uint8_t artworkGammaLUT[256];

void prepareArtworkGamma() {
  const float gamma = 1.5f;
  for (int i = 0; i < 256; i++) {
    float n = (float)i / 255.0f;
    float corrected = powf(n, gamma);
    artworkGammaLUT[i] = (uint8_t)roundf(corrected * 255.0f);
  }
}

uint16_t artworkColorCorrect565(uint16_t c, uint16_t brightness) {
  uint8_t r = ((c >> 11) & 31) * 255 / 31;
  uint8_t g = ((c >> 5) & 63) * 255 / 63;
  uint8_t b = (c & 31) * 255 / 31;

  r = artworkGammaLUT[r];
  g = artworkGammaLUT[g];
  b = artworkGammaLUT[b];

  r = (uint16_t)r * brightness / 255;
  g = (uint16_t)g * brightness / 255;
  b = (uint16_t)b * brightness / 255;

  return rgb565(r, g, b);
}

uint16_t scale565(uint16_t c, uint8_t brightness) {
  uint8_t r = ((c >> 11) & 31) * 255 / 31;
  uint8_t g = ((c >> 5) & 63) * 255 / 63;
  uint8_t b = (c & 31) * 255 / 31;
  r = (uint16_t)r * brightness / 255;
  g = (uint16_t)g * brightness / 255;
  b = (uint16_t)b * brightness / 255;
  return rgb565(r, g, b);
}

String jsonStringFrom(const String& json, const String& key, int startPosition) {
  String needle = "\"" + key + "\"";
  int p = json.indexOf(needle, startPosition);
  if (p < 0) return "";
  p += needle.length();
  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (p >= (int)json.length() || json[p] != ':') return "";
  p++;
  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (p >= (int)json.length() || json[p] != '"') return "";
  p++;
  String result;
  while (p < (int)json.length()) {
    char c = json[p++];
    if (c == '"') break;
    if (c == '\\' && p < (int)json.length()) {
      char n = json[p++];
      if (n == '"' || n == '\\' || n == '/') result += n;
      else if (n == 'n') result += '\n';
      else if (n == 'r') result += '\r';
      else if (n == 't') result += '\t';
      else result += n;
    } else {
      result += c;
    }
  }
  return result;
}

bool jsonBool(const String& json, const String& key) {
  String needle = "\"" + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return false;
  p += needle.length();
  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (p >= (int)json.length() || json[p] != ':') return false;
  p++;
  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  return json.startsWith("true", p);
}

String base64UrlEncode(const uint8_t* data, size_t length) {
  const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output;
  for (size_t i = 0; i < length; i += 3) {
    uint32_t value = ((uint32_t)data[i]) << 16;
    if (i + 1 < length) value |= ((uint32_t)data[i + 1]) << 8;
    if (i + 2 < length) value |= data[i + 2];
    output += table[(value >> 18) & 63];
    output += table[(value >> 12) & 63];
    if (i + 1 < length) output += table[(value >> 6) & 63];
    if (i + 2 < length) output += table[value & 63];
  }
  output.replace("+","-");
  output.replace("/","_");
  while (output.endsWith("=")) output.remove(output.length() - 1);
  return output;
}

String generateCodeVerifier() {
  const char chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  String result;
  for (int i = 0; i < 64; i++) result += chars[random(0, sizeof(chars) - 1)];
  return result;
}

String generateCodeChallenge(const String& verifier) {
  SHA256 sha;
  uint8_t hash[32];
  sha.reset();
  sha.update((const uint8_t*)verifier.c_str(), verifier.length());
  sha.finalize(hash, sizeof(hash));
  return base64UrlEncode(hash, sizeof(hash));
}

String spotifyAuthorizationURL() {
  codeVerifier = generateCodeVerifier();
  String challenge = generateCodeChallenge(codeVerifier);
  return String("https://accounts.spotify.com/authorize")
    + "?client_id=" + SPOTIFY_CLIENT_ID
    + "&response_type=code"
    + "&redirect_uri=" + SPOTIFY_REDIRECT_URI
    + "&scope=" + SPOTIFY_SCOPE
    + "&code_challenge_method=S256"
    + "&code_challenge=" + challenge;
}

bool saveRefreshToken() {
  if (!refreshToken.length()) return false;
  LittleFS.remove(REFRESH_TOKEN_FILE);
  File f = LittleFS.open(REFRESH_TOKEN_FILE, "w");
  if (!f) return false;
  f.print(refreshToken);
  f.close();
  Serial.println("Spotify refresh token saved.");
  return true;
}

bool loadRefreshToken() {
  if (!LittleFS.exists(REFRESH_TOKEN_FILE)) return false;
  File f = LittleFS.open(REFRESH_TOKEN_FILE, "r");
  if (!f) return false;
  refreshToken = f.readString();
  f.close();
  refreshToken.trim();
  if (!refreshToken.length()) return false;
  Serial.println("Saved Spotify refresh token found.");
  return true;
}

bool refreshSpotifyAccessToken() {
  if (!refreshToken.length()) return false;
  Serial.println("Refreshing Spotify access token...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(client, "https://accounts.spotify.com/api/token")) {
    Serial.println("Token refresh HTTPS begin failed.");
    return false;
  }

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" + refreshToken +
                "&client_id=" + String(SPOTIFY_CLIENT_ID);

  int status = https.POST(body);
  String response = https.getString();
  https.end();

  Serial.print("Refresh HTTP status: ");
  Serial.println(status);

  if (status != 200) {
    Serial.println(response);
    if (response.indexOf("invalid_grant") >= 0) {
      Serial.println("Saved refresh token is invalid.");
      LittleFS.remove(REFRESH_TOKEN_FILE);
      refreshToken = "";
    }
    return false;
  }

  String newAccess = jsonStringFrom(response, "access_token", 0);
  if (!newAccess.length()) return false;
  accessToken = newAccess;

  String newRefresh = jsonStringFrom(response, "refresh_token", 0);
  if (newRefresh.length()) {
    refreshToken = newRefresh;
    saveRefreshToken();
  }

  spotifyAuthenticated = true;
  Serial.println("Spotify access token refreshed.");
  return true;
}

bool exchangeAuthorizationCode(String code) {
  Serial.println("Exchanging authorization code...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(client, "https://accounts.spotify.com/api/token")) return false;

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=authorization_code&code=" + code +
                "&redirect_uri=" + String(SPOTIFY_REDIRECT_URI) +
                "&client_id=" + String(SPOTIFY_CLIENT_ID) +
                "&code_verifier=" + codeVerifier;

  int status = https.POST(body);
  String response = https.getString();
  https.end();

  Serial.print("Token HTTP status: ");
  Serial.println(status);

  if (status != 200) {
    Serial.println(response);
    return false;
  }

  accessToken = jsonStringFrom(response, "access_token", 0);
  refreshToken = jsonStringFrom(response, "refresh_token", 0);

  if (!accessToken.length() || !refreshToken.length()) {
    Serial.println("Required Spotify tokens were not received.");
    return false;
  }

  spotifyAuthenticated = true;
  saveRefreshToken();

  Serial.println("Spotify authentication SUCCESS.");
  return true;
}

void printSpotifyLoginURL() {
  Serial.println();
  Serial.println("==========================================");
  Serial.println("SPOTIFY FIRST-TIME LOGIN");
  Serial.println("==========================================");
  Serial.println();
  Serial.println(spotifyAuthorizationURL());
  Serial.println();
  Serial.println("Open the URL in Chrome.");
  Serial.println("After redirect, paste the FULL callback URL");
  Serial.println("or only the value after ?code=");
  Serial.println();
}

void handleSpotifyAuthInput() {
  if (!Serial.available()) return;
  String input = Serial.readStringUntil('\n');
  input.trim();
  if (!input.length()) return;

  int p = input.indexOf("?code=");
  if (p >= 0) {
    input = input.substring(p + 6);
    int amp = input.indexOf('&');
    if (amp >= 0) input = input.substring(0, amp);
  }
  exchangeAuthorizationCode(input);
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReconnecting = false;
    return;
  }

  wifiReconnecting = true;
  Serial.println("WiFi not connected. (Re)connecting...");

  WiFi.mode(WIFI_STA);                        // Ensure station mode
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t attemptStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (millis() - attemptStart > 15000) {
      Serial.println("WiFi attempt timed out, retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      attemptStart = millis();
    }
  }

  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  wifiReconnecting = false;
  hasConnectedOnce = true;

  spotifyPollClientReady = false;
}

bool fetchWeatherIntoShared() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather fetch skipped: WiFi not connected.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  HTTPClient https;
  https.setTimeout(8000);

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(WEATHER_LAT, 4) +
               "&longitude=" + String(WEATHER_LON, 4) +
               "&current=temperature_2m,weather_code&timezone=Asia%2FKolkata";

  if (!https.begin(client, url)) {
    Serial.println("Weather fetch: HTTPS begin failed.");
    return false;
  }

  int code = https.GET();
  if (code != 200) {
    Serial.print("Weather fetch failed, HTTP code: ");
    Serial.println(code);
    https.end();
    return false;
  }
  String payload = https.getString();
  https.end();

  int curIdx = payload.indexOf("\"current\":{");
  if (curIdx == -1) {
    Serial.println("Weather fetch: 'current' key not found in response.");
    Serial.println(payload);
    return false;
  }
  String cur = payload.substring(curIdx);

  const char* tKey = "\"temperature_2m\":";
  const char* wKey = "\"weather_code\":";
  int tIdx = cur.indexOf(tKey);
  int wIdx = cur.indexOf(wKey);
  if (tIdx == -1 || wIdx == -1) {
    Serial.println("Weather fetch: temperature/weather_code key missing.");
    return false;
  }

  float tempOut = cur.substring(tIdx + strlen(tKey)).toFloat();
  int codeOut = cur.substring(wIdx + strlen(wKey)).toInt();

  sharedWeatherTempC = tempOut;
  sharedWeatherCode = codeOut;
  sharedWeatherValid = true;

  Serial.print("Weather updated: ");
  Serial.print(tempOut);
  Serial.print(" C, code ");
  Serial.println(codeOut);

  return true;
}

void drawWifiRippleFrame(uint8_t overallBrightness) {
  matrix.fillScreen(black());

  const float cx = 31.5f, cy = 34.0f;
  const float cycleMs = 1100.0f;
  const float minRadius = 4.0f, maxRadius = 22.0f;

  static uint32_t animStart = 0;
  static bool wasReconnecting = false;

  if (wifiReconnecting && !wasReconnecting) {
    animStart = millis();
  }
  wasReconnecting = wifiReconnecting;

  float tMs = (float)(millis() - animStart);

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= 4.0f)
        matrix.drawPixel(x, y, spotifyGreen(overallBrightness));
    }
  }

  for (int ring = 0; ring < 3; ring++) {
    float phaseOffset = (cycleMs / 3.0f) * ring;
    float localT = fmodf(tMs + phaseOffset, cycleMs) / cycleMs;
    float radius = minRadius + localT * (maxRadius - minRadius);
    uint8_t ringBrightness = (uint8_t)((255.0f * (1.0f - localT)) * overallBrightness / 255.0f);

    for (int deg = 210; deg <= 330; deg += 3) {
      float a = deg * DEG_TO_RAD;
      int x = (int)roundf(cx + radius * cosf(a));
      int y = (int)roundf(cy + radius * sinf(a));
      if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        matrix.drawPixel(x, y, spotifyGreen(ringBrightness));
    }
  }

  matrix.show();
}

void drawOfflineCornerBrackets() {
  const uint16_t red = rgb565(255, 30, 30);
  const int arm = 6, thick = 2;
  const int corners[4][4] = {
    {0, 0, 1, 1},
    {WIDTH - 1, 0, -1, 1},
    {0, HEIGHT - 1, 1, -1},
    {WIDTH - 1, HEIGHT - 1, -1, -1},
  };
  for (int c = 0; c < 4; c++) {
    int cx = corners[c][0], cy = corners[c][1], dx = corners[c][2], dy = corners[c][3];
    for (int t = 0; t < thick; t++) {
      for (int i = 0; i < arm; i++) {
        int x1 = cx + dx * i, y1 = cy + dy * t;
        int x2 = cx + dx * t, y2 = cy + dy * i;
        if (x1 >= 0 && x1 < WIDTH && y1 >= 0 && y1 < HEIGHT) matrix.drawPixel(x1, y1, red);
        if (x2 >= 0 && x2 < WIDTH && y2 >= 0 && y2 < HEIGHT) matrix.drawPixel(x2, y2, red);
      }
    }
  }
}

void drawThickPoint(float x, float y, float radius, uint16_t color) {
  int minX = max(0, (int)floorf(x - radius));
  int maxX = min(WIDTH - 1, (int)ceilf(x + radius));
  int minY = max(0, (int)floorf(y - radius));
  int maxY = min(HEIGHT - 1, (int)ceilf(y + radius));

  for (int py = minY; py <= maxY; py++) {
    for (int px = minX; px <= maxX; px++) {
      float dx = px - x, dy = py - y;
      if (dx * dx + dy * dy <= radius * radius)
        matrix.drawPixel(px, py, color);
    }
  }
}

void drawSpotifyArc(float centerY, float width, float curve, float thickness, uint16_t color) {
  const float cx = 31.5f;
  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * width;
    float n = (u - 0.5f) * 2.0f;
    float y = centerY + curve * n * n;
    drawThickPoint(cx + x, y, thickness, color);
  }
}

void drawSpotifyIdle(uint8_t brightness) {
  matrix.fillScreen(black());

  uint16_t green = scale565(rgb565(0, 100, 0), brightness);
  uint16_t waveColor = rgb565(0, 0, 0);

  const float cx = 31.5f;
  const float cy = 31.5f;
  const float radius = 25.0f;

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - cx;
      float dy = y - cy;

      if (dx * dx + dy * dy <= radius * radius)
        matrix.drawPixel(x, y, green);
    }
  }

  float t = millis() * 0.0025f;

  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * 35.0f;
    float n = (u - 0.5f) * 2.0f;
    float y = 24.0f + 4.5f * n * n + sinf(t + n * 4.0f) * 1.0f;
    drawThickPoint(cx + x, y, 2.45f, waveColor);
  }

  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * 32.0f;
    float n = (u - 0.5f) * 2.0f;
    float y = 31.5f + 4.0f * n * n + sinf(t * 1.15f + n * 4.5f + 1.5f) * 1.0f;
    drawThickPoint(cx + x, y, 2.35f, waveColor);
  }

  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * 28.0f;
    float n = (u - 0.5f) * 2.0f;
    float y = 39.0f + 3.2f * n * n + sinf(t * 0.9f + n * 5.0f + 3.0f) * 1.0f;
    drawThickPoint(cx + x, y, 2.20f, waveColor);
  }

  matrix.show();
}

bool jpegCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!decodingToNext) return 0;

  for (uint16_t by = 0; by < h; by++) {
    int16_t destY = y + by - jpegCropY;
    if (destY < 0 || destY >= HEIGHT) continue;

    for (uint16_t bx = 0; bx < w; bx++) {
      int16_t destX = x + bx - jpegCropX;
      if (destX < 0 || destX >= WIDTH) continue;

      decodeTargetBuffer[destY * WIDTH + destX] = bitmap[by * w + bx];
    }
  }
  return 1;
}

void prepareDiscGeometry();
uint16_t calculateDominantArtworkColor(uint16_t* image);

bool decodeNextArtwork() {
  if (jpegRAMSize == 0) return false;

  uint16_t w = 0, h = 0;
  JRESULT sizeResult = TJpgDec.getJpgSize(&w, &h, jpegRAMBuffer, jpegRAMSize);
  if (sizeResult != JDR_OK) {
    Serial.print("JPEG size check failed: ");
    Serial.println((int)sizeResult);
    return false;
  }

  jpegW = w; jpegH = h;

  if (w / 8 >= 64 && h / 8 >= 64) jpegScale = 8;
  else if (w / 4 >= 64 && h / 4 >= 64) jpegScale = 4;
  else if (w / 2 >= 64 && h / 2 >= 64) jpegScale = 2;
  else jpegScale = 1;

  uint16_t decodedW = (w + jpegScale - 1) / jpegScale;
  uint16_t decodedH = (h + jpegScale - 1) / jpegScale;

  jpegCropX = decodedW > 64 ? (decodedW - 64) / 2 : 0;
  jpegCropY = decodedH > 64 ? (decodedH - 64) / 2 : 0;

  Serial.print("JPEG validated: ");
  Serial.print(w); Serial.print("x"); Serial.print(h);
  Serial.print("  scale 1/"); Serial.println(jpegScale);

  TJpgDec.setJpgScale(jpegScale);
  prepareDiscGeometry();

  decodeTargetBuffer = (currentArtworkPtr == bufferA) ? bufferB : bufferA;

  memset(decodeTargetBuffer, 0, WIDTH * HEIGHT * sizeof(uint16_t));
  decodingToNext = true;
  jpegDecodeOK = false;

  JRESULT decodeResult = TJpgDec.drawJpg(0, 0, jpegRAMBuffer, jpegRAMSize);
  decodingToNext = false;

  if (decodeResult != JDR_OK) {
    Serial.print("JPEG decode failed: ");
    Serial.println((int)decodeResult);
    return false;
  }

  jpegDecodeOK = true;
  decodedDominantColor = calculateDominantArtworkColor(decodeTargetBuffer);
  Serial.println("JPEG decode SUCCESS.");
  return true;
}

bool downloadArtworkToRAM() {
  if (!currentArtworkURL.length()) return false;
  Serial.println("Downloading new artwork (RAM)...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(client, currentArtworkURL)) {
    Serial.println("Artwork HTTPS begin failed.");
    return false;
  }

  int status = https.GET();
  if (status != 200) {
    Serial.print("Artwork HTTP status: ");
    Serial.println(status);
    https.end();
    return false;
  }

  int contentLength = https.getSize();
  if (contentLength <= 0 || (size_t)contentLength > JPEG_RAM_MAX) {
    Serial.print("Artwork size rejected: ");
    Serial.println(contentLength);
    https.end();
    return false;
  }

  WiFiClient* stream = https.getStreamPtr();
  int total = 0;
  uint32_t lastData = millis();

  while (https.connected() && total < contentLength) {
    size_t available = stream->available();

    if (available > 0) {
      size_t toRead = min((size_t)(contentLength - total), available);
      int got = stream->readBytes(jpegRAMBuffer + total, toRead);
      if (got > 0) {
        total += got;
        lastData = millis();
      }
    } else {
      if (millis() - lastData > 10000) {
        Serial.println("Artwork download timeout.");
        https.end();
        return false;
      }
      delay(1);
    }
  }

  https.end();
  jpegRAMSize = total;

  Serial.print("Artwork downloaded to RAM: ");
  Serial.print(total);
  Serial.println(" bytes");

  return total > 0;
}

void processArtworkUpdate() {
  if (!artworkDownloadPending) return;
  if (stagingReady) return;

  artworkDownloadPending = false;

  if (!currentlyPlaying) return;
  if (!currentArtworkURL.length()) return;

  if (!downloadArtworkToRAM()) {
    Serial.println("New artwork rejected: download failed.");
    return;
  }

  if (!decodeNextArtwork()) {
    Serial.println("New artwork rejected: JPEG decode failed.");
    return;
  }

  stagingBufferPtr = decodeTargetBuffer;
  stagingDominantColor = decodedDominantColor;
  stagingReady = true;
}

void prepareDiscGeometry() {
  if (discGeometryReady) return;
  const int32_t cx256 = 31 * 256 + 128;
  const int32_t cy256 = 31 * 256 + 128;

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      int i = y * WIDTH + x;
      int32_t dx = x * 256 - cx256;
      int32_t dy = y * 256 - cy256;
      discPixels[i].dx256 = (int16_t)dx;
      discPixels[i].dy256 = (int16_t)dy;
      discPixels[i].radius256 = (uint16_t)sqrtf((float)(dx * dx + dy * dy));
    }
  }
  discGeometryReady = true;
}

uint16_t calculateDominantArtworkColor(uint16_t* image) {
  static uint16_t counts[4096];
  memset(counts, 0, sizeof(counts));

  uint16_t bestBucket = 0;
  uint32_t bestScore = 0;

  for (int i = 0; i < WIDTH * HEIGHT; i++) {
    uint16_t c = image[i];
    uint8_t r = ((c >> 11) & 31) * 255 / 31;
    uint8_t g = ((c >> 5) & 63) * 255 / 63;
    uint8_t b = (c & 31) * 255 / 31;
    uint8_t hi = max(r, max(g, b));
    uint8_t lo = min(r, min(g, b));
    if (hi < 22) continue;

    uint16_t bucket = ((uint16_t)(r >> 4) << 8) | ((uint16_t)(g >> 4) << 4) | (b >> 4);
    if (counts[bucket] < 65535) counts[bucket]++;

    uint32_t score = (uint32_t)counts[bucket] * (64 + (hi - lo));
    if (score > bestScore) {
      bestScore = score;
      bestBucket = bucket;
    }
  }

  if (!bestScore) return rgb565(70, 70, 70);

  uint8_t r = ((bestBucket >> 8) & 15) * 17;
  uint8_t g = ((bestBucket >> 4) & 15) * 17;
  uint8_t b = (bestBucket & 15) * 17;

  uint8_t hi = max(r, max(g, b));
  if (hi < 48) {
    float boost = 48.0f / max(1.0f, (float)hi);
    r = min(110, (int)(r * boost));
    g = min(110, (int)(g * boost));
    b = min(110, (int)(b * boost));
  }

  r = max(10, (int)(r * 0.55f));
  g = max(10, (int)(g * 0.55f));
  b = max(10, (int)(b * 0.55f));

  return rgb565(r, g, b);
}

// Kept for reference/comparison - no longer called from drawRecordFast.
uint16_t sampleRecordPixelFast(uint16_t* image, int index, int32_t cosQ15, int32_t sinQ15) {
  const int32_t cx256 = 31 * 256 + 128;
  const int32_t cy256 = 31 * 256 + 128;
  int32_t dx = discPixels[index].dx256;
  int32_t dy = discPixels[index].dy256;
  int32_t sx256 = cx256 + (dx * cosQ15 - dy * sinQ15) / 32768;
  int32_t sy256 = cy256 + (dx * sinQ15 + dy * cosQ15) / 32768;
  int sx = (sx256 + 128) >> 8;
  int sy = (sy256 + 128) >> 8;
  if (sx < 0 || sx >= WIDTH || sy < 0 || sy >= HEIGHT) return 0;
  return image[sy * WIDTH + sx];
}

static inline uint16_t bilinearGetPixel(uint16_t* image, int x, int y) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return 0;
  return image[y * WIDTH + x];
}

static inline uint8_t bilinearLerp8(uint8_t a, uint8_t b, int frac256) {
  return (uint8_t)(((int)a * (256 - frac256) + (int)b * frac256) >> 8);
}

// Blends the 4 nearest source pixels by fractional distance instead
// of rounding to the single closest one. This is what fixes the
// 0/90/180/270 "pause" - see the header comment for why.
uint16_t sampleRecordPixelBilinear(uint16_t* image, int index, int32_t cosQ15, int32_t sinQ15) {
  const int32_t cx256 = 31 * 256 + 128;
  const int32_t cy256 = 31 * 256 + 128;
  int32_t dx = discPixels[index].dx256;
  int32_t dy = discPixels[index].dy256;
  int32_t sx256 = cx256 + (dx * cosQ15 - dy * sinQ15) / 32768;
  int32_t sy256 = cy256 + (dx * sinQ15 + dy * cosQ15) / 32768;

  int sx0 = sx256 >> 8;
  int sy0 = sy256 >> 8;
  int fx = sx256 & 0xFF;
  int fy = sy256 & 0xFF;

  uint16_t c00 = bilinearGetPixel(image, sx0,     sy0);
  uint16_t c10 = bilinearGetPixel(image, sx0 + 1, sy0);
  uint16_t c01 = bilinearGetPixel(image, sx0,     sy0 + 1);
  uint16_t c11 = bilinearGetPixel(image, sx0 + 1, sy0 + 1);

  uint8_t r00 = ((c00 >> 11) & 31) * 255 / 31, g00 = ((c00 >> 5) & 63) * 255 / 63, b00 = (c00 & 31) * 255 / 31;
  uint8_t r10 = ((c10 >> 11) & 31) * 255 / 31, g10 = ((c10 >> 5) & 63) * 255 / 63, b10 = (c10 & 31) * 255 / 31;
  uint8_t r01 = ((c01 >> 11) & 31) * 255 / 31, g01 = ((c01 >> 5) & 63) * 255 / 63, b01 = (c01 & 31) * 255 / 31;
  uint8_t r11 = ((c11 >> 11) & 31) * 255 / 31, g11 = ((c11 >> 5) & 63) * 255 / 63, b11 = (c11 & 31) * 255 / 31;

  uint8_t rTop = bilinearLerp8(r00, r10, fx), rBot = bilinearLerp8(r01, r11, fx);
  uint8_t gTop = bilinearLerp8(g00, g10, fx), gBot = bilinearLerp8(g01, g11, fx);
  uint8_t bTop = bilinearLerp8(b00, b10, fx), bBot = bilinearLerp8(b01, b11, fx);

  uint8_t r = bilinearLerp8(rTop, rBot, fy);
  uint8_t g = bilinearLerp8(gTop, gBot, fy);
  uint8_t b = bilinearLerp8(bTop, bBot, fy);

  return matrix.color565(r, g, b);
}

const uint32_t DISC_OUTER_RADIUS256   = 30 * 256;
const uint32_t DISC_BORDER_RADIUS256  = 30 * 256 - 384;
const uint32_t DISC_LABEL_RADIUS256   = 7 * 256;
const uint32_t DISC_SPINDLE_RADIUS256 = 2 * 256;

const int RIM_HIGHLIGHT_COUNT = 180;
int16_t rimHighlightX[RIM_HIGHLIGHT_COUNT];
int16_t rimHighlightY[RIM_HIGHLIGHT_COUNT];
bool rimHighlightReady = false;

void prepareRimHighlightLUT() {
  if (rimHighlightReady) return;
  int k = 0;
  for (int deg = 0; deg < 360; deg += 2) {
    float a2 = deg * DEG_TO_RAD;
    rimHighlightX[k] = (int16_t)roundf(31.5f + cosf(a2) * 29.0f);
    rimHighlightY[k] = (int16_t)roundf(31.5f + sinf(a2) * 29.0f);
    k++;
  }
  rimHighlightReady = true;
}

void drawRecordFast(uint16_t* image, float rotationDegrees, uint8_t brightness, uint16_t rimColor) {
  prepareDiscGeometry();
  prepareRimHighlightLUT();
  matrix.fillScreen(0);

  float a = -rotationDegrees * DEG_TO_RAD;
  int32_t cosQ15 = (int32_t)roundf(cosf(a) * 32768.0f);
  int32_t sinQ15 = (int32_t)roundf(sinf(a) * 32768.0f);

  uint16_t effectiveBrightness = (uint16_t)brightness * ARTWORK_MAX_BRIGHTNESS / 255;
  uint16_t rimBrightColor = scale565(rimColor, brightness);

  for (int i = 0; i < WIDTH * HEIGHT; i++) {
    uint32_t r256 = discPixels[i].radius256;
    if (r256 > DISC_OUTER_RADIUS256) continue;

    int x = i % WIDTH, y = i / WIDTH;

    if (r256 > DISC_BORDER_RADIUS256) {
      matrix.drawPixel(x, y, rimBrightColor);
      continue;
    }

    if (r256 <= DISC_SPINDLE_RADIUS256) {
      matrix.drawPixel(x, y, 0);
      continue;
    }

    uint16_t c = sampleRecordPixelBilinear(image, i, cosQ15, sinQ15);

    if (r256 <= DISC_LABEL_RADIUS256) {
      matrix.drawPixel(x, y, artworkColorCorrect565(c, effectiveBrightness));
    } else {
      float shade = ARTWORK_SHADE_MIN + ARTWORK_SHADE_EDGE * (1.0f - (float)r256 / DISC_OUTER_RADIUS256);
      uint8_t br = constrain((int)(shade * effectiveBrightness), 0, 255);
      matrix.drawPixel(x, y, artworkColorCorrect565(c, br));
    }
  }

  uint16_t rimGlowColor = scale565(rimColor, (uint8_t)((uint16_t)brightness * 105 / 255));
  for (int k = 0; k < RIM_HIGHLIGHT_COUNT; k++) {
    matrix.drawPixel(rimHighlightX[k], rimHighlightY[k], rimGlowColor);
  }

  matrix.show();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  randomSeed(micros() ^ analogRead(A0));

  Serial.println();
  Serial.println("========================================");
  Serial.println("PICO 2W SPOTIFY HUB75 - v11_2 (bilinear disc)");
  Serial.println("========================================");

  Serial.print("Reset reason: ");
  Serial.println(rp2040.getResetReason());

  prepareArtworkGamma();

  ProtomatterStatus status = matrix.begin();
  Serial.print("Protomatter status: ");
  Serial.println((int)status);

  if (status != PROTOMATTER_OK) {
    Serial.println("Protomatter FAILED.");
    while (true) delay(1000);
  }

  matrix.fillScreen(black());
  matrix.show();

  Serial.println("Starting LittleFS...");
  if (!LittleFS.begin()) {
    Serial.println("LittleFS FAILED.");
    while (true) delay(1000);
  }
  Serial.println("LittleFS ready.");

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpegCallback);

  systemReady = true;

  ensureWiFiConnected();

  setenv("TZ", CLOCK_TZ, 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  fetchWeatherIntoShared();
  lastWeatherPoll = millis();

  if (loadRefreshToken()) {
    if (refreshSpotifyAccessToken()) {
      Serial.println("SPOTIFY AUTO-LOGIN SUCCESS.");
    } else {
      Serial.println("Saved Spotify login unavailable.");
      printSpotifyLoginURL();
    }
  } else {
    Serial.println("No saved Spotify refresh token.");
    printSpotifyLoginURL();
  }

  lastSpotifyPoll = millis() - SPOTIFY_POLL_MS;
}

void loop() {
  static uint32_t lastHeapLog = 0;
  if (millis() - lastHeapLog > 30000) {
    lastHeapLog = millis();
    Serial.print("Free heap: ");
    Serial.print(rp2040.getFreeHeap());
    Serial.println(" bytes");
  }

  ensureWiFiConnected();

  const uint32_t WEATHER_RETRY_MS = 60UL * 1000;
  uint32_t weatherInterval = sharedWeatherValid ? WEATHER_POLL_MS : WEATHER_RETRY_MS;
  if (millis() - lastWeatherPoll >= weatherInterval) {
    lastWeatherPoll = millis();
    fetchWeatherIntoShared();
  }

  if (!spotifyAuthenticated) {
    handleSpotifyAuthInput();
    delay(5);
    return;
  }

  uint32_t now = millis();
  if (now - lastSpotifyPoll >= SPOTIFY_POLL_MS) {
    lastSpotifyPoll = now;
    Serial.println();
    Serial.println("Checking Spotify...");

    bool connectivityWasBad = wifiReconnecting || internetDown;

    initPollClientIfNeeded();
    HTTPClient https;
    https.setTimeout(8000);

    bool ok = false;
    if (https.begin(spotifyPollClient, "https://api.spotify.com/v1/me/player/currently-playing")) {
      https.addHeader("Authorization", "Bearer " + accessToken);
      int status = https.GET();

      internetDown = (status <= 0);

      String response = https.getString();
      https.end();

      if (status == 401) {
        Serial.println("Spotify access token expired. Refreshing...");
        refreshSpotifyAccessToken();
      } else if (status == 204) {
        bool wasPlaying = currentlyPlaying;
        currentlyPlaying = false;
        artworkDownloadPending = false;
        if (wasPlaying) Serial.println("Spotify paused / nothing playing.");
        ok = true;
      } else if (status == 200) {
        bool oldPlaying = currentlyPlaying;
        String oldArtwork = currentArtworkURL;

        currentlyPlaying = jsonBool(response, "is_playing");

        int item = response.indexOf("\"item\"");
        if (item >= 0) {
          currentTrack = jsonStringFrom(response, "name", item);

          int album = response.indexOf("\"album\"", item);
          currentAlbum = album >= 0 ? jsonStringFrom(response, "name", album) : "";

          int artists = response.indexOf("\"artists\"", item);
          currentArtist = artists >= 0 ? jsonStringFrom(response, "name", artists) : "";

          currentArtworkURL = "";
          if (album >= 0) {
            int images = response.indexOf("\"images\"", album);
            if (images >= 0) {
              int arrayEnd = response.indexOf(']', images);
              int searchPos = images;
              int lastUrl = -1;
              while (true) {
                int url = response.indexOf("\"url\"", searchPos);
                if (url < 0 || url > arrayEnd) break;
                lastUrl = url;
                searchPos = url + 5;
              }
              if (lastUrl >= 0) currentArtworkURL = jsonStringFrom(response, "url", lastUrl);
            }
          }

          if (currentArtworkURL.length() && currentArtworkURL != oldArtwork) {
            artworkDownloadPending = true;
          } else if (connectivityWasBad && currentlyPlaying && currentArtworkURL.length()) {
            artworkDownloadPending = true;
            Serial.println("Reconnected mid-track - forcing artwork refresh.");
          }

          if (oldPlaying != currentlyPlaying || oldArtwork != currentArtworkURL) {
            Serial.println();
            Serial.println("========== NOW PLAYING ==========");
            if (currentlyPlaying) {
              Serial.print("Track : "); Serial.println(currentTrack);
              Serial.print("Artist: "); Serial.println(currentArtist);
              Serial.print("Album : "); Serial.println(currentAlbum);
              Serial.print("Art   : "); Serial.println(currentArtworkURL);
            } else {
              Serial.println("Spotify paused / nothing playing.");
            }
            Serial.println("=================================");
          }
        }
        ok = true;
      } else {
        Serial.print("Spotify API HTTP status: ");
        Serial.println(status);
      }
    } else {
      Serial.println("Spotify poll HTTPS begin failed - resetting poll client.");
      spotifyPollClientReady = false;
      internetDown = true;
    }

    if (ok) Serial.println("Spotify API request completed.");
  }

  processArtworkUpdate();

  delay(1);
}

const char* clockDayNames[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};

const char* weatherLabelForCode(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Fair";
  if (code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
  if (code >= 95) return "Storm";
  if (code >= 71 && code <= 77) return "Snow";
  return "Cloudy";
}

uint16_t clockWheel(uint8_t pos, uint8_t brightness) {
  pos = 255 - pos;
  uint16_t c;
  if (pos < 85)       c = rgb565(255 - pos * 3, 0, pos * 3);
  else if (pos < 170) { pos -= 85;  c = rgb565(0, pos * 3, 255 - pos * 3); }
  else                { pos -= 170; c = rgb565(pos * 3, 255 - pos * 3, 0); }
  return scale565(c, brightness);
}

void drawClockSecondsRing(int seconds, uint8_t brightness) {
  const float cx = 32.0f, cy = 32.0f;
  float endAngle = -90.0f + (seconds / 60.0f) * 360.0f;
  for (float a2 = -90.0f; a2 <= endAngle; a2 += 3.0f) {
    float rad = a2 * DEG_TO_RAD;
    uint8_t hue = (uint8_t)(((a2 + 90.0f) / 360.0f) * 255.0f);
    for (int r = 29; r <= 30; r++) {
      int x = (int)roundf(cx + r * cosf(rad));
      int y = (int)roundf(cy + r * sinf(rad));
      if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        matrix.drawPixel(x, y, clockWheel(hue, brightness));
    }
  }
}

void drawDegreeMark(int x, int y, uint16_t color) {
  matrix.drawPixel(x, y, color);
  matrix.drawPixel(x + 1, y, color);
  matrix.drawPixel(x, y - 1, color);
  matrix.drawPixel(x + 1, y - 1, color);
}

void drawPicoLabelScaled(const char* str, int centerX, int baselineY, uint16_t color, float scale) {
  matrix.setFont(&Picopixel);
  matrix.setTextSize(1);
  int16_t bx, by;
  uint16_t bw, bh;
  matrix.getTextBounds(str, 0, 0, &bx, &by, &bw, &bh);
  matrix.setFont();
  if (bw == 0 || bh == 0) return;

  GFXcanvas1 canvas(bw + 2, bh + 2);
  canvas.setFont(&Picopixel);
  canvas.setTextSize(1);
  canvas.setTextColor(1);
  canvas.setCursor(-bx, -by);
  canvas.print(str);

  int scaledW = (int)roundf(bw * scale);
  int scaledH = (int)roundf(bh * scale);
  int originX = centerX - scaledW / 2;
  int originY = baselineY - scaledH;

  for (int y = 0; y < scaledH; y++) {
    int srcY = (int)(y / scale);
    for (int x = 0; x < scaledW; x++) {
      int srcX = (int)(x / scale);
      if (canvas.getPixel(srcX, srcY)) matrix.drawPixel(originX + x, originY + y, color);
    }
  }
}

void picoLabelBounds(const char* str, float scale, int* outW, int* outH) {
  matrix.setFont(&Picopixel);
  matrix.setTextSize(1);
  int16_t bx, by;
  uint16_t bw, bh;
  matrix.getTextBounds(str, 0, 0, &bx, &by, &bw, &bh);
  matrix.setFont();
  *outW = (int)roundf(bw * scale);
  *outH = (int)roundf(bh * scale);
}

void drawClockScene(uint8_t sceneBrightness) {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  matrix.fillScreen(0);
  drawClockSecondsRing(t->tm_sec, sceneBrightness);

  int hour12 = t->tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ampm = (t->tm_hour < 12) ? "AM" : "PM";

  char timeStr[10];
  if (t->tm_sec % 2 == 0)
    snprintf(timeStr, sizeof(timeStr), "%d:%02d %s", hour12, t->tm_min, ampm);
  else
    snprintf(timeStr, sizeof(timeStr), "%d %02d %s", hour12, t->tm_min, ampm);

  matrix.setFont();
  matrix.setTextSize(1);

  int timeWidth = strlen(timeStr) * 6;
  int timeX = (WIDTH - timeWidth) / 2;
  matrix.setTextColor(scale565(rgb565(0, 255, 180), sceneBrightness));
  matrix.setCursor(timeX, 14);
  matrix.print(timeStr);

  const char* dayName = clockDayNames[t->tm_wday];
  int dayWidth = strlen(dayName) * 6;
  int dayX = (WIDTH - dayWidth) / 2;
  matrix.setTextColor(scale565(rgb565(255, 200, 100), sceneBrightness));
  matrix.setCursor(dayX, 25);
  matrix.print(dayName);

  matrix.drawFastHLine(16, 36, 32, scale565(rgb565(45, 45, 65), sceneBrightness));

  float breathe = 0.7f + 0.3f * sinf(millis() * 0.0022f);
  uint8_t weatherBrightness = (uint8_t)constrain((int)(sceneBrightness * breathe), 0, 255);

  if (sharedWeatherValid) {
    char tempStr[6];
    snprintf(tempStr, sizeof(tempStr), "%d", (int)roundf(sharedWeatherTempC));
    const char* label = weatherLabelForCode(sharedWeatherCode);

    const float weatherScale = 1.3f;
    uint16_t weatherColor = scale565(rgb565(180, 150, 255), weatherBrightness);
    int labelBaselineY = 48;
    int tempBaselineY = 58;

    drawPicoLabelScaled(label, WIDTH / 2, labelBaselineY, weatherColor, weatherScale);
    drawPicoLabelScaled(tempStr, WIDTH / 2, tempBaselineY, weatherColor, weatherScale);

    int tempWidth, tempHeight;
    picoLabelBounds(tempStr, weatherScale, &tempWidth, &tempHeight);
    int degX = WIDTH / 2 + tempWidth / 2 + 1;
    int degY = tempBaselineY - tempHeight - 1;
    if (degX + 1 < WIDTH) drawDegreeMark(degX, degY, weatherColor);
  } else {
    const float weatherScale = 1.3f;
    uint16_t placeholderColor = scale565(rgb565(120, 120, 130), weatherBrightness);
    drawPicoLabelScaled("| - - |", WIDTH / 2, 48, placeholderColor, weatherScale);
    drawPicoLabelScaled("| - - |", WIDTH / 2, 58, placeholderColor, weatherScale);
  }

  if ((wifiReconnecting || internetDown) && hasConnectedOnce) drawOfflineCornerBrackets();

  matrix.show();
}

void setup1() {
}

float discRotation = 0.0f;
SceneType currentScene = SCENE_CLOCK;

bool playbackStoppedTracked = false;
uint32_t stoppedPlayingAt = 0;

SceneType computeIdleOrClockScene() {
  if (!playbackStoppedTracked) {
    stoppedPlayingAt = millis();
    playbackStoppedTracked = true;
  }
  uint32_t elapsed = millis() - stoppedPlayingAt;
  return (elapsed < IDLE_TO_CLOCK_MS) ? SCENE_IDLE : SCENE_CLOCK;
}

SceneType computeFallbackScene() {
  return (currentlyPlaying && currentArtworkValid) ? SCENE_DISC : computeIdleOrClockScene();
}

bool transitionActive = false;
bool transitionIsArtworkSwap = false;
uint32_t transitionStartLocal = 0;

SceneType transitionFromScene = SCENE_IDLE;
uint16_t* transitionFromPtr = nullptr;
uint16_t transitionFromColor = 0;

SceneType transitionToScene = SCENE_IDLE;
uint16_t* transitionToPtr = nullptr;
uint16_t transitionToColor = 0;

void beginTransition(SceneType fromScene, uint16_t* fromPtr, uint16_t fromColor,
                      SceneType toScene, uint16_t* toPtr, uint16_t toColor,
                      bool isArtworkSwap) {
  transitionActive = true;
  transitionIsArtworkSwap = isArtworkSwap;
  transitionStartLocal = millis();
  transitionFromScene = fromScene;
  transitionFromPtr = fromPtr;
  transitionFromColor = fromColor;
  transitionToScene = toScene;
  transitionToPtr = toPtr;
  transitionToColor = toColor;
}

void renderScene(SceneType scene, uint16_t* artPtr, uint16_t artColor, float rotation, uint8_t brightness) {
  if (scene == SCENE_DISC) drawRecordFast(artPtr, rotation, brightness, artColor);
  else if (scene == SCENE_WIFI) drawWifiRippleFrame(brightness);
  else if (scene == SCENE_CLOCK) drawClockScene(brightness);
  else drawSpotifyIdle(brightness);
}

void loop1() {
  if (!systemReady) {
    delay(10);
    return;
  }

  static uint32_t nextFrameUs = 0;
  uint32_t nowUs = micros();
  const uint32_t framePeriodUs = 1000000UL / DISPLAY_FPS;

  if (nextFrameUs == 0) nextFrameUs = nowUs;
  if ((int32_t)(nowUs - nextFrameUs) < 0) return;
  if ((uint32_t)(nowUs - nextFrameUs) > framePeriodUs * 3UL) nextFrameUs = nowUs;
  nextFrameUs += framePeriodUs;

  static uint32_t previousRotationUs = 0;
  uint32_t rotationNowUs = micros();
  if (previousRotationUs == 0) previousRotationUs = rotationNowUs;
  float dt = (rotationNowUs - previousRotationUs) / 1000000.0f;
  previousRotationUs = rotationNowUs;
  if (dt > 0.10f) dt = 0.10f;
  discRotation += DISC_DEGREES_PER_SECOND * dt;
  while (discRotation >= 360.0f) discRotation -= 360.0f;

  static bool previousWifiReconnecting = false;
  static bool previousPlayingForTimer = false;

  if (currentlyPlaying != previousPlayingForTimer) {
    if (!currentlyPlaying) playbackStoppedTracked = false;
    previousPlayingForTimer = currentlyPlaying;
  }

  if (!transitionActive) {
    if (wifiReconnecting != previousWifiReconnecting) {
      if (!hasConnectedOnce || currentScene == SCENE_WIFI) {
        if (wifiReconnecting) {
          beginTransition(currentScene, currentArtworkPtr, currentDominantColor,
                           SCENE_WIFI, nullptr, 0, false);
        } else {
          SceneType wantScene = computeFallbackScene();
          beginTransition(SCENE_WIFI, nullptr, 0,
                           wantScene, currentArtworkPtr, currentDominantColor, false);
        }
      }
      previousWifiReconnecting = wifiReconnecting;

    } else if (stagingReady && currentScene != SCENE_WIFI) {
      if (!currentArtworkValid) {
        currentArtworkPtr = stagingBufferPtr;
        currentDominantColor = stagingDominantColor;
        currentArtworkValid = true;
        currentScene = SCENE_DISC;
        stagingReady = false;
      } else {
        beginTransition(SCENE_DISC, currentArtworkPtr, currentDominantColor,
                         SCENE_DISC, stagingBufferPtr, stagingDominantColor, true);
      }

    } else if (currentScene != SCENE_WIFI) {
      bool offline = (wifiReconnecting || internetDown) && hasConnectedOnce;
      SceneType wantScene = offline ? computeIdleOrClockScene() : computeFallbackScene();
      if (wantScene != currentScene) {
        beginTransition(currentScene, currentArtworkPtr, currentDominantColor,
                         wantScene, currentArtworkPtr, currentDominantColor, false);
      }
    }
  }

  if (transitionActive) {
    uint32_t elapsed = millis() - transitionStartLocal;
    float half = TRANSITION_MS / 2.0f;

    if (elapsed < half) {
      uint8_t b = (uint8_t)(255.0f * (1.0f - (elapsed / half)));
      renderScene(transitionFromScene, transitionFromPtr, transitionFromColor, discRotation, b);
    } else if (elapsed < (uint32_t)TRANSITION_MS) {
      uint8_t b = (uint8_t)(255.0f * ((elapsed - half) / half));
      renderScene(transitionToScene, transitionToPtr, transitionToColor, discRotation, b);
    } else {
      currentScene = transitionToScene;
      if (transitionToScene == SCENE_DISC) {
        currentArtworkPtr = transitionToPtr;
        currentDominantColor = transitionToColor;
        currentArtworkValid = true;
      }
      transitionActive = false;
      if (transitionIsArtworkSwap) stagingReady = false;
    }
    return;
  }

  renderScene(currentScene, currentArtworkPtr, currentDominantColor, discRotation, 255);
}

/*
  ================================================================
  SPOTIFY HUB75 CLOCK/DISPLAY - STAGE 7 (offline indicator)
  Raspberry Pi Pico W + HUB75 64x64 1/32 scan
  Arduino-Pico / Adafruit Protomatter

  ARCHITECTURE
    CORE 0 (setup/loop):   ALL networking. WiFi connect/reconnect,
                            Spotify polling, token auth, artwork
                            download + JPEG decode. NEVER touches
                            the matrix.
    CORE 1 (setup1/loop1): ALL matrix drawing. Disc animation, idle
                            icon, WiFi ripple animation, fade
                            transitions. NEVER touches WiFi/HTTP/
                            LittleFS/String - only reads a handful
                            of shared primitive flags/pointers that
                            core0 writes.

  WHAT CHANGED FROM STAGE 6 (no watchdog)
    1. New cross-core flag hasConnectedOnce: true after the very
       first successful WiFi connection. Distinguishes "still
       booting, never connected yet" from "was connected, just
       dropped" - only the former still gets the full-screen ripple
       takeover.
    2. The full-screen WiFi ripple animation (SCENE_WIFI) is now
       ONLY shown before hasConnectedOnce becomes true - i.e. the
       initial boot connection wait. Once connected at least once,
       a later drop does NOT force a scene switch anymore; whatever
       scene is showing (clock, disc, idle) just keeps rendering.
    3. New drawOfflineCornerBrackets(): small red L-shaped brackets
       at all four corners, drawn on top of the clock scene whenever
       wifiReconnecting is true AND hasConnectedOnce is true. Clears
       automatically the moment WiFi reconnects.
    4. Weather block in drawClockScene() now has a fallback: if
       sharedWeatherValid is false (offline, or the first fetch just
       hasn't landed yet), it shows a neutral "| - - |" placeholder
       on both the label and temperature lines instead of leaving
       that area blank.
    5. Everything else (WiFi reconnect monitor, artwork RAM
       pipeline, transitions, disc rendering, Spotify polling, TLS
       buffer fix) is unchanged from Stage 6.

  IMPORTANT
    Tools -> Board: Raspberry Pi Pico W
    Tools -> CPU Speed: 175 MHz
    Tools -> Flash Size: pick one with filesystem space (e.g. 2MB
    Sketch: 1MB, FS: 1MB) - LittleFS is still used for the saved
    Spotify refresh token.

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
#include <Fonts/Picopixel.h>   // tiny hand-drawn font - stays legible small, unlike downscaling the default font

// ================================================================
// USER SETTINGS
// ================================================================

const char* WIFI_SSID     = "your SSID";
const char* WIFI_PASSWORD = "your Password";

const char* SPOTIFY_CLIENT_ID = "your client ID";
const char* SPOTIFY_REDIRECT_URI = "http://127.0.0.1:8888/callback";
const char* SPOTIFY_SCOPE = "user-read-currently-playing";

const uint32_t SPOTIFY_POLL_MS = 5000;
const uint16_t DISPLAY_FPS = 60;
const uint16_t TRANSITION_MS = 1300;
const float DISC_DEGREES_PER_SECOND = 22.0f;

// Shrunk TLS buffers for all our HTTPS clients. Defaults on
// WiFiClientSecure are ~16KB RX + 16KB TX, which is a lot on a
// chip this size when you're only ever moving small JSON replies
// or a <32KB JPEG. Recv needs to be big enough for one TLS record
// (16KB max by spec) but our actual payloads are tiny, so we can
// go much smaller safely; if you ever see TLS errors, bump these
// back up first before suspecting anything else.
const uint16_t TLS_RX_BUF = 4096;
const uint16_t TLS_TX_BUF = 1024;

// Declared here (not further down) because Arduino auto-generates
// function prototypes and inserts them near the top of the file -
// if this enum were defined later, those hoisted prototypes would
// reference an undeclared type and fail to compile.
enum SceneType { SCENE_IDLE, SCENE_DISC, SCENE_WIFI, SCENE_CLOCK };

// ================================================================
// DISPLAY (CORE 1 OWNS ALL matrix.* CALLS)
// ================================================================

#define WIDTH 64
#define HEIGHT 64

uint8_t rgbPins[]  = {0,1,2,3,4,5};
uint8_t addrPins[] = {6,7,8,9,10};

Adafruit_Protomatter matrix(
  WIDTH, 4, 1, rgbPins, 5, addrPins,
  11, 12, 13, true
);

// ================================================================
// ARTWORK BUFFERS - two fixed static buffers, ping-ponged by
// pointer reassignment instead of memcpy. This avoids any torn-
// frame risk: a pointer write is effectively atomic, a memcpy of
// 8KB across cores is not.
// ================================================================

uint16_t bufferA[WIDTH * HEIGHT];
uint16_t bufferB[WIDTH * HEIGHT];

// Owned by core1 - the artwork currently on screen
uint16_t* currentArtworkPtr = nullptr;
uint16_t currentDominantColor = 0;
bool currentArtworkValid = false;

// Handoff from core0 -> core1
volatile bool stagingReady = false;
uint16_t* stagingBufferPtr = nullptr;
volatile uint16_t stagingDominantColor = 0;

// Owned by core0 - which buffer the next decode writes into
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

// ================================================================
// CROSS-CORE FLAGS
// ================================================================

volatile bool systemReady = false;      // core0 -> core1: matrix is initialized, safe to draw
volatile bool wifiReconnecting = false; // core0 -> core1: show the ripple animation (boot only) / corner brackets (after boot)
volatile bool hasConnectedOnce = false; // core0 -> core1: true after the very first successful WiFi connection
volatile bool currentlyPlaying = false; // core0 -> core1
volatile bool spotifyAuthenticated = false;

// ================================================================
// CLOCK / WEATHER HOMESCREEN (ported from Wifi_Time_Matrix)
// Clock face itself only needs libc time() (safe on either core -
// no WiFi/HTTP/LittleFS/String involved). Weather DOES need HTTP,
// so it is fetched on core0 and handed to core1 as plain primitives
// only (float/int/bool), same pattern as the other cross-core flags.
// ================================================================

const char* CLOCK_TZ = "IST-5:30"; // IST = UTC+5:30, no DST
const float WEATHER_LAT = 26.7271f;
const float WEATHER_LON = 88.3953f;
const uint32_t WEATHER_POLL_MS = 10UL * 60 * 1000; // refetch every 10 min

// How long after playback stops before the homescreen drops from the
// Spotify idle logo down to the full clock/weather screen.
const uint32_t IDLE_TO_CLOCK_MS = 2UL * 60 * 1000; // 2 minutes

volatile float sharedWeatherTempC = 0.0f; // core0 -> core1
volatile int   sharedWeatherCode  = 3;    // core0 -> core1 (Open-Meteo WMO code)
volatile bool  sharedWeatherValid = false; // core0 -> core1

// ================================================================
// SPOTIFY STATE (core0-owned)
// ================================================================

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

// Raw compressed JPEG bytes land here before decoding. Fixed size,
// no malloc/free needed. 32KB comfortably covers a 64x64 thumbnail
// (typically 2-10KB) with margin.
const size_t JPEG_RAM_MAX = 32768;
uint8_t jpegRAMBuffer[JPEG_RAM_MAX];
size_t jpegRAMSize = 0;

// Persistent TLS client for the 5s currently-playing poll, instead
// of constructing + fully handshaking a new one every single poll.
// This is the main fix for repeated alloc/free churn on a small
// heap. Token refresh and artwork download stay as local/one-shot
// clients since those happen far less often, but they still get
// the shrunk buffer sizes below.
WiFiClientSecure spotifyPollClient;
bool spotifyPollClientReady = false;

void initPollClientIfNeeded() {
  if (spotifyPollClientReady) return;
  spotifyPollClient.setInsecure();
  spotifyPollClient.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  spotifyPollClientReady = true;
}

// ================================================================
// COLORS
// ================================================================

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

// ================================================================
// JSON HELPERS (unchanged)
// ================================================================

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

// ================================================================
// PKCE (unchanged)
// ================================================================

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

// ================================================================
// SAVE / LOAD REFRESH TOKEN (core0 only)
// ================================================================

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

// ================================================================
// TOKEN REFRESH / AUTH (core0 only)
// ================================================================

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

// ================================================================
// WIFI CONNECT / RECONNECT MONITOR (core0 only)
// Same code path handles both the initial boot connection and any
// later drop - core1 shows the ripple animation only for the FIRST
// connection (gated on hasConnectedOnce); later drops just draw the
// corner brackets over whatever scene is already showing.
// ================================================================

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReconnecting = false;
    return;
  }

  wifiReconnecting = true;
  Serial.println("WiFi not connected. (Re)connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t attemptStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
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

  // A reconnect means the old TLS session on the persistent poll
  // client is no longer valid - force it to be rebuilt on next use.
  spotifyPollClientReady = false;
}

// ================================================================
// WEATHER FETCH (core0 only) - HTTPS, same client pattern used
// elsewhere in this file (setInsecure + shared TLS buffer sizes).
// Switched from plain HTTP: port 80 traffic is far more likely to be
// blocked/redirected by a router or ISP than 443, which is the most
// likely reason this was working one day and silently failing the
// next. Writes only primitive values into the shared* volatiles
// above - never touches core1's matrix and never hands core1 a
// String.
// ================================================================

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
    Serial.println(code); // negative values are HTTPClient error codes, e.g. -1 = connection failed/timeout
    https.end();
    return false;
  }
  String payload = https.getString();
  https.end();

  // "current_units" appears before "current" with the same key names,
  // so search only inside "current":{...} or indexOf can match the
  // units entry instead of the real reading.
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

// ================================================================
// WIFI RIPPLE ANIMATION (core1 only) - shown during the initial
// boot connection wait only (see loop1() gating on hasConnectedOnce)
// ================================================================

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

// ================================================================
// OFFLINE CORNER BRACKETS (core1 only)
// Small red L-shaped brackets at all four corners, drawn ON TOP of
// the clock scene whenever wifiReconnecting is true AND we've
// already connected once before (i.e. this is a runtime drop, not
// the initial boot wait, which still uses the full ripple instead).
// Same visual language as the ripple animation's colour but small
// enough not to obscure the clock underneath.
// ================================================================

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

// ================================================================
// SPOTIFY IDLE SCREEN (core1 only)
// ================================================================

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

  // Spotify green circle
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - cx;
      float dy = y - cy;

      if (dx * dx + dy * dy <= radius * radius)
        matrix.drawPixel(x, y, green);
    }
  }

  // ------------------------------------------------------------
  // ANIMATED WAVES
  // ------------------------------------------------------------

  float t = millis() * 0.0025f;

  // Wave 1
  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * 35.0f;
    float n = (u - 0.5f) * 2.0f;

    float y =
      24.0f
      + 4.5f * n * n
      + sinf(t + n * 4.0f) * 1.0f;

    drawThickPoint(cx + x, y, 2.45f, waveColor);
  }

  // Wave 2
  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * 32.0f;
    float n = (u - 0.5f) * 2.0f;

    float y =
      31.5f
      + 4.0f * n * n
      + sinf(t * 1.15f + n * 4.5f + 1.5f) * 1.0f;

    drawThickPoint(cx + x, y, 2.35f, waveColor);
  }

  // Wave 3
  for (float u = 0.0f; u <= 1.0f; u += 0.008f) {
    float x = (u - 0.5f) * 28.0f;
    float n = (u - 0.5f) * 2.0f;

    float y =
      39.0f
      + 3.2f * n * n
      + sinf(t * 0.9f + n * 5.0f + 3.0f) * 1.0f;

    drawThickPoint(cx + x, y, 2.20f, waveColor);
  }

  matrix.show();
}

// ================================================================
// JPEG CALLBACK (core0 only - runs during decodeNextArtwork)
// ================================================================

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

// ================================================================
// PREPARE + DECODE NEXT ARTWORK (core0 only, from RAM)
// ================================================================

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

  // decodeTargetBuffer is always the buffer NOT currently on screen
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

// ================================================================
// DOWNLOAD ARTWORK INTO RAM (core0 only)
// No LittleFS writes at all - this is what actually fixes the
// flicker, since flash writes halt code execution regardless of
// how the surrounding code is structured.
// ================================================================

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

// ================================================================
// ARTWORK PROCESSOR (core0 only)
// Gated on !stagingReady so we never overwrite decodeTargetBuffer
// while core1 is still mid-transition consuming the previous one.
// ================================================================

void processArtworkUpdate() {
  if (!artworkDownloadPending) return;
  if (stagingReady) return; // core1 hasn't consumed the last one yet

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
  stagingReady = true; // hand off to core1
}

// ================================================================
// DISC GEOMETRY + DOMINANT COLOR (called from core0 during decode)
// ================================================================

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

// ================================================================
// DISC RENDERING (core1 only)
// brightness parameter drives both normal display AND the simple
// fade-out/fade-in transition - no per-pixel blending needed.
// ================================================================

void drawRecordFast(uint16_t* image, float rotationDegrees, uint8_t brightness, uint16_t rimColor) {
  prepareDiscGeometry();
  matrix.fillScreen(0);

  const uint32_t radius256 = 30 * 256;
  const uint32_t border256 = 30 * 256 - 384;

  // Negated: the sampling math below is an inverse-rotation lookup, so
  // feeding it +rotationDegrees made the artwork visibly spin clockwise.
  // Negating here flips the visible spin to anticlockwise without
  // touching how discRotation itself accumulates in loop1.
  float a = -rotationDegrees * DEG_TO_RAD;
  int32_t cosQ15 = (int32_t)roundf(cosf(a) * 32768.0f);
  int32_t sinQ15 = (int32_t)roundf(sinf(a) * 32768.0f);

  for (int i = 0; i < WIDTH * HEIGHT; i++) {
    uint32_t r256 = discPixels[i].radius256;
    if (r256 > radius256) continue;

    int x = i % WIDTH, y = i / WIDTH;

    if (r256 > border256) {
      matrix.drawPixel(x, y, scale565(rimColor, brightness));
      continue;
    }

    uint16_t c = sampleRecordPixelFast(image, i, cosQ15, sinQ15);
    float shade = ARTWORK_SHADE_MIN + ARTWORK_SHADE_EDGE * (1.0f - (float)r256 / radius256);
    uint16_t effectiveBrightness = (uint16_t)brightness * ARTWORK_MAX_BRIGHTNESS / 255;
    uint8_t br = constrain((int)(shade * effectiveBrightness), 0, 255);

    matrix.drawPixel(x, y, artworkColorCorrect565(c, br));
  }

  for (int deg = 0; deg < 360; deg += 2) {
    float a2 = deg * DEG_TO_RAD;
    int x = (int)roundf(31.5f + cosf(a2) * 29.0f);
    int y = (int)roundf(31.5f + sinf(a2) * 29.0f);
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
      matrix.drawPixel(x, y, scale565(rimColor, (uint8_t)((uint16_t)brightness * 105 / 255)));
  }

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - 31.5f, dy = y - 31.5f;
      if (dx * dx + dy * dy <= 49.0f) {
        int i = y * WIDTH + x;
        uint16_t c = sampleRecordPixelFast(image, i, cosQ15, sinQ15);
        matrix.drawPixel(x, y, artworkColorCorrect565(c, (uint16_t)brightness * ARTWORK_MAX_BRIGHTNESS / 255));
      }
    }
  }

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      float dx = x - 31.5f, dy = y - 31.5f;
      if (dx * dx + dy * dy <= 4.0f) matrix.drawPixel(x, y, 0);
    }
  }

  matrix.show();
}

// ================================================================
// LOOP - CORE 0 (networking only, never touches matrix)
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(1200);
  randomSeed(micros() ^ analogRead(A0));

  Serial.println();
  Serial.println("========================================");
  Serial.println("PICO W SPOTIFY HUB75 - STAGE 7 (offline indicator)");
  Serial.println("========================================");

  // No watchdog anywhere in this build. If a reset happens on its
  // own (brownout, crash, power blip) this tells you which on the
  // NEXT boot - it's diagnostic only, doesn't reset anything itself.
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

  systemReady = true; // core1 can start drawing now (will show WiFi animation)

  ensureWiFiConnected(); // blocks here until connected, core1 shows the ripple meanwhile

  // Clock/weather homescreen setup - non-blocking. time() will just read
  // 1970 until NTP catches up in the background over the next few
  // seconds; that's fine, the clock screen isn't shown for the first
  // IDLE_TO_CLOCK_MS anyway.
  setenv("TZ", CLOCK_TZ, 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  fetchWeatherIntoShared(); // best-effort, retried periodically in loop()
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
  // Diagnostic: watch this in Serial Monitor over time. If it trends
  // steadily downward over hours, that confirms heap fragmentation
  // from the repeated String/JSON/TLS churn as the cause of the freeze.
  static uint32_t lastHeapLog = 0;
  if (millis() - lastHeapLog > 30000) {
    lastHeapLog = millis();
    Serial.print("Free heap: ");
    Serial.print(rp2040.getFreeHeap());
    Serial.println(" bytes");
  }

  ensureWiFiConnected(); // cheap no-op check when already connected

  // On failure, retry in 1 minute instead of waiting the full interval -
  // a transient network hiccup shouldn't mean 10 minutes with no reading.
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

    initPollClientIfNeeded();
    HTTPClient https;
    https.setTimeout(8000);

    bool ok = false;
    if (https.begin(spotifyPollClient, "https://api.spotify.com/v1/me/player/currently-playing")) {
      https.addHeader("Authorization", "Bearer " + accessToken);
      int status = https.GET();
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
              // Spotify lists images largest-first; take the LAST url
              // in the array (smallest, usually 64x64) instead of the
              // first (largest, ~640x640) - far less to download/decode.
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
      spotifyPollClientReady = false; // force rebuild next time
    }

    if (ok) Serial.println("Spotify API request completed.");
  }

  processArtworkUpdate();

  delay(1);
}

// ================================================================
// CLOCK / WEATHER SCENE (core1 only)
// Reads time() (libc, safe on either core) and the shared* weather
// primitives written by core0's fetchWeatherIntoShared(). Never
// touches WiFi/HTTP/LittleFS/String.
// ================================================================

const char* clockDayNames[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};

// Short weather labels, kept tight so "temp label" always fits the
// 64px width alongside a 3-char temperature (e.g. "-20 P.Cldy").
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

// Small hollow "degree" mark drawn by hand since neither font used below
// has a glyph for it - keeps "22 Cloudy" reading as "22deg Cloudy".
void drawDegreeMark(int x, int y, uint16_t color) {
  matrix.drawPixel(x, y, color);
  matrix.drawPixel(x + 1, y, color);
  matrix.drawPixel(x, y - 1, color);
  matrix.drawPixel(x + 1, y - 1, color);
}

// Renders text with the tiny Picopixel font, scaled UP slightly (not
// down) via a 1-bit offscreen canvas. This is the same technique the
// original clock used for its date line - Picopixel is hand-drawn to
// be legible at a few pixels tall, which is why it stays readable here
// where shrinking the normal 5x7 font down would not.
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

// Pixel width/height a string would occupy if rendered with
// drawPicoLabelScaled at the given scale - used to place the degree
// mark without re-rendering the text.
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

  // Time + day: same font, same size, same y-positions as the original
  // clock (14 / 25) - that layout already sits cleanly inside the ring.
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

  // ---- Breathing temperature + weather line, in place of the date ----
  // Floor kept high (0.7) so the pulse reads as a gentle "breathing"
  // glow rather than dipping dim enough to hurt readability.
  float breathe = 0.7f + 0.3f * sinf(millis() * 0.0022f);
  uint8_t weatherBrightness = (uint8_t)constrain((int)(sceneBrightness * breathe), 0, 255);

  if (sharedWeatherValid) {
    char tempStr[6];
    snprintf(tempStr, sizeof(tempStr), "%d", (int)roundf(sharedWeatherTempC));
    const char* label = weatherLabelForCode(sharedWeatherCode);

    // Two stacked lines - label on top, temperature below - centered in
    // the lower part of the ring where the available chord width narrows.
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
    // No weather reading available (offline, or the very first fetch
    // just hasn't landed yet since boot) - neutral placeholder instead
    // of leaving this area blank, so it's clear something's missing
    // rather than looking like a rendering bug.
    const float weatherScale = 1.3f;
    uint16_t placeholderColor = scale565(rgb565(120, 120, 130), weatherBrightness);
    drawPicoLabelScaled("| - - |", WIDTH / 2, 48, placeholderColor, weatherScale);
    drawPicoLabelScaled("| - - |", WIDTH / 2, 58, placeholderColor, weatherScale);
  }

  // Small red corner brackets on top of everything above whenever WiFi
  // is down AFTER the initial boot connection - see drawOfflineCornerBrackets()
  // for why this is gated on hasConnectedOnce.
  if (wifiReconnecting && hasConnectedOnce) drawOfflineCornerBrackets();

  matrix.show();
}

// ================================================================
// SETUP1 / LOOP1 - CORE 1 (rendering only, never touches
// WiFi/HTTP/LittleFS/String)
// ================================================================

void setup1() {
  // core0's setup() handles matrix.begin() - we just wait for
  // systemReady before drawing anything.
}

float discRotation = 0.0f;
SceneType currentScene = SCENE_CLOCK;

// Tracks how long playback has been stopped, so the homescreen can show
// the Spotify idle logo first and only drop to the clock/weather screen
// after IDLE_TO_CLOCK_MS of continued silence.
bool playbackStoppedTracked = false;   // core1-local: has stoppedPlayingAt been set yet
uint32_t stoppedPlayingAt = 0;

// Scene to show when nothing is currently playing (idle logo vs clock).
SceneType computeIdleOrClockScene() {
  if (!playbackStoppedTracked) {
    stoppedPlayingAt = millis();
    playbackStoppedTracked = true;
  }
  uint32_t elapsed = millis() - stoppedPlayingAt;
  return (elapsed < IDLE_TO_CLOCK_MS) ? SCENE_IDLE : SCENE_CLOCK;
}

bool transitionActive = false;
bool transitionIsArtworkSwap = false; // whether to clear stagingReady when this transition finishes
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

  // Fixed-rate frame scheduler
  static uint32_t nextFrameUs = 0;
  uint32_t nowUs = micros();
  const uint32_t framePeriodUs = 1000000UL / DISPLAY_FPS;

  if (nextFrameUs == 0) nextFrameUs = nowUs;
  if ((int32_t)(nowUs - nextFrameUs) < 0) return;
  if ((uint32_t)(nowUs - nextFrameUs) > framePeriodUs * 3UL) nextFrameUs = nowUs;
  nextFrameUs += framePeriodUs;

  // Rotation timing - always advances, regardless of what's on screen,
  // so motion never jumps when returning to the disc.
  static uint32_t previousRotationUs = 0;
  uint32_t rotationNowUs = micros();
  if (previousRotationUs == 0) previousRotationUs = rotationNowUs;
  float dt = (rotationNowUs - previousRotationUs) / 1000000.0f;
  previousRotationUs = rotationNowUs;
  if (dt > 0.10f) dt = 0.10f;
  discRotation += DISC_DEGREES_PER_SECOND * dt;
  while (discRotation >= 360.0f) discRotation -= 360.0f;

  // ---- Decide whether a new transition needs to start ----
  static bool previousWifiReconnecting = false;
  static bool previousPlayingForTimer = false;

  // Reset the idle/clock timer exactly when playback actually stops -
  // restarts the 2-minute countdown fresh each time, not just at boot.
  if (currentlyPlaying != previousPlayingForTimer) {
    if (!currentlyPlaying) playbackStoppedTracked = false; // re-armed on next computeIdleOrClockScene() call
    previousPlayingForTimer = currentlyPlaying;
  }

  if (!transitionActive) {
    if (wifiReconnecting != previousWifiReconnecting) {
      // Full-screen ripple takeover is reserved for the VERY FIRST boot
      // connection wait only. Once connected at least once, a later drop
      // does NOT force a scene switch - the clock (or whatever's on
      // screen) keeps rendering as normal, and drawOfflineCornerBrackets()
      // inside drawClockScene() is the only visual change.
      if (!hasConnectedOnce || currentScene == SCENE_WIFI) {
        if (wifiReconnecting) {
          beginTransition(currentScene, currentArtworkPtr, currentDominantColor,
                           SCENE_WIFI, nullptr, 0, false);
        } else {
          SceneType wantScene = (currentlyPlaying && currentArtworkValid) ? SCENE_DISC : computeIdleOrClockScene();
          beginTransition(SCENE_WIFI, nullptr, 0,
                           wantScene, currentArtworkPtr, currentDominantColor, false);
        }
      }
      previousWifiReconnecting = wifiReconnecting;

    } else if (stagingReady && currentScene != SCENE_WIFI) {
      if (!currentArtworkValid) {
        // first artwork ever - show immediately, no fade needed
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
      SceneType wantScene = (currentlyPlaying && currentArtworkValid) ? SCENE_DISC : computeIdleOrClockScene();
      if (wantScene != currentScene) {
        beginTransition(currentScene, currentArtworkPtr, currentDominantColor,
                         wantScene, currentArtworkPtr, currentDominantColor, false);
      }
    }
  }

  // ---- Render ----
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
      if (transitionIsArtworkSwap) stagingReady = false; // release core0
    }
    return;
  }

  // Was ARTWORK_MAX_BRIGHTNESS (200) here while the transition above fades
  // toward 255 - meaning every transition ended with a visible brightness
  // "pop" down to 200 the instant it completed. That pop is what made
  // clock/idle/artwork transitions look abrupt instead of a smooth fade.
  // drawRecordFast still applies its own ARTWORK_MAX_BRIGHTNESS ceiling
  // internally, so passing 255 here just matches the transition's target.
  renderScene(currentScene, currentArtworkPtr, currentDominantColor, discRotation, 255);
}

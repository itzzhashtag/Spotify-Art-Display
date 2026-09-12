#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

const char* ssid = "yourwifi";
const char* password = "yourpasscode";
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Pico W HTTPS Test");
  Serial.println("------------------");

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected!");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // ----------------------------------------------------------
  // HTTPS TEST
  // ----------------------------------------------------------

  WiFiClientSecure client;

  // TEMPORARY TEST ONLY.
  // This disables certificate verification.
  // We will NOT use this approach for the final Spotify version.
  client.setInsecure();

  HTTPClient https;

  Serial.println();
  Serial.println("Connecting to HTTPS...");

  if (https.begin(client, "https://www.google.com")) {

    int httpCode = https.GET();

    Serial.print("HTTP response code: ");
    Serial.println(httpCode);

    if (httpCode > 0) {
      Serial.println("HTTPS connection SUCCESS!");
    } else {
      Serial.print("HTTPS request failed: ");
      Serial.println(https.errorToString(httpCode));
    }

    https.end();

  } else {
    Serial.println("Unable to start HTTPS connection.");
  }
}

void loop() {
}
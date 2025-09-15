#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
// comment test
// -------- CONFIGURE THESE --------
#define DOOR_PIN 12                        // Feather HUZZAH pin labeled "12/MISO"
const char* WIFI_SSID = "ORBI95";
const char* WIFI_PASS = "260795133tekai";
const char* MQTT_HOST = "192.168.1.29";    // your broker (laptop) LAN IP
const uint16_t MQTT_PORT = 1883;
// If you enabled broker auth later, fill these. For now, leave nullptrs.
const char* MQTT_USER = nullptr;
const char* MQTT_PASS = nullptr;
// ---------------------------------

// Topics (consistent & human-readable)
const char* TOPIC_STATE = "home/door/garage/state";
const char* TOPIC_LWT   = "home/door/garage/availability"; // online/offline

WiFiClient wifi;
PubSubClient mqtt(wifi);

String stateFromLevel(int level) { return level == LOW ? "CLOSED" : "OPEN"; }

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300); Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " failed");
}

bool connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  // LWT: broker will publish "offline" retained if we vanish unexpectedly
  while (!mqtt.connected()) {
    bool ok = mqtt.connect(
      DEVICE_ID,                // client id (unique)
      MQTT_USER, MQTT_PASS,     // creds (nullable if anonymous)
      TOPIC_LWT,                // will topic
      1,                        // will QoS (ignored by PubSubClient publish; still OK)
      true,                     // will retained
      "offline"                 // will payload
    );
    if (ok) {
      // Birth message + retained so new subscribers see it instantly
      mqtt.publish(TOPIC_LWT, "online", true);
      return true;
    }
    delay(1000);
  }
  return false;
}

void publishStateRetained() {
  String s = stateFromLevel(digitalRead(DOOR_PIN));
  mqtt.publish(TOPIC_STATE, s.c_str(), true); // retained
  Serial.printf("Door -> %s\n", s.c_str());
}

void setup() {
  Serial.begin(115200);
  pinMode(DOOR_PIN, INPUT_PULLUP);

  ensureWifi();
  connectMqtt();

  // Publish current state at boot so dashboards are correct immediately
  publishStateRetained();
}

void loop() {
  ensureWifi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  static int last = -1;
  static uint32_t lastMs = 0;

  int now = digitalRead(DOOR_PIN);
  uint32_t ms = millis();

  // Debounce + edge detection (publish only when it changes)
  if (now != last && (ms - lastMs) > 40) {
    last = now;
    lastMs = ms;
    publishStateRetained();
  }

  delay(30);
}

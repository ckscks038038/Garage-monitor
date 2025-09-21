// Basic garage door monitor with on-demand Wi-Fi + MQTT
// - Sends only the latest status ("open"/"closed") as retained message
// - Keeps Wi-Fi on for a 10-minute window after sending; otherwise sleeps radio

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASS, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS

// ---------- User-configurable pins and behavior ----------
// Switch wiring: use internal pull-up, switch to GND.
// When pin reads LOW => switch active. Adjust ACTIVE_LOW if needed.
static const uint8_t SWITCH_PIN = 12;      // GPIO12
static const bool    ACTIVE_LOW = true;    // LOW = active
static const unsigned long DEBOUNCE_MS = 80;

// Connection window after a publish
static const unsigned long WINDOW_MS = 10UL * 60UL * 1000UL; // 10 minutes

// Topics
static const char* TOPIC_STATUS = "garage/door";           // retained: "open"/"closed"
static const char* TOPIC_ONLINE = "garage/door/online";    // retained: "true"/"false"

// Publish on boot so the broker has a correct retained state
static const bool PUBLISH_ON_BOOT = true;

// ---------- Globals ----------
WiFiClient wifi;
PubSubClient mqtt(wifi);

// Status tracking
bool lastStable = false;           // stable logical state (true=open)
bool lastRead = false;             // last raw read mapped to logical
unsigned long lastBounceAt = 0;    // last time the raw read changed

// Dirty flag: there is an unsent latest status
bool dirty = false;

// Window: remain connected until deadline, then sleep Wi-Fi if no pending data
unsigned long windowDeadline = 0;

// Helpers to map pin to logical "open"/"closed"
inline bool readLogical() {
    int v = digitalRead(SWITCH_PIN);
    bool active = (v == LOW);
    bool logicalOpen = ACTIVE_LOW ? !active : active; // default: active means "closed"
    return logicalOpen;
}

inline const char* statusString(bool logicalOpen) {
    return logicalOpen ? "open" : "closed";
}

// ---------- Wi-Fi radio control ----------
void wifiRadioSleep() {
    mqtt.disconnect();
    WiFi.disconnect(true);
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    delay(1);
}

bool wifiEnsureConnected(unsigned long timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.forceSleepWake();
    delay(1);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
        delay(50);
        yield();
    }
    return WiFi.status() == WL_CONNECTED;
}

// ---------- MQTT ----------
String makeClientId() {
#ifdef DEVICE_ID
    return String("esp-") + String(DEVICE_ID);
#else
    return String("esp-") + String(ESP.getChipId(), HEX);
#endif
}

bool mqttConnect() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    String cid = makeClientId();

    // LWT: broker publishes "false" (retained) to online topic if we drop unexpectedly
    const char* willTopic = TOPIC_ONLINE;
    const char* willMsg = "false";
    bool willRetain = true;
    uint8_t willQos = 0;

    bool ok;
    if (strlen(MQTT_USER) > 0 || strlen(MQTT_PASS) > 0) {
        ok = mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, willTopic, willQos, willRetain, willMsg);
    } else {
        ok = mqtt.connect(cid.c_str(), willTopic, willQos, willRetain, willMsg);
    }

    if (ok) {
        // We are online
        mqtt.publish(TOPIC_ONLINE, "true", true);
    }
    return ok;
}

bool publishStatus(bool logicalOpen) {
    const char* payload = statusString(logicalOpen);
    bool ok = mqtt.publish(TOPIC_STATUS, payload, true);
    if (ok) {
        windowDeadline = millis() + WINDOW_MS; // extend window
        dirty = false;
    }
    return ok;
}

void ensureMqttAndPublishIfDirty() {
    if (!dirty) return;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi: connecting...");
    }
    if (!wifiEnsureConnected(8000)) {
        Serial.println("WiFi: connect failed");
        return; // short Wi-Fi attempt
    }
    if (!mqtt.connected()) {
        Serial.println("MQTT: connecting...");
        if (!mqttConnect()) {
            Serial.println("MQTT: connect failed");
            return;
        }
    }
    if (publishStatus(lastStable)) {
        Serial.print("MQTT: published status = ");
        Serial.println(statusString(lastStable));
    } else {
        Serial.println("MQTT: publish failed");
    }
}

void setup() {
    Serial.begin(115200);
    delay(10);
    Serial.println();
    Serial.println("Garage monitor starting...");

    pinMode(SWITCH_PIN, INPUT_PULLUP);

    // Initialize state from current reading
    lastRead = lastStable = readLogical();
    lastBounceAt = millis();

    // Start with Wi-Fi radio OFF to save power
    wifiRadioSleep();

    if (PUBLISH_ON_BOOT) {
        dirty = true;               // publish initial status once
        ensureMqttAndPublishIfDirty();
    }
}

void loop() {
    // 1) Read switch with debounce
    bool nowRead = readLogical();
    if (nowRead != lastRead) {
        lastRead = nowRead;
        lastBounceAt = millis();
    }
    if ((millis() - lastBounceAt) >= DEBOUNCE_MS && lastStable != lastRead) {
        lastStable = lastRead;
        dirty = true; // new status to send
        Serial.print("door: ");
        Serial.println(statusString(lastStable));
    }

    // 2) If we have unsent status, connect on-demand and send
    ensureMqttAndPublishIfDirty();

    // 3) Maintain connection during the window
    if (mqtt.connected()) {
        mqtt.loop();
        if (!dirty && (long)(millis() - windowDeadline) >= 0) {
            Serial.println("Window expired. Sleeping Wi-Fi.");
            wifiRadioSleep();
        }
    } else {
        // If Wi-Fi is still connected but MQTT dropped during window, try reconnect quickly
        if ((WiFi.status() == WL_CONNECTED) && (windowDeadline != 0) && (millis() <= windowDeadline)) {
            Serial.println("MQTT: reconnecting during window...");
            mqttConnect();
        }
    }

    delay(10);
}

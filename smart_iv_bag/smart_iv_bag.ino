/*
  Smart IV Bag Monitor
  Board : TTGO T-Beam v1.2 (ESP32)
  Sensor: HX711 + load cell
  Output: MQTT (patient + control topics), InfluxDB, status LEDs
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HX711.h>
#include <ArduinoJson.h>
#include <InfluxDbClient.h>
#include <time.h>
#include "secrets.h"

// ---------- Pins ----------
#define HX711_DOUT_PIN 13
#define HX711_SCK_PIN 14
#define LED_NOTIFY 25
#define LED_WEIGHT 32
#define LED_NETWORK 33
#define LED_COMM 15
#define LED_ALERT_1 21
#define LED_ALERT_2 22
#define BUTTON_PIN 2

// ---------- Tunables ----------
const float CALIBRATION_FACTOR = -7050.0;   // adjust for your load cell (see README)
const unsigned long SAMPLE_INTERVAL_MS = 5000;
const unsigned long PUBLISH_INTERVAL_MS = 10000;
const int WMA_WINDOW = 6;                   // number of rate samples in the WMA
const float G_PER_ML = 1.0;                 // IV saline ~1 g/mL

// ---------- State ----------
HX711 scale;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
InfluxDBClient influx(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
Point sensorPoint("monitor variables");

String pubTopic = String("public/ivbag/") + PATIENT_ID;
String ctlTopic = String("private/ctl/ivbag/") + PATIENT_ID;

float fullWeight = 0;        // grams, set by push button
bool fullWeightSet = false;
float threshold = 20;        // percent
bool alarmActive = false;

float rateHistory[WMA_WINDOW];
int rateCount = 0;
float lastWeight = 0;
unsigned long lastSampleMs = 0;
unsigned long lastPublishMs = 0;
unsigned long lastBlinkMs = 0;
bool blinkState = false;
bool commError = false;

float level = 100;
float flowRate = 0;          // mL/hr
float currentWeight = 0;

// ---------- Helpers ----------
float wma(const float *vals, int n) {
  float num = 0, den = 0;
  for (int i = 0; i < n; i++) {
    num += vals[i] * (i + 1);   // newest sample has the largest weight
    den += (i + 1);
  }
  return den > 0 ? num / den : 0;
}

void pushRate(float r) {
  if (rateCount < WMA_WINDOW) {
    rateHistory[rateCount++] = r;
  } else {
    for (int i = 1; i < WMA_WINDOW; i++) rateHistory[i - 1] = rateHistory[i];
    rateHistory[WMA_WINDOW - 1] = r;
  }
}

void resetSystem() {
  fullWeightSet = false;
  alarmActive = false;
  rateCount = 0;
  flowRate = 0;
  level = 100;
  lastWeight = 0;
  digitalWrite(LED_ALERT_1, LOW);
  digitalWrite(LED_ALERT_2, LOW);
}

// ---------- MQTT ----------
void onMessage(char *topic, byte *payload, unsigned int length) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, length)) return;

  if (doc.containsKey("threshold")) threshold = doc["threshold"].as<float>();
  if (doc["reset"] | false) resetSystem();
  digitalWrite(LED_NOTIFY, HIGH);
  delay(100);
  digitalWrite(LED_NOTIFY, LOW);
}

bool connectMqtt() {
  if (mqtt.connected()) return true;
  String clientId = String("ivbag-") + PATIENT_ID;
  bool ok = (strlen(MQTT_USER) > 0)
              ? mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)
              : mqtt.connect(clientId.c_str());
  if (ok) mqtt.subscribe(ctlTopic.c_str());
  return ok;
}

void publishStatus() {
  StaticJsonDocument<192> doc;
  doc["level"] = round(level);
  doc["rate"] = round(flowRate * 100) / 100.0;

  float remainingMl = (currentWeight / G_PER_ML);
  int totalMin = (flowRate > 0.1) ? (int)(remainingMl / flowRate * 60.0) : 0;
  JsonObject t = doc.createNestedObject("timeLeft");
  t["hour"] = totalMin / 60;
  t["minute"] = totalMin % 60;

  char buf[192];
  size_t n = serializeJson(doc, buf);
  commError = !mqtt.publish(pubTopic.c_str(), (const uint8_t *)buf, n, false);
}

void writeInflux() {
  sensorPoint.clearFields();
  sensorPoint.addField("weight", currentWeight);
  sensorPoint.addField("level", level);
  sensorPoint.addField("rate", flowRate);
  if (!influx.writePoint(sensorPoint)) commError = true;
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  int outs[] = {LED_NOTIFY, LED_WEIGHT, LED_NETWORK, LED_COMM, LED_ALERT_1, LED_ALERT_2};
  for (int p : outs) pinMode(p, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {      // Network LED blinks until connected
    digitalWrite(LED_NETWORK, !digitalRead(LED_NETWORK));
    delay(300);
  }
  digitalWrite(LED_NETWORK, HIGH);

  timeSync("IST-5:30", "pool.ntp.org", "time.nist.gov");
  influx.setInsecure();   // remove if you use a trusted TLS cert
  sensorPoint.addTag("patient", PATIENT_ID);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  if (!connectMqtt()) commError = true;
  mqtt.loop();

  // Button: set the current weight as the full-bag weight
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      fullWeight = scale.get_units(10);
      if (fullWeight > 1) {
        fullWeightSet = true;
        lastWeight = fullWeight;
        rateCount = 0;
        alarmActive = false;
      }
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }

  // Sample weight, compute level and WMA flow rate
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    float dtSec = (now - lastSampleMs) / 1000.0;
    lastSampleMs = now;
    currentWeight = scale.get_units(5);

    if (fullWeightSet) {
      level = constrain(currentWeight / fullWeight * 100.0, 0, 100);
      float usedMl = (lastWeight - currentWeight) / G_PER_ML;
      float rate = max(0.0f, usedMl / dtSec * 3600.0f);
      pushRate(rate);
      flowRate = wma(rateHistory, rateCount);
      lastWeight = currentWeight;
      alarmActive = (level <= threshold);
    }
  }

  // Publish
  if (fullWeightSet && now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    commError = false;
    if (mqtt.connected()) publishStatus(); else commError = true;
    writeInflux();
  }

  // LEDs
  if (now - lastBlinkMs >= 400) {
    lastBlinkMs = now;
    blinkState = !blinkState;
    digitalWrite(LED_WEIGHT, fullWeightSet ? HIGH : blinkState);
    digitalWrite(LED_COMM, commError ? blinkState : HIGH);
    digitalWrite(LED_ALERT_1, alarmActive ? blinkState : LOW);
    digitalWrite(LED_ALERT_2, alarmActive ? !blinkState : LOW);
  }
}

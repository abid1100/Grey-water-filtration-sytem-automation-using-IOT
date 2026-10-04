#include <esp_timer.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER = "192.168.1.100";   // Raspberry Pi IP
const uint16_t MQTT_PORT = 1883;
const char* DEVICE_ID = "esp32-hydro-01";
const char* SENSOR_TOPIC = "hydro/";
const char* CMD_TOPIC = "hydro/";

// ======== PINS ========
const int PIN_TDS = 34;           // analog input
const int PIN_PH = 35;            // analog input
const int RELAY_NUTRIENT = 26;    // output to injector relay (active LOW)

// ======== INTERVAL ========
unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 15000; // 15 seconds

WiFiClient espClient;
PubSubClient mqttClient(espClient);

float readAnalogVoltage(int pin) {
  int raw = analogRead(pin);
  return (raw / 4095.0) * 3.3;
}

// Convert analog readings to approximate pH & TDS (requires calibration)
float readPH() {
  float v = readAnalogVoltage(PIN_PH);
  // Adjust with your calibration values
  float phValue = 7.0 + (2.5 - v);
  return phValue;
}

float readTDS() {
  float v = readAnalogVoltage(PIN_TDS);
  float ec = v * 2.0; // placeholder mapping (calibrate!)
  return ec;
}

void handleCommand(String payload) {
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("JSON parse error in command.");
    return;
  }
  const char* cmd = doc["cmd"];
  const char* action = doc["action"];

  if (strcmp(cmd, "injector") == 0) {
    if (strcmp(action, "on") == 0) {
      digitalWrite(RELAY_NUTRIENT, LOW);  // turn ON
    } else {
      digitalWrite(RELAY_NUTRIENT, HIGH); // turn OFF
    }
    Serial.printf("Injector %s\n", action);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("Command received: %s\n", msg.c_str());
  handleCommand(msg);
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
}

void connectMQTT() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  while (!mqttClient.connected()) {
    String clientId = String(DEVICE_ID) + String(random(0xffff), HEX);
    Serial.printf("Connecting to MQTT as %s...\n", clientId.c_str());
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("Connected to MQTT.");
      String sub = String("hydro/") + DEVICE_ID + "/commands";
      mqttClient.subscribe(sub.c_str());
      Serial.printf("Subscribed to %s\n", sub.c_str());
    } else {
      Serial.print(".");
      delay(1000);
    }
  }
}

void publishData() {
  float ph = readPH();
  float tds = readTDS();

  StaticJsonDocument<200> doc;
  doc["device_id"] = DEVICE_ID;
  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["pH"] = ph;
  sensors["tds"] = tds;

  char buffer[200];
  size_t n = serializeJson(doc, buffer);

  String topic = String("hydro/") + DEVICE_ID + "/sensors";
  mqttClient.publish(topic.c_str(), buffer, n);
  Serial.printf("Published: %s\n", buffer);
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_NUTRIENT, OUTPUT);
  digitalWrite(RELAY_NUTRIENT, HIGH); // default OFF

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  if (millis() - lastPublish > PUBLISH_INTERVAL) {
    publishData();
    lastPublish = millis();
  }
}

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
// Keep false while reviewing and testing the software.
// Do not enable physical pumping until wiring and safeguards are verified.
const bool ENABLE_PUMP_OUTPUT = false;

const uint32_t MAX_INJECT_SECONDS = 5;

esp_timer_handle_t injectorTimer = nullptr;
portMUX_TYPE injectorMux = portMUX_INITIALIZER_UNLOCKED;

bool injectorRunning = false;
int64_t injectorDeadlineUs = 0;

// Runs in the ESP timer task.
// Keep this short: no Serial, MQTT, delays or database work here.
void injectorTimeout(void* arg) {
  portENTER_CRITICAL(&injectorMux);

  // The deadline check also prevents an old, delayed callback
  // from switching off a newly started action prematurely.
  if (injectorRunning &&
      esp_timer_get_time() >= injectorDeadlineUs) {
    gpio_set_level(
      static_cast<gpio_num_t>(RELAY_NUTRIENT), 1
    );  // Active-low relay: HIGH means OFF.

    injectorRunning = false;
  }

  portEXIT_CRITICAL(&injectorMux);
}

bool initializeInjectorTimer() {
  esp_timer_create_args_t args = {};
  args.callback = &injectorTimeout;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "injector_off";

  return esp_timer_create(&args, &injectorTimer) == ESP_OK;
}

void stopInjector() {
  portENTER_CRITICAL(&injectorMux);

  gpio_set_level(
    static_cast<gpio_num_t>(RELAY_NUTRIENT), 1
  );
  injectorRunning = false;

  portEXIT_CRITICAL(&injectorMux);

  if (injectorTimer != nullptr) {
    // An already-expired/inactive timer needs no further action.
    esp_timer_stop(injectorTimer);
  }
}

bool startInjector(uint32_t seconds) {
  if (injectorTimer == nullptr ||
      seconds == 0 ||
      seconds > MAX_INJECT_SECONDS) {
    return false;
  }

  const uint64_t durationUs =
      static_cast<uint64_t>(seconds) * 1000000ULL;

  portENTER_CRITICAL(&injectorMux);

  // A repeated ON must not extend an action already in progress.
  if (injectorRunning) {
    portEXIT_CRITICAL(&injectorMux);
    return false;
  }

  injectorDeadlineUs = esp_timer_get_time() + durationUs;
  injectorRunning = true;

  gpio_set_level(
    static_cast<gpio_num_t>(RELAY_NUTRIENT),
    ENABLE_PUMP_OUTPUT ? 0 : 1
  );

  portEXIT_CRITICAL(&injectorMux);

  // If arming the timer fails, immediately return to OFF.
  if (esp_timer_start_once(injectorTimer, durationUs) != ESP_OK) {
    stopInjector();
    return false;
  }

  return true;
}
void handleCommand(String payload) {
  StaticJsonDocument<256> doc;

  if (deserializeJson(doc, payload)) {
    Serial.println("Rejected: invalid JSON.");
    return;
  }

  const char* cmd = doc["cmd"].as<const char*>();
  const char* action = doc["action"].as<const char*>();

  if (cmd == nullptr ||
      action == nullptr ||
      strcmp(cmd, "injector") != 0) {
    Serial.println("Rejected: invalid command.");
    return;
  }

  if (strcmp(action, "off") == 0) {
    stopInjector();
    Serial.println("Injector OFF.");
    return;
  }

  if (strcmp(action, "on") != 0) {
    Serial.println("Rejected: action must be on or off.");
    return;
  }

  if (!doc["duration"].is<uint32_t>()) {
    Serial.println("Rejected: duration must be an integer in seconds.");
    return;
  }

  uint32_t seconds = doc["duration"].as<uint32_t>();

  if (seconds == 0 || seconds > MAX_INJECT_SECONDS) {
    Serial.println("Rejected: duration outside the configured limit.");
    return;
  }

  if (!startInjector(seconds)) {
    Serial.println("Rejected: already active or timer unavailable.");
    return;
  }

  Serial.printf(
    "%s for %lu seconds.\n",
    ENABLE_PUMP_OUTPUT ? "Injector ON" : "Dry-run started; relay stays OFF",
    static_cast<unsigned long>(seconds));
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
  digitalWrite(RELAY_NUTRIENT, HIGH);  // Default OFF.

  if (!initializeInjectorTimer()) {
    Serial.println("Timer initialization failed. Pump disabled.");
    return;
  }

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (injectorTimer == nullptr) {
    stopInjector();
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    stopInjector();
    mqttClient.disconnect();
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    stopInjector();
    connectMQTT();
  }

  mqttClient.loop();

  if (millis() - lastPublish > PUBLISH_INTERVAL) {
    publishData();
    lastPublish = millis();
  }
}


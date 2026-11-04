#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

#include "dashboard.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif

#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

#ifndef GOVEE_NAME_PREFIX
#define GOVEE_NAME_PREFIX "GVH"
#endif

namespace {
constexpr uint32_t kScanDurationSeconds = 5;
constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr uint32_t kWifiReconnectIntervalMs = 30000;
constexpr size_t kMaxDeviceAliases = 8;
constexpr size_t kMaxAliasCharacters = 20;
constexpr char kMqttClientId[] = "govee-to-mqtt";
constexpr char kHostName[] = "govee-gw";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Preferences aliasPreferences;
WebServer webServer(80);
BLEScan* bleScan = nullptr;
uint32_t lastReconnectAttempt = 0;
uint32_t lastWifiReconnectAttempt = 0;
bool otaPasswordConfigured = false;
bool otaStarted = false;
bool webServerStarted = false;
bool aliasStorageReady = false;
bool bleScanRunning = false;
volatile bool bleScanCompleted = false;

bool hasRequiredConfiguration() {
  return strlen(WIFI_SSID) > 0 && strlen(MQTT_HOST) > 0;
}

bool connectToWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (strlen(WIFI_SSID) == 0) {
    Serial.println("Wi-Fi is not configured. Copy include/secrets.h.example to include/secrets.h.");
    return false;
  }

  Serial.printf("Connecting to Wi-Fi network %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  if (!WiFi.setHostname(kHostName)) {
    Serial.println("Unable to set the Wi-Fi hostname.");
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection timed out.");
    return false;
  }

  Serial.printf("Wi-Fi connected, IP address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Dashboard URL: http://%s.local/\n", kHostName);
  return true;
}

bool connectToMqtt() {
  if (mqttClient.connected()) {
    return true;
  }

  if (strlen(MQTT_HOST) == 0) {
    Serial.println("MQTT_HOST is not configured.");
    return false;
  }

  const bool connected = strlen(MQTT_USERNAME) == 0
                             ? mqttClient.connect(kMqttClientId)
                             : mqttClient.connect(kMqttClientId, MQTT_USERNAME, MQTT_PASSWORD);
  if (!connected) {
    Serial.printf("MQTT connection failed, state=%d\n", mqttClient.state());
    return false;
  }

  Serial.println("MQTT connected.");
  return true;
}

bool isSupportedSensor(const std::string& name) {
  return name.find("H5104") != std::string::npos ||
         name.find("H5110") != std::string::npos;
}

struct SensorReadings {
  float temperatureCelsius;
  float humidityPercent;
  uint8_t batteryPercent;
};

struct TrackedSensor {
  String name;
  String address;
  SensorReadings readings;
  uint32_t lastSeenMillis;
};

struct DeviceAlias {
  String address;
  String name;
  uint8_t slot;
};

std::vector<TrackedSensor> trackedSensors;
std::vector<DeviceAlias> deviceAliases;

String aliasStorageKey(uint8_t slot) {
  return String("slot") + slot;
}

int findAliasIndex(const String& address) {
  for (size_t index = 0; index < deviceAliases.size(); ++index) {
    if (deviceAliases[index].address == address) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

void loadDeviceAliases() {
  if (!aliasPreferences.begin("govee", false)) {
    Serial.println("Unable to open NVS storage for sensor aliases.");
    return;
  }

  aliasStorageReady = true;
  for (uint8_t slot = 0; slot < kMaxDeviceAliases; ++slot) {
    const String stored = aliasPreferences.getString(aliasStorageKey(slot).c_str(), "");
    const int separator = stored.indexOf('|');
    if (separator <= 0 || separator == static_cast<int>(stored.length()) - 1) {
      continue;
    }

    DeviceAlias alias;
    alias.address = stored.substring(0, separator);
    alias.name = stored.substring(separator + 1);
    alias.slot = slot;
    deviceAliases.push_back(alias);
  }
}

size_t countAliasCharacters(const String& alias) {
  size_t characters = 0;
  for (size_t index = 0; index < alias.length(); ++index) {
    const uint8_t byte = static_cast<uint8_t>(alias[index]);
    if ((byte & 0xC0) != 0x80) {
      ++characters;
    }
  }
  return characters;
}

void handleAliasUpdate() {
  if (!aliasStorageReady) {
    webServer.send(503, "application/json", "{\"error\":\"alias_storage_unavailable\"}");
    return;
  }

  DynamicJsonDocument request(256);
  const DeserializationError parseError =
      deserializeJson(request, webServer.arg("plain"));
  if (parseError) {
    webServer.send(400, "application/json", "{\"error\":\"invalid_json\"}");
    return;
  }

  const String address = request["address"] | "";
  String aliasName = request["alias"] | "";
  if (address.isEmpty() || !request["alias"].is<const char*>()) {
    webServer.send(400, "application/json", "{\"error\":\"address_and_alias_required\"}");
    return;
  }

  bool knownSensor = false;
  for (const TrackedSensor& sensor : trackedSensors) {
    if (sensor.address == address) {
      knownSensor = true;
      break;
    }
  }
  if (!knownSensor) {
    webServer.send(404, "application/json", "{\"error\":\"sensor_not_found\"}");
    return;
  }

  aliasName.trim();
  if (countAliasCharacters(aliasName) > kMaxAliasCharacters) {
    webServer.send(400, "application/json", "{\"error\":\"alias_too_long\"}");
    return;
  }

  const int existingIndex = findAliasIndex(address);
  if (aliasName.isEmpty()) {
    if (existingIndex >= 0) {
      const uint8_t slot = deviceAliases[existingIndex].slot;
      if (!aliasPreferences.remove(aliasStorageKey(slot).c_str())) {
        Serial.printf("Unable to remove the saved alias for %s.\n", address.c_str());
        webServer.send(500, "application/json", "{\"error\":\"alias_storage_failed\"}");
        return;
      }
      deviceAliases.erase(deviceAliases.begin() + existingIndex);
    }
    webServer.send(200, "application/json", "{\"alias\":\"\"}");
    return;
  }

  int slot = existingIndex >= 0 ? deviceAliases[existingIndex].slot : -1;
  if (slot < 0) {
    for (uint8_t candidate = 0; candidate < kMaxDeviceAliases; ++candidate) {
      bool occupied = false;
      for (const DeviceAlias& savedAlias : deviceAliases) {
        if (savedAlias.slot == candidate) {
          occupied = true;
          break;
        }
      }
      if (!occupied) {
        slot = candidate;
        break;
      }
    }
  }

  if (slot < 0) {
    webServer.send(409, "application/json", "{\"error\":\"alias_limit_reached\"}");
    return;
  }

  const String stored = address + "|" + aliasName;
  if (aliasPreferences.putString(aliasStorageKey(slot).c_str(), stored) == 0) {
    Serial.printf("Unable to save the alias for %s.\n", address.c_str());
    webServer.send(500, "application/json", "{\"error\":\"alias_storage_failed\"}");
    return;
  }

  DeviceAlias savedAlias;
  savedAlias.address = address;
  savedAlias.name = aliasName;
  savedAlias.slot = static_cast<uint8_t>(slot);
  if (existingIndex >= 0) {
    deviceAliases[existingIndex] = savedAlias;
  } else {
    deviceAliases.push_back(savedAlias);
  }

  webServer.send(200, "application/json", "{\"saved\":true}");
}

bool decodeSensorReadings(const std::string& data, SensorReadings& readings) {
  if (data.length() < 8) {
    return false;
  }

  // Manufacturer data starts with the two-byte company identifier.
  const uint32_t encoded =
      (static_cast<uint8_t>(data[4]) << 16) |
      (static_cast<uint8_t>(data[5]) << 8) |
      static_cast<uint8_t>(data[6]);
  const bool isNegative = (encoded & 0x800000) != 0;
  const uint32_t magnitude = encoded & 0x7FFFFF;
  const float temperature = static_cast<float>(magnitude / 1000) / 10.0f;
  const float humidity = static_cast<float>(magnitude % 1000) / 10.0f;
  const uint8_t battery = static_cast<uint8_t>(data[7]);

  readings.temperatureCelsius = isNegative ? -temperature : temperature;
  readings.humidityPercent = humidity;
  readings.batteryPercent = battery & 0x7F;

  return (battery & 0x80) == 0 &&
         readings.temperatureCelsius >= -40.0f &&
         readings.temperatureCelsius <= 85.0f &&
         readings.humidityPercent <= 100.0f &&
         readings.batteryPercent <= 100;
}

void updateTrackedSensor(const String& address, const std::string& name,
                         const SensorReadings& readings) {
  for (TrackedSensor& sensor : trackedSensors) {
    if (sensor.address == address) {
      sensor.name = name.c_str();
      sensor.readings = readings;
      sensor.lastSeenMillis = millis();
      return;
    }
  }

  TrackedSensor sensor;
  sensor.name = name.c_str();
  sensor.address = address;
  sensor.readings = readings;
  sensor.lastSeenMillis = millis();
  trackedSensors.push_back(sensor);
}

void handleSensorData() {
  DynamicJsonDocument payload(256 + trackedSensors.size() * 192);
  JsonArray sensors = payload.createNestedArray("sensors");
  const uint32_t now = millis();

  for (const TrackedSensor& trackedSensor : trackedSensors) {
    JsonObject sensor = sensors.createNestedObject();
    sensor["name"] = trackedSensor.name;
    sensor["address"] = trackedSensor.address;
    const int aliasIndex = findAliasIndex(trackedSensor.address);
    sensor["alias"] = aliasIndex >= 0 ? deviceAliases[aliasIndex].name : "";
    sensor["temperature_c"] = trackedSensor.readings.temperatureCelsius;
    sensor["humidity_percent"] = trackedSensor.readings.humidityPercent;
    sensor["battery_percent"] = trackedSensor.readings.batteryPercent;
    sensor["last_seen_seconds"] =
        (now - trackedSensor.lastSeenMillis) / 1000;
  }

  if (payload.overflowed()) {
    Serial.println("Unable to create sensor dashboard response.");
    webServer.send(500, "application/json", "{\"error\":\"response_too_large\"}");
    return;
  }

  String response;
  serializeJson(payload, response);
  webServer.send(200, "application/json", response);
}

void configureWebServer() {
  webServer.on("/", HTTP_GET, []() {
    webServer.send_P(200, "text/html", DASHBOARD_HTML);
  });
  webServer.on("/api/sensors", HTTP_GET, handleSensorData);
  webServer.on("/api/aliases", HTTP_POST, handleAliasUpdate);
}

void startWebServer() {
  if (WiFi.status() != WL_CONNECTED || webServerStarted) {
    return;
  }

  webServer.begin();
  webServerStarted = true;
  Serial.println("Sensor dashboard web server started on port 80.");
}

void handleOta() {
  if (WiFi.status() != WL_CONNECTED) {
    if (otaStarted) {
      ArduinoOTA.end();
      otaStarted = false;
    }
    return;
  }

  if (!otaPasswordConfigured) {
    return;
  }

  if (!otaStarted) {
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.printf("OTA updates ready at %s.local\n", kHostName);
  }

  ArduinoOTA.handle();
}

bool isGoveeAdvertisement(BLEAdvertisedDevice& device) {
  if (!device.haveName()) {
    return false;
  }

  const std::string name = device.getName();
  return name.rfind(GOVEE_NAME_PREFIX, 0) == 0;
}

void publishAdvertisement(BLEAdvertisedDevice& device) {
  const String address = device.getAddress().toString().c_str();
  const String topic = String("govee/raw/") + address;
  const std::string name = device.getName();
  const std::string manufacturerData = device.getManufacturerData();
  if (!isSupportedSensor(name)) {
    return;
  }

  SensorReadings readings;
  if (!decodeSensorReadings(manufacturerData, readings)) {
    //Serial.printf("Skipping non-data or invalid advertisement from %s\n", address.c_str());
    return;
  }

  updateTrackedSensor(address, name, readings);
  if (!mqttClient.connected()) {
    return;
  }

  StaticJsonDocument<384> payload;
  payload["name"] = name.c_str();
  payload["address"] = address;
  const int aliasIndex = findAliasIndex(address);
  payload["alias"] = aliasIndex >= 0 ? deviceAliases[aliasIndex].name : "";
  payload["rssi"] = device.getRSSI();
  JsonObject sensors = payload.createNestedObject("sensors");
  sensors["temperature_c"] = readings.temperatureCelsius;
  sensors["humidity_percent"] = readings.humidityPercent;
  sensors["battery_percent"] = readings.batteryPercent;

  char json[384];
  const size_t jsonLength = serializeJson(payload, json, sizeof(json));
  if (jsonLength == 0 || jsonLength >= sizeof(json)) {
    Serial.printf("Skipping oversized BLE advertisement from %s\n", address.c_str());
    return;
  }

  if (!mqttClient.publish(topic.c_str(), json, true)) {
    Serial.printf("Failed to publish advertisement from %s\n", address.c_str());
    return;
  }

  Serial.printf("Published %s to %s\n", json, topic.c_str());
}

void onBleScanComplete(BLEScanResults) {
  bleScanCompleted = true;
}

void serviceBleScan() {
  if (bleScanCompleted) {
    bleScanCompleted = false;
    BLEScanResults results = bleScan->getResults();
    for (int index = 0; index < results.getCount(); ++index) {
      BLEAdvertisedDevice device = results.getDevice(index);
      if (isGoveeAdvertisement(device)) {
        publishAdvertisement(device);
      }
    }
    bleScan->clearResults();
    bleScanRunning = false;
  }

  if (WiFi.status() == WL_CONNECTED && !bleScanRunning) {
    if (bleScan->start(kScanDurationSeconds, onBleScanComplete, false)) {
      bleScanRunning = true;
    } else {
      Serial.println("Unable to start BLE scan.");
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("Reset reason: %d\n", static_cast<int>(esp_reset_reason()));

  if (!hasRequiredConfiguration()) {
    Serial.println("Configure Wi-Fi and MQTT in include/secrets.h before use.");
  }

  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("Wi-Fi disconnected, reason=%u\n",
                    static_cast<unsigned>(info.wifi_sta_disconnected.reason));
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("Wi-Fi got IP: %s\n", WiFi.localIP().toString().c_str());
    }
  });

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  if (!mqttClient.setBufferSize(512)) {
    Serial.println("Unable to configure the MQTT packet buffer.");
  }
  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(80);

  loadDeviceAliases();

  otaPasswordConfigured = strlen(OTA_PASSWORD) > 0;
  if (otaPasswordConfigured) {
    ArduinoOTA.setHostname(kHostName);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      Serial.println("OTA update started.");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nOTA update finished.");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("OTA update failed, error=%u\n", static_cast<unsigned>(error));
    });
  } else {
    Serial.println("OTA disabled: set a non-empty OTA_PASSWORD in include/secrets.h.");
  }

  configureWebServer();
  lastWifiReconnectAttempt = millis();
  connectToWifi();
  startWebServer();
}

void loop() {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED &&
      now - lastWifiReconnectAttempt >= kWifiReconnectIntervalMs) {
    lastWifiReconnectAttempt = now;
    connectToWifi();
  }
  startWebServer();
  if (webServerStarted) {
    webServer.handleClient();
  }
  handleOta();
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    if (now - lastReconnectAttempt >= kReconnectIntervalMs) {
      lastReconnectAttempt = now;
      connectToMqtt();
    }
  }

  mqttClient.loop();
  serviceBleScan();
  if (webServerStarted) {
    webServer.handleClient();
  }
  handleOta();
  delay(10);
}

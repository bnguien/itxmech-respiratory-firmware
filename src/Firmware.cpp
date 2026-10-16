#include "Firmware.h"
#include "AudioRecorder.h"
#include <BuildConfig.h>

#if AUDIO_SERIAL_DEBUG
static AudioRecorder audio;
static bool ready = false;
void firmwareSetup() {
  Serial.begin(921600);
  ready = audio.begin();
}
void firmwareLoop() {
  if (!ready) { delay(1000); return; }
  int16_t pcm[kBufferSize];
  int count = audio.read(pcm);
  if (count > 0) Serial.write((uint8_t *)pcm, count * sizeof(int16_t));
}
#else
#include "DeviceFlow.h"
#include "Network.h"
#include "Protocol.h"
#include <atomic>

namespace {
struct Assignment { char message[37]; char visit[37]; };
struct Event { char payload[384]; };
QueueHandle_t commands = nullptr;
QueueHandle_t events = nullptr;
std::atomic<bool> busy{true};
WiFiManager wifi;
MqttConnection mqtt;
bool initialized = false;

void emit(const char *type, const String &visit) {
  Event event{};
  String payload = eventPayload(type, visit);
  strlcpy(event.payload, payload.c_str(), sizeof(event.payload));
  xQueueSend(events, &event, portMAX_DELAY);
}

void command(String &topic, String &payload) {
  if (topic != mqtt.commandTopic() || payload.length() > 1024) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload) || !doc.is<JsonObject>() ||
      !doc["version"].is<int>() || doc["version"].as<int>() != 1 ||
      doc["type"] != "ASSIGN_VISIT" || !doc["messageId"].is<String>() ||
      !doc["visitId"].is<String>() || !doc["timestamp"].is<String>()) {
    Serial.println("[MQTT] invalid command rejected"); return;
  }
  String message = doc["messageId"].as<String>();
  String visit = doc["visitId"].as<String>();
  if (!validUuid(message) || !validUuid(visit) ||
      !validTimestamp(doc["timestamp"].as<String>())) {
    Serial.println("[MQTT] invalid identifiers/timestamp"); return;
  }
  bool expected = false;
  if (uxQueueMessagesWaiting(events) || !busy.compare_exchange_strong(expected, true)) {
    Serial.println("[MQTT] busy; assignment rejected, current visit unchanged"); return;
  }
  message.toLowerCase(); visit.toLowerCase();
  Assignment assignment{};
  strlcpy(assignment.message, message.c_str(), sizeof(assignment.message));
  strlcpy(assignment.visit, visit.c_str(), sizeof(assignment.visit));
  if (xQueueSend(commands, &assignment, 0) != pdTRUE) busy = false;
  else Serial.println("[MQTT] ASSIGN_VISIT received");
}

void worker(void *) {
  DeviceFlow flow(emit);
  if (!flow.begin()) {
    Serial.println("[BOOT] audio/storage unavailable; provision filesystem, no auto-format");
    vTaskDelete(nullptr); return;
  }
  // MQTT and HTTPS wait for a real NTP reply after this boot.
  while (!wifi.ready()) delay(250);
  busy = !flow.resume();
  Assignment assignment;
  for (;;) {
    if (xQueueReceive(commands, &assignment, portMAX_DELAY) == pdTRUE)
      busy = !flow.assign(assignment.message, assignment.visit);
  }
}
}

void firmwareSetup() {
  Serial.begin(921600);
  Serial.println("[BOOT] production");
  commands = xQueueCreate(1, sizeof(Assignment));
  events = xQueueCreate(16, sizeof(Event));
  if (!commands || !events) { Serial.println("[BOOT] queue allocation failed"); return; }
  wifi.begin();
  mqtt.begin(command);
  if (xTaskCreatePinnedToCore(worker, "recording", 16384, nullptr, 1,
                              nullptr, 1) != pdPASS) {
    Serial.println("[BOOT] worker allocation failed"); return;
  }
  initialized = true;
}

void firmwareLoop() {
  if (!initialized) { delay(1000); return; }
  mqtt.tick(wifi.tick());
  Event event;
  if (xQueuePeek(events, &event, 0) == pdTRUE && mqtt.publishEvent(event.payload))
    xQueueReceive(events, &event, 0);
  delay(5);
}
#endif

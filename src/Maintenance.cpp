#include "Firmware.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <BuildConfig.h>
#include <LittleFS.h>
#include <Preferences.h>

#ifndef MAINTENANCE_VISIT_ID
#error "Maintenance must only be built for an explicitly selected visit"
#endif

// One-shot local service: no Wi-Fi, MQTT, recording or filesystem formatting.
void firmwareSetup() {
  Serial.begin(921600);
  delay(2000);
  Serial.println("[SERVICE] checking failed job; no recording/network activity");

  if (!LittleFS.begin(false)) {
    Serial.println("[SERVICE] STOP: LittleFS mount failed; nothing deleted");
    return;
  }
  Preferences prefs;
  if (!prefs.begin("respicare", false)) {
    Serial.println("[SERVICE] STOP: NVS unavailable; nothing deleted");
    return;
  }
  String stored = prefs.getString("job", "");
  JsonDocument job;
  if (stored.isEmpty() || deserializeJson(job, stored) ||
      job["visit"].as<String>() != MAINTENANCE_VISIT_ID ||
      job["phase"].as<String>() != "error") {
    Serial.println("[SERVICE] STOP: visit/phase mismatch or no job; nothing deleted");
    prefs.end();
    return;
  }
  if (!LittleFS.exists("/recording.wav")) {
    Serial.println("[SERVICE] STOP: WAV missing; checkpoint left untouched");
    prefs.end();
    return;
  }
  if (!LittleFS.remove("/recording.wav")) {
    Serial.println("[SERVICE] STOP: WAV deletion failed; checkpoint left untouched");
    prefs.end();
    return;
  }
  if (!prefs.remove("job")) {
    Serial.println("[SERVICE] STOP: NVS removal failed; do not assign a new visit");
    prefs.end();
    return;
  }
  prefs.end();
  Serial.println("[SERVICE] DONE: failed WAV/job removed; accepted.log preserved");
  Serial.println("[SERVICE] flash production firmware before assigning new visit");
}

void firmwareLoop() { delay(1000); }

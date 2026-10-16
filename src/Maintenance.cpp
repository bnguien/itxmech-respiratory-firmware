#include "Firmware.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <BuildConfig.h>
#include <LittleFS.h>
#include <Preferences.h>
#if MAINTENANCE_RECOVER
#include "RecordingStorage.h"
#include "AudioRecorder.h"
#endif

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
  if (!prefs.isKey("job")) {
    Serial.println("[SERVICE] STOP: NVS job missing; nothing changed");
    Serial.printf("[SERVICE] WAV present: %s\n",
                  LittleFS.exists("/recording.wav") ? "yes" : "no");
    prefs.end();
    return;
  }
  String stored = prefs.getString("job", "");
  JsonDocument job;
  if (stored.isEmpty() || deserializeJson(job, stored)) {
    Serial.println("[SERVICE] STOP: unreadable NVS job; nothing changed");
    prefs.end();
    return;
  }
  String visit = job["visit"].as<String>();
  String phase = job["phase"].as<String>();
  Serial.printf("[SERVICE] checkpoint visit=%s phase=%s\n",
                visit.c_str(), phase.c_str());
  if (visit != MAINTENANCE_VISIT_ID || phase != "error") {
    Serial.println("[SERVICE] STOP: visit/phase mismatch; nothing changed");
    prefs.end();
    return;
  }
  if (!LittleFS.exists("/recording.wav")) {
    Serial.println("[SERVICE] STOP: WAV missing; checkpoint left untouched");
    prefs.end();
    return;
  }
#if MAINTENANCE_RECOVER
  RecordingStorage storage;
  if (job["recording"].as<String>().length() != 36 ||
      !storage.validRecording(RECORD_SECONDS * kSampleRate * 2)) {
    Serial.println("[SERVICE] STOP: backend ID/WAV invalid; checkpoint untouched");
    prefs.end();
    return;
  }
  // The failed PUT happened after the backend recording ID was checkpointed.
  // Resume with a fresh signed URL; never capture audio a second time.
  job["phase"] = "uploading";
  String updated;
  serializeJson(job, updated);
  if (prefs.putString("job", updated) != updated.length()) {
    Serial.println("[SERVICE] STOP: checkpoint write failed; WAV retained");
    prefs.end();
    return;
  }
  prefs.end();
  Serial.println("[SERVICE] DONE: failed WAV preserved; upload checkpoint restored");
  Serial.println("[SERVICE] flash production firmware to resume this visit");
#else
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
#endif
}

void firmwareLoop() { delay(1000); }

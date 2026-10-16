#include "RecordingStorage.h"
#include <ArduinoJson.h>

bool RecordingStorage::begin() {
  // Never erase user flash implicitly. Provision the partition separately.
  if (!LittleFS.begin(false) || !prefs.begin("respicare", false)) return false;
  // An orphan may be recoverable user audio; never overwrite it automatically.
  return !LittleFS.exists("/recording.wav") || prefs.isKey("job");
}

bool RecordingStorage::hasRoom(uint32_t bytes) {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  return used <= total && total - used >= bytes + 32768;
}

bool RecordingStorage::validRecording(uint32_t pcmBytes) {
  File file = open();
  if (!file || pcmBytes + 44 > 50UL * 1024 * 1024 ||
      file.size() != pcmBytes + 44) return false;
  uint8_t header[44];
  if (file.read(header, sizeof(header)) != sizeof(header)) return false;
  auto u16 = [&](int offset) {
    return uint16_t(header[offset]) | (uint16_t(header[offset + 1]) << 8);
  };
  auto u32 = [&](int offset) {
    return uint32_t(header[offset]) | (uint32_t(header[offset + 1]) << 8) |
           (uint32_t(header[offset + 2]) << 16) | (uint32_t(header[offset + 3]) << 24);
  };
  return memcmp(header, "RIFF", 4) == 0 && u32(4) == pcmBytes + 36 &&
         memcmp(header + 8, "WAVEfmt ", 8) == 0 && u32(16) == 16 &&
         u16(20) == 1 && u16(22) == 1 && u32(24) == 16000 &&
         u32(28) == 32000 && u16(32) == 2 && u16(34) == 16 &&
         memcmp(header + 36, "data", 4) == 0 && u32(40) == pcmBytes;
}

bool RecordingStorage::seen(const String &messageId, const String &visitId) {
  if (!LittleFS.exists("/accepted.log")) return false;
  File log = LittleFS.open("/accepted.log", "r");
  if (!log) return true;  // Fail closed on unreadable deduplication history.
  while (log.available()) {
    String line = log.readStringUntil('\n');
    if (line.length() != 73 || line[36] != ' ') return true;
    if (line.substring(0, 36) == messageId || line.substring(37) == visitId)
      return true;
  }
  return false;
}

bool RecordingStorage::accept(const RecordingJob &job) {
  File log = LittleFS.open("/accepted.log", "a");
  if (!log) return false;
  String line = job.messageId + " " + job.visitId + "\n";
  bool ok = log.print(line) == line.length();
  log.flush();
  log.close();
  return ok && save(job);
}

bool RecordingStorage::save(const RecordingJob &job) {
  JsonDocument doc;
  doc["message"] = job.messageId;
  doc["visit"] = job.visitId;
  doc["recording"] = job.recordingId;
  doc["phase"] = job.phase;
  String value;
  serializeJson(doc, value);
  return prefs.putString("job", value) == value.length();
}

bool RecordingStorage::load(RecordingJob &job) {
  String value = prefs.getString("job", "");
  if (value.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, value)) {
    job.phase = "error";
    return true;
  }
  job.messageId = doc["message"].as<String>();
  job.visitId = doc["visit"].as<String>();
  job.recordingId = doc["recording"].as<String>();
  job.phase = doc["phase"].as<String>();
  return true;
}

bool RecordingStorage::finish() {
  if (LittleFS.exists("/recording.wav") && !LittleFS.remove("/recording.wav"))
    return false;
  return prefs.remove("job");
}

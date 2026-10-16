#pragma once
#include <LittleFS.h>
#include <Preferences.h>

struct RecordingJob {
  String messageId;
  String visitId;
  String recordingId;
  String phase;
};

// Replace this adapter if the confirmed hardware uses another filesystem.
class RecordingStorage {
 public:
  bool begin();
  bool seen(const String &messageId, const String &visitId);
  bool accept(const RecordingJob &job);
  bool save(const RecordingJob &job);
  bool load(RecordingJob &job);
  bool finish();
  bool hasRoom(uint32_t bytes);
  bool validRecording(uint32_t pcmBytes);
  File create() { return LittleFS.open("/recording.wav", "w+"); }
  File open() { return LittleFS.open("/recording.wav", "r"); }
 private:
  Preferences prefs;
};

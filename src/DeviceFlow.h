#pragma once
#include "AudioRecorder.h"
#include "RecordingApiClient.h"

enum class DeviceState {
  BOOT, IDLE, VISIT_ASSIGNED, RECORDING, RECORDING_COMPLETE,
  REQUESTING_UPLOAD_URL, UPLOADING, COMPLETING, ERROR, RETRY
};
using EventHandler = void (*)(const char *, const String &);

class DeviceFlow {
 public:
  explicit DeviceFlow(EventHandler handler) : emit(handler) {}
  bool begin();
  bool resume();
  bool assign(const String &messageId, const String &visitId);
 private:
  RecordingStorage storage;
  AudioRecorder audio;
  RecordingApiClient api;
  RecordingJob job;
  DeviceState state = DeviceState::BOOT;
  EventHandler emit;
  bool run();
  bool checkpoint(const char *phase);
  bool fail(int code);
  bool upload();
  bool complete();
  void backoff(unsigned attempt);
};

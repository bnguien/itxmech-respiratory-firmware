#include "DeviceFlow.h"
#include <BuildConfig.h>
#include <WiFi.h>

bool DeviceFlow::begin() {
  return storage.begin() && audio.begin();
}

bool DeviceFlow::checkpoint(const char *phase) {
  job.phase = phase;
  return storage.save(job);
}

bool DeviceFlow::fail(int code) {
  state = DeviceState::ERROR;
  Serial.printf("[FLOW] failed code=%d; local recording retained; service required\n", code);
  checkpoint("error");
  emit("UPLOAD_FAILED", job.visitId);
  return false;
}

void DeviceFlow::backoff(unsigned attempt) {
  state = DeviceState::RETRY;
  delay((1000U << attempt) + (esp_random() % 500));
}

bool DeviceFlow::resume() {
  if (!storage.load(job)) { state = DeviceState::IDLE; return true; }
  Serial.println("[FLOW] restoring checkpoint");
  // A power loss mid-capture cannot be silently re-recorded as a new WAV.
  if (job.phase == "recording" || job.phase == "error") return fail(0);
  return run();
}

bool DeviceFlow::assign(const String &messageId, const String &visitId) {
  if (storage.seen(messageId, visitId)) {
    Serial.println("[MQTT] duplicate assignment ignored");
    return true;
  }
  job = {messageId, visitId, "", "assigned"};
  if (!storage.hasRoom(RECORD_SECONDS * kSampleRate * 2 + 44)) return fail(413);
  if (!storage.accept(job)) return fail(0);
  state = DeviceState::VISIT_ASSIGNED;
  Serial.printf("[VISIT] %s\n", visitId.c_str());
  emit("VISIT_ASSIGNED", visitId);
  return run();
}

bool DeviceFlow::upload() {
  int code = -1;
  // Each attempt requests a fresh URL. Backend reuses the same recording ID.
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (attempt) backoff(attempt - 1);
    state = DeviceState::REQUESTING_UPLOAD_URL;
    Serial.println("[API] requesting upload URL");
    UploadTicket ticket;
    code = api.requestUrl(job, ticket);
    if (!api.success(code)) {
      if (api.retryable(code)) continue;
      return fail(code);
    }
    // Persist the backend ID before any bytes can reach Storage.
    if (!checkpoint("uploading")) return fail(0);
    File file = storage.open();
    state = DeviceState::UPLOADING;
    emit("UPLOAD_STARTED", job.visitId);
    Serial.println("[UPLOAD] uploading WAV");
    code = api.upload(ticket, file);
    file.close();
    if (api.success(code)) {
      if (!checkpoint("completing")) return fail(0);
      return true;
    }
    // Signed Storage tokens may report expiry as 400, 401 or 403.
    if (!api.retryable(code) && code != 400 && code != 401 && code != 403)
      return fail(code);
  }
  return fail(code);
}

bool DeviceFlow::complete() {
  int code = -1;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (attempt) backoff(attempt - 1);
    state = DeviceState::COMPLETING;
    code = api.complete(job);
    if (api.success(code)) {
      Serial.println("[API] complete acknowledged");
      return checkpoint("done") || fail(0);
    }
    if (!api.retryable(code)) return fail(code);
  }
  return fail(code);
}

bool DeviceFlow::run() {
  if (job.phase == "assigned") {
    if (!storage.hasRoom(RECORD_SECONDS * kSampleRate * 2 + 44) ||
        !checkpoint("recording")) return fail(0);
    File file = storage.create();
    if (!file) return fail(0);
    state = DeviceState::RECORDING;
    emit("RECORDING_STARTED", job.visitId);
    Serial.println("[AUDIO] recording started");
    bool ok = audio.record(file, RECORD_SECONDS);
    file.close();
    if (!ok || !checkpoint("recorded")) return fail(0);
    state = DeviceState::RECORDING_COMPLETE;
    emit("RECORDING_STOPPED", job.visitId);
    Serial.println("[AUDIO] recording stopped");
  }
  if (job.phase == "recorded" || job.phase == "uploading") {
    // A recovered checkpoint is not proof that the local WAV survived intact.
    if (!storage.validRecording(RECORD_SECONDS * kSampleRate * 2)) return fail(415);
    if (!upload()) return false;
  }
  if (job.phase == "completing" && !complete()) return false;
  if (job.phase != "done") return fail(0);
  emit("UPLOAD_SUCCESS", job.visitId);
  if (!storage.finish()) {
    Serial.println("[FLOW] complete succeeded; cleanup failed; locked until service/reboot");
    return false;  // Keep DONE, not ERROR: never redo an acknowledged upload.
  }
  job = {};
  state = DeviceState::IDLE;
  Serial.println("[FLOW] success");
  return true;
}

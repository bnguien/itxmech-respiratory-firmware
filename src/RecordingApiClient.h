#pragma once
#include "RecordingStorage.h"

struct UploadTicket {
  String url;
  uint32_t issuedAt = 0;
  uint32_t lifetimeMs = 0;
  bool fresh() const {
    return lifetimeMs > 30000 && uint32_t(millis() - issuedAt) < lifetimeMs - 30000;
  }
};

class RecordingApiClient {
 public:
  int requestUrl(RecordingJob &job, UploadTicket &ticket);
  int upload(const UploadTicket &ticket, File &file);
  int complete(const RecordingJob &job);
  static bool success(int code) { return code >= 200 && code < 300; }
  static bool retryable(int code) {
    return code <= 0 || code == 408 || code == 429 || code >= 500;
  }
};

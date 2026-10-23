    WiFi.setSleep(sleep);
    Serial.printf("[UPLOAD] TLS connect failed after %lu ms\n",
                  (unsigned long)(millis() - connecting));
    logTlsError("UPLOAD", tls);
    logLink("UPLOAD");
    return -1;
  }
  Serial.printf("[UPLOAD] TLS connected in %lu ms\n",
                (unsigned long)(millis() - connecting));
  // Keep device API credentials off the Storage host.
  String head = "PUT " + path + " HTTP/1.1\r\nHost: " + UPLOAD_HOST +
                "\r\nContent-Type: audio/wav\r\nContent-Length: " +
                String(total) + "\r\nConnection: close\r\n\r\n";

  // All timeouts here cover one attempt, not all retries.
  constexpr uint32_t kBodyDeadlineMs = 180000;
  constexpr uint32_t kChunkDeadlineMs = 20000;
  constexpr size_t kChunkBytes = 1024;
  bool ok = true;
  size_t headerWritten = writeAll(tls,
      reinterpret_cast<const uint8_t *>(head.c_str()),
      head.length(), millis() + kChunkDeadlineMs, ok);
  if (!ok || headerWritten != head.length()) {
    Serial.printf("[UPLOAD] headers incomplete: %u/%u bytes\n",
                  unsigned(headerWritten), unsigned(head.length()));
    logTlsError("UPLOAD", tls);
    tls.stop();
    WiFi.setSleep(sleep);
    return -1;
  }
  if (!file.seek(0)) {
    Serial.println("[UPLOAD] failed to seek WAV to start");
    tls.stop();
    WiFi.setSleep(sleep);
    return -1;
  }

  static uint8_t buffer[kChunkBytes];
  size_t sent = 0;
  uint32_t started = millis();
  uint32_t lastReport = started;
  size_t reported = 0;
  uint32_t slowest = 0, slowWrites = 0, readMs = 0;
  int earlyCode = -1;

  while (ok && sent < total) {
    if (millis() - started >= kBodyDeadlineMs) {
      Serial.println("[UPLOAD] body deadline exceeded (180 s)");
      ok = false;
      break;
    }

    // If the server rejects the request before receiving the full body,
    // capture the HTTP status rather than continuing a doomed PUT.
    if (tls.available() > 0) {
      Serial.println("[UPLOAD] Storage responded before body completed");
      earlyCode = readResponse(tls, 3000);
      ok = false;
      break;
    }

    uint32_t readStart = millis();
    size_t n = file.read(buffer, min(kChunkBytes, total - sent));
    readMs += millis() - readStart;
    if (n == 0) {
      Serial.printf("[UPLOAD] LittleFS read returned 0 at offset %u\n",
                    unsigned(sent));
      ok = false;
      break;
    }

    uint32_t writeStart = millis();
    // Do not count the whole chunk unless all its bytes were accepted.
    uint32_t elapsed = millis() - started;
    uint32_t remainingBody = elapsed < kBodyDeadlineMs ? kBodyDeadlineMs - elapsed : 0;
    uint32_t budget = min(kChunkDeadlineMs, remainingBody);
    size_t accepted = writeAll(tls, buffer, n, millis() + budget, ok);
    uint32_t writeMs = millis() - writeStart;
    sent += accepted;

    if (writeMs > slowest) slowest = writeMs;
    if (writeMs > 1000) {
      ++slowWrites;
      Serial.printf("[UPLOAD] slow write: %u/%u bytes took %lu ms at offset %u\n",
                    unsigned(accepted), unsigned(n),
                    (unsigned long)writeMs, unsigned(sent - accepted));
    }
    if (!ok || accepted != n) {
      Serial.printf("[UPLOAD] write incomplete at %u: %u/%u in %lu ms\n",
                    unsigned(sent - accepted), unsigned(accepted), unsigned(n),
                    (unsigned long)writeMs);
      logTlsError("UPLOAD", tls);
      logLink("UPLOAD");
      ok = false;
      break;
    }

    if (sent - reported >= 65536) {
      uint32_t now = millis();
      uint32_t span = now - lastReport;
      if (span == 0) span = 1;
      Serial.printf("[UPLOAD] progress %u/%u bytes, %lu ms, %.1f KB/s, RSSI %d\n",
                    unsigned(sent), unsigned(total),
                    (unsigned long)(now - started),
                    (sent - reported) * 1000.0f / (1024.0f * span),
                    WiFi.RSSI());
      reported = sent;
      lastReport = now;
    }
  }

  Serial.printf("[UPLOAD] accepted %u/%u bytes in %lu ms; slowest write %lu ms, "
                "%lu writes >1 s, file read %lu ms\n", unsigned(sent),
                unsigned(total), (unsigned long)(millis() - started),
                (unsigned long)slowest, (unsigned long)slowWrites,
                (unsigned long)readMs);

  int code = earlyCode;
  if (code < 0) {
    uint32_t waiting = millis();
    code = readResponse(tls, ok && sent == total ? 30000 : 3000);
    Serial.printf("[UPLOAD] response wait %lu ms, result %d\n",
                  (unsigned long)(millis() - waiting), code);
  }
  tls.stop();
  WiFi.setSleep(sleep);

  // A positive HTTP error is useful to caller; never report success if
  // the complete WAV was not transmitted.
  if (code >= 200 && code < 300 && (!ok || sent != total)) {
    Serial.println("[UPLOAD] rejecting premature success; WAV body incomplete");
    return -1;
  }
  return code > 0 ? code : -1;
}

int RecordingApiClient::complete(const RecordingJob &job) {
  WiFiClientSecure tls;
  HTTPClient http;
  configure(http, tls, true);
  String endpoint = String(WEB_BASE_URL) + "/api/device/recordings/" +
                    job.recordingId + "/complete";
  logLink("COMPLETE");
  if (!http.begin(tls, endpoint)) {
    Serial.println("[COMPLETE] http.begin failed");
    return -1;
  }
  http.addHeader("X-Device-Key", FIRMWARE_DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  uint32_t started = millis();
  int code = http.POST("{}");
  logHttpResult("COMPLETE", http, code, millis() - started);
  if (code < 0) logTlsError("COMPLETE", tls);
  if (code > 0 && !success(code)) {
    String body = http.getString();
    if (body.length() > 512) body = body.substring(0, 512);
    Serial.printf("[COMPLETE] error body: %s\n", body.c_str());
  }
  http.end();
  return code;
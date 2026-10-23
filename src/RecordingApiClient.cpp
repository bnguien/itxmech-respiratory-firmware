#include "RecordingApiClient.h"
#include "Protocol.h"
#include <BuildConfig.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "WebApiCa.h"
#include "StorageCa.h"

namespace {
// HTTPClient decodes chunking into this bounded sink; never buffer a WAV in RAM.
class ResponseBuffer : public Stream {
 public:
  String value;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (value.length() + size > 8192) return 0;
    return value.concat(reinterpret_cast<const char *>(buffer), size) ? size : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

void configure(HTTPClient &http, WiFiClientSecure &tls, bool webApi) {
  // The Vercel chain contains a cross-signed GTS root; the Arduino bundle
  // callback tries to anchor the final cross-sign instead of trusted GTS R1.
  // Use Mozilla-trusted self-signed roots for the two allowlisted hosts.
  if (webApi) tls.setCACert(kWebApiCa);
  else tls.setCACert(kStorageCa);  // Signed Supabase Storage host (GTS R4).
  tls.setHandshakeTimeout(15);
  http.setConnectTimeout(15000);
  // Backend complete can wait for AI; a timeout is retried idempotently.
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
}

bool allowedUploadUrl(const String &url) {
  return url.startsWith(String("https://") + UPLOAD_HOST + "/") &&
         url.indexOf('#') < 0 && url.length() <= 4096;
}

// Debug helpers. Never print the signed URL query (token) or the device key.
void logLink(const char *tag) {
  Serial.printf("[%s] wifi status=%d RSSI=%d dBm ch=%d heap=%u min=%u\n", tag,
                int(WiFi.status()), WiFi.RSSI(), WiFi.channel(),
                unsigned(ESP.getFreeHeap()), unsigned(ESP.getMinFreeHeap()));
}

void logTlsError(const char *tag, WiFiClientSecure &tls) {
  char text[96] = {0};
  int err = tls.lastError(text, sizeof(text));
  Serial.printf("[%s] TLS last error %d: %s\n", tag, err, text);
}

void logHttpResult(const char *tag, HTTPClient &http, int code, uint32_t ms) {
  if (code < 0) {
    Serial.printf("[%s] HTTP error %d (%s) after %lu ms\n", tag, code,
                  HTTPClient::errorToString(code).c_str(), (unsigned long)ms);
  } else {
    Serial.printf("[%s] HTTP %d after %lu ms\n", tag, code, (unsigned long)ms);
  }
}

String redactedPath(const String &url) {
  int query = url.indexOf('?');
  String path = url.substring(strlen("https://") + strlen(UPLOAD_HOST),
                              query < 0 ? url.length() : query);
  return path + (query < 0 ? "" : "?<redacted>");
}
}

int RecordingApiClient::requestUrl(RecordingJob &job, UploadTicket &ticket) {
  WiFiClientSecure tls;
  HTTPClient http;
  configure(http, tls, true);
  String endpoint = String(WEB_BASE_URL) + "/api/device/visits/" +
                    job.visitId + "/recording/upload-url";
  logLink("API");
  if (!http.begin(tls, endpoint)) {
    Serial.println("[API] http.begin failed for upload-url endpoint");
    return -1;
  }
  http.addHeader("X-Device-Key", FIRMWARE_DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  uint32_t started = millis();
  int code = http.POST("{}");
  logHttpResult("API", http, code, millis() - started);
  if (code < 0) logTlsError("API", tls);
  if (success(code)) {
    ResponseBuffer body;
    JsonDocument doc;
    if (http.writeToStream(&body) < 0) {
      Serial.println("[API] failed to read upload-url response body");
      http.end(); return -2;
    }
    if (DeserializationError err = deserializeJson(doc, body.value)) {
      Serial.printf("[API] upload-url JSON invalid: %s (%u bytes)\n",
                    err.c_str(), unsigned(body.value.length()));
      http.end(); return -2;
    }
    JsonVariant data = doc["data"];
    String id = data["recording_id"].as<String>();
    String visit = data["visit_id"].as<String>();
    String url = data["upload_url"].as<String>();
    uint32_t expiry = data["expires_in"] | 0U;
    if (!validUuid(id) || visit != job.visitId || !allowedUploadUrl(url) ||
        expiry <= 30 || expiry > 86400 ||
        (!job.recordingId.isEmpty() && job.recordingId != id)) {
      Serial.printf("[API] contract violation: id_ok=%d visit_ok=%d url_ok=%d "
                    "expiry=%u recording_match=%d\n",
                    validUuid(id), visit == job.visitId, allowedUploadUrl(url),
                    unsigned(expiry),
                    job.recordingId.isEmpty() || job.recordingId == id);
      http.end(); return 415;  // Contract violation: do not upload elsewhere.
    }
    Serial.printf("[API] recording=%s expires_in=%u s path=%s\n", id.c_str(),
                  unsigned(expiry), redactedPath(url).c_str());
    job.recordingId = id;
    ticket.url = url;
    ticket.issuedAt = millis();
    ticket.lifetimeMs = expiry * 1000;
  } else if (code > 0) {
    // Backend error JSON; it does not echo the device key.
    String body = http.getString();
    if (body.length() > 512) body = body.substring(0, 512);
    Serial.printf("[API] upload-url error body: %s\n", body.c_str());
  }
  http.end();
  return code;
}

namespace {
// Storage may answer (e.g. 400/413/415) and reset before the body is fully
// sent; read whatever response arrived so the real reason reaches the log.
int readResponse(WiFiClientSecure &tls, uint32_t timeoutMs) {
  uint32_t start = millis();
  String status;
  while (millis() - start < timeoutMs && (tls.connected() || tls.available())) {
    if (!tls.available()) { delay(10); continue; }
    status = tls.readStringUntil('\n');
    break;
  }
  status.trim();
  if (!status.startsWith("HTTP/1.")) {
    Serial.println("[UPLOAD] no HTTP response from Storage");
    return -1;
  }
  int code = status.substring(9, 12).toInt();
  while (millis() - start < timeoutMs && (tls.connected() || tls.available())) {
    if (!tls.available()) { delay(10); continue; }
    String line = tls.readStringUntil('\n');
    if (line == "\r" || line.isEmpty()) break;
  }
  if (code < 200 || code >= 300) {
    // Storage error JSON only; it carries no token or device credential.
    String body;
    while (millis() - start < timeoutMs && body.length() < 512 &&
           (tls.connected() || tls.available())) {
      if (!tls.available()) { delay(10); continue; }
      body += char(tls.read());
    }
    Serial.printf("[UPLOAD] Storage HTTP %d: %s\n", code, body.c_str());
  }
  return code;
}

bool writeAll(WiFiClientSecure &tls, const uint8_t *data, size_t len) {
  while (len) {
    size_t sent = tls.write(data, len);
    if (sent == 0 || sent > len) return false;
    data += sent; len -= sent;
  }
  return true;
}
}

int RecordingApiClient::upload(const UploadTicket &ticket, File &file) {
  if (!ticket.fresh()) {
    Serial.println("[UPLOAD] signed URL too close to expiry; skipping");
    return 403;
  }
  if (!file || file.size() < 44 || file.size() > 50UL * 1024 * 1024) {
    Serial.printf("[UPLOAD] local WAV invalid: open=%d size=%u\n", bool(file),
                  file ? unsigned(file.size()) : 0U);
    return 413;
  }
  if (!allowedUploadUrl(ticket.url)) {
    Serial.println("[UPLOAD] upload URL not on allowlisted host");
    return -1;
  }
  String path = ticket.url.substring(strlen("https://") + strlen(UPLOAD_HOST));
  WiFiClientSecure tls;
  tls.setCACert(kStorageCa);  // Signed Supabase Storage host (GTS R4).
  tls.setHandshakeTimeout(15);
  tls.setTimeout(30);
  // Modem sleep throttles TX badly on phone hotspots; restore it afterwards.
  bool sleep = WiFi.getSleep();
  WiFi.setSleep(false);
  size_t total = file.size();
  Serial.printf("[UPLOAD] RSSI %d dBm, free heap %u; connecting Storage TLS\n",
                WiFi.RSSI(), unsigned(ESP.getFreeHeap()));
  uint32_t connecting = millis();
  logLink("UPLOAD");
  Serial.printf("[UPLOAD] PUT %s (%u bytes), URL age %lu ms\n",
                redactedPath(ticket.url).c_str(), unsigned(total),
                (unsigned long)(millis() - ticket.issuedAt));
  if (!tls.connect(UPLOAD_HOST, 443)) {
    WiFi.setSleep(sleep);
    Serial.printf("[UPLOAD] TLS connect failed after %lu ms\n",
                  (unsigned long)(millis() - connecting));
    logTlsError("UPLOAD", tls);
    logLink("UPLOAD");
    return -1;
  }
  Serial.printf("[UPLOAD] TLS connected in %lu ms\n",
                (unsigned long)(millis() - connecting));
  // Deliberately do not attach the device API key to the Storage host.
  String head = "PUT " + path + " HTTP/1.1\r\nHost: " + UPLOAD_HOST +
                "\r\nContent-Type: audio/wav\r\nContent-Length: " +
                String(total) + "\r\nConnection: close\r\n\r\n";
  bool ok = writeAll(tls, (const uint8_t *)head.c_str(), head.length());
  file.seek(0);
  static uint8_t buffer[4096];  // One mbedTLS record; keeps the stack small.
  size_t sent = 0;
  uint32_t started = millis();
  if (!ok) Serial.println("[UPLOAD] failed to send request headers");
  size_t reported = 0;
  uint32_t lastReport = started;
  uint32_t slowest = 0, slowWrites = 0, readMs = 0;
  while (ok && sent < total) {
    // A stalled link must fail into the retry path instead of hanging.
    if (millis() - started > 180000) {
      Serial.println("[UPLOAD] body timeout after 180 s");
      ok = false; break;
    }
    uint32_t readStart = millis();
    size_t n = file.read(buffer, min(sizeof(buffer), total - sent));
    readMs += millis() - readStart;
    if (n == 0) {
      Serial.printf("[UPLOAD] LittleFS read returned 0 at offset %u\n",
                    unsigned(sent));
      ok = false; break;
    }
    uint32_t writeStart = millis();
    ok = writeAll(tls, buffer, n);
    uint32_t writeMs = millis() - writeStart;
    if (writeMs > slowest) slowest = writeMs;
    if (writeMs > 1000) {
      ++slowWrites;
      Serial.printf("[UPLOAD] slow write: %u bytes took %lu ms at offset %u\n",
                    unsigned(n), (unsigned long)writeMs, unsigned(sent));
    }
    if (!ok) {
      Serial.printf("[UPLOAD] write failed at offset %u after %lu ms\n",
                    unsigned(sent), (unsigned long)writeMs);
      logTlsError("UPLOAD", tls);
      logLink("UPLOAD");
    }
    if (ok) sent += n;
    if (sent - reported >= 65536) {
      uint32_t now = millis();
      uint32_t span = now - lastReport ? now - lastReport : 1;
      Serial.printf("[UPLOAD] progress %u/%u bytes, %lu ms, %.1f KB/s, RSSI %d\n",
                    unsigned(sent), unsigned(total),
                    (unsigned long)(now - started),
                    (sent - reported) / 1.024 / span, WiFi.RSSI());
      reported = sent;
      lastReport = now;
    }
  }
  Serial.printf("[UPLOAD] sent %u/%u bytes in %lu ms; slowest write %lu ms, "
                "%lu writes >1 s, file read %lu ms\n", unsigned(sent),
                unsigned(total), (unsigned long)(millis() - started),
                (unsigned long)slowest, (unsigned long)slowWrites,
                (unsigned long)readMs);
  uint32_t waiting = millis();
  int code = readResponse(tls, ok ? 30000 : 3000);
  Serial.printf("[UPLOAD] response wait %lu ms, result %d\n",
                (unsigned long)(millis() - waiting), code);
  tls.stop();
  WiFi.setSleep(sleep);
  return ok || code > 0 ? code : -1;
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
}

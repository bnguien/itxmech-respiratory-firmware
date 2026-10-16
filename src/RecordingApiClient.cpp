#include "RecordingApiClient.h"
#include "Protocol.h"
#include <BuildConfig.h>
#include <HTTPClient.h>
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
}

int RecordingApiClient::requestUrl(RecordingJob &job, UploadTicket &ticket) {
  WiFiClientSecure tls;
  HTTPClient http;
  configure(http, tls, true);
  String endpoint = String(WEB_BASE_URL) + "/api/device/visits/" +
                    job.visitId + "/recording/upload-url";
  if (!http.begin(tls, endpoint)) return -1;
  http.addHeader("X-Device-Key", FIRMWARE_DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{}");
  if (success(code)) {
    ResponseBuffer body;
    JsonDocument doc;
    if (http.writeToStream(&body) < 0 || deserializeJson(doc, body.value)) {
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
      http.end(); return 415;  // Contract violation: do not upload elsewhere.
    }
    job.recordingId = id;
    ticket.url = url;
    ticket.issuedAt = millis();
    ticket.lifetimeMs = expiry * 1000;
  }
  http.end();
  return code;
}

int RecordingApiClient::upload(const UploadTicket &ticket, File &file) {
  if (!ticket.fresh()) return 403;
  if (!file || file.size() < 44 || file.size() > 50UL * 1024 * 1024) return 413;
  WiFiClientSecure tls;
  HTTPClient http;
  configure(http, tls, false);
  if (!allowedUploadUrl(ticket.url) || !http.begin(tls, ticket.url)) return -1;
  http.addHeader("Content-Type", "audio/wav");
  // Deliberately do not attach the device API key to the Storage host.
  file.seek(0);
  int code = http.sendRequest("PUT", &file, file.size());
  http.end();
  return code;
}

int RecordingApiClient::complete(const RecordingJob &job) {
  WiFiClientSecure tls;
  HTTPClient http;
  configure(http, tls, true);
  String endpoint = String(WEB_BASE_URL) + "/api/device/recordings/" +
                    job.recordingId + "/complete";
  if (!http.begin(tls, endpoint)) return -1;
  http.addHeader("X-Device-Key", FIRMWARE_DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{}");
  http.end();
  return code;
}

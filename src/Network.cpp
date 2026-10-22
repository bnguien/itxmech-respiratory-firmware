#include "Network.h"
#include "Protocol.h"
#include <BuildConfig.h>
#include <WiFi.h>
#include <time.h>
#include <atomic>
#include <esp_sntp.h>
#include <ctype.h>

namespace {
std::atomic<bool> ntpSynced{false};
void onNtpSync(struct timeval *) { ntpSynced.store(true); }
}

void WiFiManager::begin() {
  settings.begin("wifi_setup", false);
  ssid = settings.getString("ssid", WIFI_SSID);
  password = settings.getString("password", WIFI_PASSWORD);
  JsonDocument credentials;
  String saved = settings.getString("credentials", "");
  if (saved.length() && !deserializeJson(credentials, saved) &&
      credentials["ssid"].is<String>() && credentials["password"].is<String>()) {
    ssid = credentials["ssid"].as<String>();
    password = credentials["password"].as<String>();
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (ssid.length()) WiFi.begin(ssid.c_str(), password.c_str());
  lastAttempt = millis();
  disconnectedSince = lastAttempt;
  ntpSynced.store(false);
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  sntp_set_time_sync_notification_cb(onNtpSync);
  Serial.println("[WIFI] connecting; waiting for UTC time before TLS");
  if (!ssid.length()) startPortal();
}

void WiFiManager::showPortal() {
  if (portal.client().localIP() != WiFi.softAPIP()) {
    portal.send(403, "text/plain", "Connect to device setup Wi-Fi first"); return;
  }
  // No credentials are interpolated into HTML or echoed back to the browser.
  String html = F("<!doctype html><html lang='vi'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>RespiCare Wi-Fi</title><body style='font:18px sans-serif;max-width:480px;margin:40px auto;padding:20px'>"
    "<h1>RespiCare Wi-Fi</h1><p>Kết nối Wi-Fi 2.4 GHz. Trang này không cần Internet.</p><p>");
  html += portalMessage;
  html += F("</p><form method='post' action='/configure'>"
    "<label>Tên Wi-Fi (SSID)<br><input name='ssid' maxlength='32' required></label><br><br>"
    "<label>Mật khẩu<br><input name='password' type='password' maxlength='64'></label><br><br>"
    "<button>Kết nối</button></form><p>Sau khi gửi, chờ 30 giây rồi tải lại trang. "
    "Wi-Fi setup sẽ tắt khi kết nối thành công.</p></body></html>");
  portal.sendHeader("Cache-Control", "no-store");
  portal.send(200, "text/html; charset=utf-8", html);
}

void WiFiManager::startPortal() {
  if (portalActive) return;
  String apPassword = settings.getString("ap_password", "");
  if (apPassword.length() < 12) {
    char generated[17];
    snprintf(generated, sizeof(generated), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    apPassword = generated;
    settings.putString("ap_password", apPassword);
  }
  String apName = String("RespiCare-Setup-") + String(DEVICE_ID).substring(0, 16);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if (!WiFi.softAP(apName.c_str(), apPassword.c_str())) {
    Serial.println("[SETUP] AP start failed");
    return;
  }
  portalMessage = "Nhập thông tin Wi-Fi mới.";
  portal.on("/", HTTP_GET, [this]() { showPortal(); });
  portal.on("/configure", HTTP_POST, [this]() {
    if (portal.client().localIP() != WiFi.softAPIP()) {
      portal.send(403, "text/plain", "Connect to device setup Wi-Fi first"); return;
    }
    if (testing || connectPending || closePortalAt) {
      portal.send(409, "text/plain", "Connection already in progress"); return;
    }
    String newSsid = portal.arg("ssid"), newPassword = portal.arg("password");
    bool validPassword = newPassword.isEmpty() || (newPassword.length() >= 8 && newPassword.length() <= 63);
    if (newPassword.length() == 64) {
      validPassword = true;
      for (unsigned i=0; i<64; ++i) if (!isxdigit((unsigned char)newPassword[i])) validPassword = false;
    }
    if (!newSsid.length() || newSsid.length() > 32 || !validPassword) {
      portal.send(400, "text/plain", "Invalid SSID or WPA password"); return;
    }
    pendingSsid = newSsid; pendingPassword = newPassword;
    connectPending = true;
    portalMessage = "Đang thử kết nối. Chưa ghi đè cấu hình cũ.";
    showPortal();
  });
  portal.onNotFound([this]() {
    portal.sendHeader("Location", "http://192.168.4.1/", true);
    portal.send(302, "text/plain", "");
  });
  dns.start(53, "*", WiFi.softAPIP());
  portal.begin(); portalActive = true;
  Serial.printf("[SETUP] SSID: %s | Setup password: %s | http://192.168.4.1\n", apName.c_str(), apPassword.c_str());
}

void WiFiManager::stopPortal() {
  portal.stop(); dns.stop(); WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA); portalActive = false; closePortalAt = 0;
}

bool WiFiManager::tick() {
  uint32_t now = millis();
  if (portalActive) { dns.processNextRequest(); portal.handleClient(); }
  if (connectPending) {
    connectPending = false; testing = true; testStarted = now;
    WiFi.disconnect(false, false);
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (testing && connected && WiFi.SSID() == pendingSsid) {
    // Store the pair atomically as one JSON record, read below on subsequent boots.
    JsonDocument credentials;
    credentials["ssid"] = pendingSsid; credentials["password"] = pendingPassword;
    String encoded; serializeJson(credentials, encoded);
    if (settings.putString("credentials", encoded) > 0) {
      ssid = pendingSsid; password = pendingPassword;
      portalMessage = "Kết nối thành công. Đã lưu cấu hình.";
      closePortalAt = now + 10000;
    } else {
      portalMessage = "Kết nối được nhưng lưu thất bại. Vui lòng thử lại.";
    }
    pendingPassword = ""; testing = false;
  } else if (testing && now - testStarted >= 30000) {
    testing = false; pendingPassword = "";
    portalMessage = "Kết nối thất bại. Cấu hình cũ vẫn được giữ. Vui lòng thử lại.";
    WiFi.disconnect(false, false);
    if (ssid.length()) WiFi.begin(ssid.c_str(), password.c_str());
    lastAttempt = now;
    connected = false;
  }
  if (connected) disconnectedSince = now;
  if (!connected && !testing && now - disconnectedSince >= 60000) startPortal();
  if (portalActive && connected && !testing && closePortalAt && int32_t(now-closePortalAt)>=0) stopPortal();
  if (portalActive && connected && !testing && !closePortalAt && WiFi.SSID()==ssid) closePortalAt=now+10000;
  if (connected && !wasConnected) {
    Serial.println("[WIFI] connected");
    if (!ntpSynced.load()) Serial.println("[NTP] waiting for UTC synchronization");
  }
  wasConnected = connected;
  if (!connected && !testing && ssid.length() && millis() - lastAttempt >= 15000) {
    lastAttempt = millis();
    Serial.println("[WIFI] reconnecting");
    WiFi.reconnect();
  }
  if (ready() && !wasSynced) {
    wasSynced = true;
    Serial.printf("[NTP] synchronized UTC %s\n", utcNow().c_str());
  }
  return ready();
}

bool WiFiManager::ready() const {
  // A plausible RTC value alone is not proof of an NTP reply after this boot.
  return WiFi.status() == WL_CONNECTED && ntpSynced.load() &&
         time(nullptr) > 1735689600;
}

String MqttConnection::topic(const char *suffix) const {
  return String("respicare/device/") + DEVICE_ID + "/" + suffix;
}

String MqttConnection::commandTopic() const { return topic("command"); }

void MqttConnection::begin(CommandHandler handler) {
  tls.setCACert(MQTT_CA);
  tls.setHandshakeTimeout(8);
  client.begin(MQTT_HOST, MQTT_PORT, tls);
  client.setOptions(30, false, 1500);  // Persistent session, bounded ACK wait.
  client.onMessage(handler);
}

void MqttConnection::tick(bool networkReady) {
  if (!networkReady) return;
  if (client.connected()) { client.loop(); return; }
  if (int32_t(millis() - nextAttempt) < 0) return;
  Serial.println("[MQTT] connecting TLS");
  String offline = eventPayload("DEVICE_OFFLINE");
  client.setWill(topic("status").c_str(), offline.c_str(), true, 1);
  if (client.connect(DEVICE_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    String online = eventPayload("DEVICE_ONLINE");
    if (client.subscribe(commandTopic(), 1) &&
        client.publish(topic("status"), online, true, 1)) {
      failures = 0;
      Serial.println("[MQTT] TLS connected; command subscribed QoS1");
      Serial.println("[MQTT] DEVICE_ONLINE retained QoS1");
      return;
    }
    client.disconnect();
  }
  Serial.printf("[MQTT] connection/setup failed (error=%d, connack=%d)\n",
                int(client.lastError()), int(client.returnCode()));
  unsigned power = min(failures++, 5U);
  nextAttempt = millis() + (1000U << power) + esp_random() % 500;
}

bool MqttConnection::publishEvent(const char *payload) {
  return client.connected() && client.publish(topic("event"), payload, false, 1);
}

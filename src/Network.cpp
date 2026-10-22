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

void onNtpSync(struct timeval *) {
  ntpSynced.store(true);
}
}

// ============================================================
// Wi-Fi Manager
// ============================================================

void WiFiManager::begin() {
  settings.begin("wifi_setup", false);

  ssid = settings.getString("ssid", WIFI_SSID);
  password = settings.getString("password", WIFI_PASSWORD);

  JsonDocument credentials;
  String saved = settings.getString("credentials", "");

  if (saved.length() &&
      !deserializeJson(credentials, saved) &&
      credentials["ssid"].is<String>() &&
      credentials["password"].is<String>()) {
    ssid = credentials["ssid"].as<String>();
    password = credentials["password"].as<String>();
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  lastAttempt = millis();
  disconnectedSince = lastAttempt;

  ntpSynced.store(false);

  configTime(
      0, 0,
      "pool.ntp.org",
      "time.cloudflare.com");

  sntp_set_time_sync_notification_cb(onNtpSync);

  Serial.println(
      "[WIFI] connecting; waiting for UTC time before TLS");

  if (!ssid.length()) {
    startPortal();
  }
}

// ============================================================
// HTTP helpers
// ============================================================

void WiFiManager::redirectToPortal() {
  portal.sendHeader("Location", "/", true);
  portal.sendHeader("Cache-Control", "no-store");
  portal.send(303, "text/plain", "");
}

// ============================================================
// Wi-Fi Setup Page
// ============================================================

void WiFiManager::showPortal() {
  if (portal.client().localIP() != WiFi.softAPIP()) {
    portal.send(
        403,
        "text/plain",
        "Connect to device setup Wi-Fi first");
    return;
  }

  const bool busy = testing || connectPending;
  const bool success = closePortalAt != 0;

  String html = F(
      "<!doctype html>"
      "<html lang='vi'>"
      "<head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' "
      "content='width=device-width,initial-scale=1'>"
      "<title>RespiCare Wi-Fi Setup</title>"
      "<style>"
      "*{box-sizing:border-box}"
      "body{font:16px Arial,sans-serif;"
      "background:#f3f6fb;color:#172033;"
      "margin:0;padding:32px 16px}"
      ".card{max-width:460px;margin:auto;background:#fff;"
      "border-radius:14px;padding:26px;"
      "box-shadow:0 6px 24px rgba(0,0,0,.08)}"
      "h1{font-size:24px;margin:0 0 10px}"
      "p{line-height:1.6;color:#526072}"
      "label{display:block;font-weight:600;margin-top:18px}"
      "input{width:100%;padding:12px;margin-top:8px;"
      "border:1px solid #cbd5e1;border-radius:8px;"
      "font-size:16px}"
      "button{width:100%;padding:13px;margin-top:22px;"
      "background:#2563eb;color:white;border:0;"
      "border-radius:8px;font-size:16px;cursor:pointer}"
      ".status{padding:14px;margin:20px 0;"
      "border-radius:8px;background:#eff6ff;"
      "color:#1e40af;line-height:1.6}"
      ".success{background:#ecfdf5;color:#065f46}"
      ".error{background:#fef2f2;color:#991b1b}"
      ".spin{width:24px;height:24px;"
      "border:3px solid #bfdbfe;"
      "border-top-color:#2563eb;border-radius:50%;"
      "animation:spin 1s linear infinite;"
      "margin:16px auto}"
      "@keyframes spin{to{transform:rotate(360deg)}}"
      ".hint{font-size:13px;color:#64748b}"
      "</style>"
      "</head>"
      "<body><div class='card'>"
      "<h1>RespiCare Wi-Fi Setup</h1>"
      "<p>Ket noi thiet bi voi mang Wi-Fi 2.4 GHz.</p>");

  // portalMessage contains firmware-controlled messages only.
  // Credentials are never echoed to the browser.
  html += "<div class='status";

  if (success) {
    html += " success";
  } else if (!busy &&
             portalMessage.indexOf("thất bại") >= 0) {
    html += " error";
  }

  html += "'>";
  html += portalMessage;
  html += "</div>";

  if (busy) {
    html += F(
        "<div class='spin'></div>"
        "<p style='text-align:center'>"
        "Dang thu ket noi Wi-Fi...</p>"
        "<p class='hint'>"
        "Qua trinh co the mat toi da 30 giay."
        "</p>"
        "<script>"
        "setTimeout(function(){"
        "window.location.replace('/');"
        "},3000);"
        "</script>");
  } else if (success) {
    html += F(
        "<h3>Da ket noi thanh cong!</h3>"
        "<p>Thong tin Wi-Fi da duoc luu.</p>"
        "<p>Thiet bi se tu dong dong mang Setup.</p>"
        "<p class='hint'>"
        "Hay ket noi lai Internet va mo web RespiCare."
        "</p>");
  } else {
    html += F(
        "<form method='POST' action='/configure'>"
        "<label for='ssid'>Ten Wi-Fi (SSID)</label>"
        "<input id='ssid' name='ssid' maxlength='32'"
        " required autocomplete='off'>"
        "<label for='password'>Mat khau Wi-Fi</label>"
        "<input id='password' name='password'"
        " type='password' maxlength='64'"
        " autocomplete='off'>"
        "<button type='submit'>Ket noi</button>"
        "</form>"
        "<p class='hint'>"
        "Nhap Wi-Fi nha ban, khong phai RespiCare-Setup."
        "</p>");
  }

  html += F("</div></body></html>");

  portal.sendHeader("Cache-Control", "no-store");
  portal.send(
      200,
      "text/html; charset=utf-8",
      html);
}

// ============================================================
// Start Wi-Fi Setup Portal
// ============================================================

void WiFiManager::startPortal() {
  if (portalActive) {
    return;
  }

  String apPassword =
      settings.getString("ap_password", "");

  if (apPassword.length() < 12) {
    char generated[17];

    snprintf(
        generated,
        sizeof(generated),
        "%08lx%08lx",
        (unsigned long)esp_random(),
        (unsigned long)esp_random());

    apPassword = generated;
    settings.putString("ap_password", apPassword);
  }

  String apName =
      String("RespiCare-Setup-") +
      String(DEVICE_ID).substring(0, 16);

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAPConfig(
      IPAddress(192, 168, 4, 1),
      IPAddress(192, 168, 4, 1),
      IPAddress(255, 255, 255, 0));

  if (!WiFi.softAP(
          apName.c_str(),
          apPassword.c_str())) {
    Serial.println("[SETUP] AP start failed");
    return;
  }

  testing = false;
  connectPending = false;
  closePortalAt = 0;

  portalMessage = "Nhập thông tin Wi-Fi mới.";

  portal.on("/", HTTP_GET, [this]() {
    showPortal();
  });

  // Avoid browser GET requests returning handler-not-found.
  portal.on("/configure", HTTP_GET, [this]() {
    redirectToPortal();
  });

  portal.on("/configure", HTTP_POST, [this]() {
    if (portal.client().localIP() != WiFi.softAPIP()) {
      portal.send(
          403,
          "text/plain",
          "Connect to device setup Wi-Fi first");
      return;
    }

    // Duplicate POSTs redirect to the status page.
    if (testing || connectPending || closePortalAt) {
      redirectToPortal();
      return;
    }

    String newSsid = portal.arg("ssid");
    String newPassword = portal.arg("password");

    bool validPassword =
        newPassword.isEmpty() ||
        (newPassword.length() >= 8 &&
         newPassword.length() <= 63);

    if (newPassword.length() == 64) {
      validPassword = true;

      for (unsigned i = 0; i < 64; ++i) {
        if (!isxdigit(
                (unsigned char)newPassword[i])) {
          validPassword = false;
          break;
        }
      }
    }

    if (newSsid.isEmpty() ||
        newSsid.length() > 32 ||
        !validPassword) {
      portal.send(
          400,
          "text/plain",
          "Invalid SSID or WPA password");
      return;
    }

    pendingSsid = newSsid;
    pendingPassword = newPassword;

    connectPending = true;
    portalMessage =
        "Đang thử kết nối. Chưa ghi đè cấu hình cũ.";

    // Never log credentials.
    Serial.printf(
        "[SETUP] Connection request accepted "
        "(SSID length=%u)\n",
        unsigned(newSsid.length()));

    // POST -> 303 -> GET
    redirectToPortal();
  });

  portal.onNotFound([this]() {
    portal.sendHeader(
        "Location",
        "http://192.168.4.1/",
        true);
    portal.send(302, "text/plain", "");
  });

  dns.start(53, "*", WiFi.softAPIP());

  portal.begin();
  portalActive = true;

  Serial.printf(
      "[SETUP] SSID: %s | Setup password: %s | "
      "http://192.168.4.1\n",
      apName.c_str(),
      apPassword.c_str());
}

// ============================================================
// Stop Wi-Fi Setup Portal
// ============================================================

void WiFiManager::stopPortal() {
  portal.stop();
  dns.stop();

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  portalActive = false;
  closePortalAt = 0;

  Serial.println("[SETUP] Portal closed");
}

// ============================================================
// Wi-Fi State Machine
// ============================================================

bool WiFiManager::tick() {
  uint32_t now = millis();

  if (portalActive) {
    dns.processNextRequest();
    portal.handleClient();
  }

  if (connectPending) {
    connectPending = false;
    testing = true;
    testStarted = now;

    Serial.println(
        "[SETUP] Trying new Wi-Fi credentials");

    WiFi.disconnect(false, false);
    WiFi.begin(
        pendingSsid.c_str(),
        pendingPassword.c_str());
  }

  bool connected =
      WiFi.status() == WL_CONNECTED;

  if (testing &&
      connected &&
      WiFi.SSID() == pendingSsid) {

    // Store SSID and password together in NVS.
    JsonDocument credentials;
    credentials["ssid"] = pendingSsid;
    credentials["password"] = pendingPassword;

    String encoded;
    serializeJson(credentials, encoded);

    if (settings.putString(
            "credentials", encoded) > 0) {

      ssid = pendingSsid;
      password = pendingPassword;

      portalMessage =
          "Kết nối thành công. Đã lưu cấu hình.";

      closePortalAt = now + 10000;

      Serial.printf(
          "[SETUP] Wi-Fi connected and saved; IP=%s\n",
          WiFi.localIP().toString().c_str());

    } else {
      portalMessage =
          "Kết nối được nhưng lưu thất bại. "
          "Vui lòng thử lại.";

      Serial.println(
          "[SETUP] NVS save failed");

      // Restore old credentials if persistence fails.
      WiFi.disconnect(false, false);
      if (ssid.length()) {
        WiFi.begin(ssid.c_str(), password.c_str());
      }
      connected = false;
      lastAttempt = now;
    }

    pendingPassword = "";
    pendingSsid = "";
    testing = false;

  } else if (
      testing &&
      now - testStarted >= 30000) {

    testing = false;

    pendingPassword = "";
    pendingSsid = "";

    portalMessage =
        "Kết nối thất bại. "
        "Cấu hình cũ vẫn được giữ. "
        "Vui lòng thử lại.";

    Serial.printf(
        "[SETUP] Wi-Fi connection timeout, status=%d\n",
        int(WiFi.status()));

    WiFi.disconnect(false, false);

    if (ssid.length()) {
      WiFi.begin(ssid.c_str(), password.c_str());
    }

    lastAttempt = now;
    connected = false;
  }

  if (connected) {
    disconnectedSince = now;
  }

  if (!connected &&
      !testing &&
      now - disconnectedSince >= 60000) {
    startPortal();
  }

  if (portalActive &&
      connected &&
      !testing &&
      closePortalAt &&
      int32_t(now - closePortalAt) >= 0) {
    stopPortal();
  }

  if (portalActive &&
      connected &&
      !testing &&
      !closePortalAt &&
      WiFi.SSID() == ssid) {
    closePortalAt = now + 10000;
  }

  if (connected && !wasConnected) {
    Serial.println("[WIFI] connected");

    if (!ntpSynced.load()) {
      Serial.println(
          "[NTP] waiting for UTC synchronization");
    }
  }

  wasConnected = connected;

  if (!connected &&
      !testing &&
      ssid.length() &&
      millis() - lastAttempt >= 15000) {

    lastAttempt = millis();

    Serial.println("[WIFI] reconnecting");
    WiFi.reconnect();
  }

  if (ready() && !wasSynced) {
    wasSynced = true;

    Serial.printf(
        "[NTP] synchronized UTC %s\n",
        utcNow().c_str());
  }

  return ready();
}

bool WiFiManager::ready() const {
  return WiFi.status() == WL_CONNECTED &&
         ntpSynced.load() &&
         time(nullptr) > 1735689600;
}

// ============================================================
// MQTT Connection - Unchanged
// ============================================================

String MqttConnection::topic(
    const char *suffix) const {
  return String("respicare/device/") +
         DEVICE_ID + "/" + suffix;
}

String MqttConnection::commandTopic() const {
  return topic("command");
}

void MqttConnection::begin(
    CommandHandler handler) {
  tls.setCACert(MQTT_CA);
  tls.setHandshakeTimeout(8);

  client.begin(
      MQTT_HOST,
      MQTT_PORT,
      tls);

  client.setOptions(30, false, 1500);
  client.onMessage(handler);
}

void MqttConnection::tick(bool networkReady) {
  if (!networkReady) {
    return;
  }

  if (client.connected()) {
    client.loop();
    return;
  }

  if (int32_t(millis() - nextAttempt) < 0) {
    return;
  }

  Serial.println("[MQTT] connecting TLS");

  String offline = eventPayload("DEVICE_OFFLINE");

  client.setWill(
      topic("status").c_str(),
      offline.c_str(),
      true,
      1);

  if (client.connect(
          DEVICE_ID,
          MQTT_USERNAME,
          MQTT_PASSWORD)) {

    String online = eventPayload("DEVICE_ONLINE");

    if (client.subscribe(commandTopic(), 1) &&
        client.publish(
            topic("status"),
            online,
            true,
            1)) {

      failures = 0;

      Serial.println(
          "[MQTT] TLS connected; "
          "command subscribed QoS1");

      Serial.println(
          "[MQTT] DEVICE_ONLINE retained QoS1");

      return;
    }

    client.disconnect();
  }

  Serial.printf(
      "[MQTT] connection/setup failed "
      "(error=%d, connack=%d)\n",
      int(client.lastError()),
      int(client.returnCode()));

  unsigned power = min(failures++, 5U);

  nextAttempt =
      millis() +
      (1000U << power) +
      esp_random() % 500;
}

bool MqttConnection::publishEvent(
    const char *payload) {
  return client.connected() &&
         client.publish(
             topic("event"),
             payload,
             false,
             1);
}

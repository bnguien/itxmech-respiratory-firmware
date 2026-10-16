#include "Network.h"
#include "Protocol.h"
#include <BuildConfig.h>
#include <WiFi.h>
#include <time.h>
#include <atomic>
#include <esp_sntp.h>

namespace {
std::atomic<bool> ntpSynced{false};
void onNtpSync(struct timeval *) { ntpSynced.store(true); }
}

void WiFiManager::begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastAttempt = millis();
  ntpSynced.store(false);
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  sntp_set_time_sync_notification_cb(onNtpSync);
  Serial.println("[WIFI] connecting; waiting for UTC time before TLS");
}

bool WiFiManager::tick() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.println("[WIFI] connected");
    if (!ntpSynced.load()) Serial.println("[NTP] waiting for UTC synchronization");
  }
  wasConnected = connected;
  if (!connected && millis() - lastAttempt >= 15000) {
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

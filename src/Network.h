#pragma once
#include <Arduino.h>
#include <MQTT.h>
#include <WiFiClientSecure.h>

using CommandHandler = void (*)(String &, String &);

class WiFiManager {
 public:
  void begin();
  bool tick();
  bool ready() const;
 private:
  uint32_t lastAttempt = 0;
  bool wasConnected = false;
  bool wasSynced = false;
};

class MqttConnection {
 public:
  void begin(CommandHandler handler);
  void tick(bool networkReady);
  bool publishEvent(const char *payload);
  String commandTopic() const;
 private:
  WiFiClientSecure tls;
  MQTTClient client{2048};
  uint32_t nextAttempt = 0;
  unsigned failures = 0;
  String topic(const char *suffix) const;
};

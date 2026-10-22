#pragma once

#include <Arduino.h>
#include <MQTT.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

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

  WebServer portal{80};
  DNSServer dns;
  Preferences settings;

  String ssid;
  String password;
  String pendingSsid;
  String pendingPassword;

  bool portalActive = false;
  bool testing = false;
  bool connectPending = false;

  uint32_t disconnectedSince = 0;
  uint32_t testStarted = 0;
  uint32_t closePortalAt = 0;

  String portalMessage;

  void startPortal();
  void stopPortal();
  void showPortal();
  void redirectToPortal();
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

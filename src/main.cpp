#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "config.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "ota_handler.h"
#include "web_server.h"

static DNSServer dnsServer;
static bool setupApActive = false;
static bool captiveDnsActive = false;
static bool lastWifiConnected = false;
static bool gatewayServicesInitialized = false;
static bool mdnsStarted = false;
static unsigned long lastWifiAttempt = 0;
static unsigned long wifiRetryDelayMs = 20000;
static unsigned long setupApStopAt = 0;
static bool setupApStopScheduled = false;
static constexpr unsigned long SETUP_AP_SUCCESS_GRACE_MS = 30000;
static constexpr unsigned long WIFI_RETRY_MAX_DELAY_MS = 60000;

static bool applyStationNetworkConfig() {
  if (!wifiUseDhcp) {
    IPAddress localIP, gateway, subnet, dns;
    if (!localIP.fromString(wifiStaticIp) ||
        !gateway.fromString(wifiGateway) ||
        !subnet.fromString(wifiSubnet) ||
        !dns.fromString(wifiDns) ||
        !WiFi.config(localIP, gateway, subnet, dns)) {
      addLog("WIFI: saved static IPv4 configuration is invalid or could not be applied",
             "error");
      return false;
    }
    addLog("WIFI: static IPv4 configuration applied: " +
           localIP.toString(), "info");
    return true;
  }

  const IPAddress automaticAddress(0, 0, 0, 0);
  if (!WiFi.config(automaticAddress, automaticAddress, automaticAddress,
                   automaticAddress)) {
    addLog("WIFI: failed to enable DHCP", "error");
    return false;
  }
  addLog("WIFI: using DHCP", "info");
  return true;
}

static bool startStationConnection() {
  if (!wifi_configured || wifi_ssid.isEmpty()) return false;
  if (!applyStationNetworkConfig()) return false;

  lastWifiAttempt = millis();
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  return true;
}

static void ensureSetupAccessPoint() {
  if (setupApActive) return;
  if (!WiFi.softAP(AP_SSID, ap_password.c_str())) {
    addLog("WIFI: failed to start setup AP", "error");
    return;
  }

  addSerialLog("Setup AP password: " + ap_password, "info");
  setupApActive = true;
  captiveDnsActive = dnsServer.start(53, "*", WiFi.softAPIP());
  if (!captiveDnsActive) {
    addLog("WIFI: failed to start captive DNS; open the setup AP IP directly",
           "warning");
  }
  addLog("WIFI: setup AP started at " + WiFi.softAPIP().toString(), "success");
}

static void stopSetupAccessPoint() {
  if (!setupApActive) return;
  if (captiveDnsActive) dnsServer.stop();
  captiveDnsActive = false;
  WiFi.softAPdisconnect(true);
  setupApActive = false;
  WiFi.mode(WIFI_STA);
  addLog("WIFI: setup AP stopped after station connection", "info");
}

static void scheduleSetupAccessPointStop() {
  if (!setupApActive || setupApStopScheduled) return;
  setupApStopAt = millis() + SETUP_AP_SUCCESS_GRACE_MS;
  setupApStopScheduled = true;
  addLog("WIFI: keeping setup AP available briefly so setup clients can see the connection result",
         "info");
}

static void prepareSetupAccessPointForReconnect() {
  if (!setupApActive) {
    WiFi.mode(WIFI_AP_STA);
    ensureSetupAccessPoint();
    return;
  }
  if (!captiveDnsActive) {
    captiveDnsActive = dnsServer.start(53, "*", WiFi.softAPIP());
    if (!captiveDnsActive) {
      addLog("WIFI: failed to restart captive DNS; open the setup AP IP directly",
             "warning");
    }
  }
}

static void initializeGatewayServices() {
  if (gatewayServicesInitialized || WiFi.status() != WL_CONNECTED) return;

  const bool rtuInitialized = initModbusRTU();
  const bool tcpInitialized = rtuInitialized && initModbusTCP();
  updateGatewayInitializationState(true, rtuInitialized, tcpInitialized);
  gatewayServicesInitialized = true;
  if (!tcpInitialized) {
    modbusReady = false;
    addLog("STATE: remains INITIALIZATION; RTU/TCP bridge initialization failed",
           "error");
  }
}

static void startMdns(bool wifiConnected) {
  if (mdnsStarted) MDNS.end();
  mdnsStarted = MDNS.begin(OTA_HOSTNAME);
  if (mdnsStarted) {
    MDNS.addService("http", "tcp", WEB_PORT);
    const String setupUrl = "http://" + String(OTA_HOSTNAME) + ".local";
    addLog(wifiConnected
             ? "MDNS: dashboard available at " + setupUrl
             : "MDNS: setup page available at " + setupUrl +
                 " (AP IP " + WiFi.softAPIP().toString() + ")",
           "success");
  } else {
    addLog("MDNS: failed to start; use the logged IP address as fallback",
           "warning");
  }
}

static void maintainWifiConnection() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    if (!lastWifiConnected) {
      wifi_configured = true;
      wifiRetryDelayMs = 20000;
      addLog("WIFI: connected; IP=" + WiFi.localIP().toString(), "success");
      scheduleSetupAccessPointStop();
      initializeGatewayServices();
      startMdns(true);
    }
    lastWifiConnected = true;
    updateGatewayNetworkState(true);
    if (setupApStopScheduled &&
        static_cast<int32_t>(millis() - setupApStopAt) >= 0) {
      stopSetupAccessPoint();
      setupApStopScheduled = false;
    }
    return;
  }

  updateGatewayNetworkState(false);
  if (lastWifiConnected) {
    setupApStopScheduled = false;
    addLog("WIFI: station disconnected; enabling setup AP and reconnecting",
           "warning");
    if (wifi_configured) {
      prepareSetupAccessPointForReconnect();
      lastWifiAttempt = millis();
      WiFi.reconnect();
    }
    lastWifiConnected = false;
  }

  if (wifi_configured &&
      !wifiNetworkScanInProgress() &&
      millis() - lastWifiAttempt >= wifiRetryDelayMs) {
    addLog("WIFI: retrying saved station connection", "info");
    if (!startStationConnection()) {
      lastWifiAttempt = millis();
    }
    wifiRetryDelayMs = min(wifiRetryDelayMs * 2, WIFI_RETRY_MAX_DELAY_MS);
  }

  if (captiveDnsActive) dnsServer.processNextRequest();
}

void setup() {
  Serial.begin(115200);
  startMs = millis();
  if (!initLogging()) {
    Serial.println("LOG: unable to create log-buffer mutex");
  }
  addLog("BOOT: starting ESP32 Modbus Gateway", "info");

  addLog("BOOT: loading persistent configuration", "info");
  loadConfig();
  addLog(String("BOOT: saved WiFi credentials ") +
         (wifi_configured ? "found" : "not found"), "info");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(wifi_configured);
  WiFi.persistent(false);
  WiFi.setHostname(OTA_HOSTNAME);

  if (wifi_configured) {
    addLog("WIFI: starting station connection with setup AP available",
           "info");
    WiFi.mode(WIFI_AP_STA);
    ensureSetupAccessPoint();
    startStationConnection();
    lastWifiAttempt = millis();
  } else {
    addLog("WIFI: no saved station credentials; entering setup AP mode",
           "warning");
    WiFi.mode(WIFI_AP);
    ensureSetupAccessPoint();
  }

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (!wifiConnected) {
    setupApStopScheduled = false;
    updateGatewayInitializationState(false, false, false);
    addLog("STATE: remains INITIALIZATION; Wi-Fi setup or connection required",
           "warning");
  } else {
    wifi_configured = true;
    scheduleSetupAccessPointStop();
    initializeGatewayServices();
  }

  lastWifiConnected = wifiConnected;
  startMdns(wifiConnected);

  addLog("HTTP: starting configuration and status server", "info");
  initWebServer();
  initOTA();
  addLog("STATE: HTTP_SERVER_READY (port " + String(WEB_PORT) + ")", "success");
}

void loop() {
  handleWebServerRestart();
  maintainWifiConnection();
  maintainModbusPathState();

  static unsigned long lastStatsSave = 0;
  if (millis() - lastStatsSave > 300000) {
    saveStats();
    lastStatsSave = millis();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}

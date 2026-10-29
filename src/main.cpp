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
static constexpr unsigned long SERIAL_CLI_AUTH_TIMEOUT_MS = 5UL * 60UL * 1000UL;
static bool serialCliAuthenticated = false;
static unsigned long serialCliLastAuthenticatedAt = 0;

static bool readSerialArgument(const String& command, size_t& position,
                               String& argument) {
  while (position < command.length() &&
         isspace(static_cast<unsigned char>(command[position]))) {
    ++position;
  }
  if (position >= command.length()) return false;

  const char quote = command[position] == '"' || command[position] == '\''
    ? command[position++] : '\0';
  argument = "";
  while (position < command.length()) {
    const char current = command[position++];
    if (quote != '\0' && current == quote) {
      return position == command.length() ||
        isspace(static_cast<unsigned char>(command[position]));
    }
    if (quote == '\0' &&
        isspace(static_cast<unsigned char>(current))) {
      return true;
    }
    if (current == '\\' && quote != '\0' &&
        position < command.length() &&
        (command[position] == quote || command[position] == '\\')) {
      argument += command[position++];
    } else {
      argument += current;
    }
  }
  return quote == '\0';
}

static void executeSerialCommand(String command) {
  const String rawCommand = command;
  command.trim();
  if (command.isEmpty()) return;

  if (command.equalsIgnoreCase("help")) {
    Serial.println("--- Serial Commands ---");
    Serial.println("auth <admin-password>        - Unlock Wi-Fi commands for 5 minutes");
    Serial.println("logout                        - Lock Wi-Fi commands");
    Serial.println("status                        - Show Wi-Fi connection status");
    Serial.println("wifi set <ssid> <password>    - Save Wi-Fi credentials and restart");
    Serial.println("wifi clear                    - Clear saved Wi-Fi and restart");
    Serial.println("factory default               - Restore factory defaults and restart");
    Serial.println("Quote arguments containing spaces; use \\\" or \\\\ inside quotes.");
    Serial.println("------------------------------");
    return;
  }

  if (command.equalsIgnoreCase("status")) {
    Serial.printf("Wi-Fi connected: %s\n",
                  WiFi.status() == WL_CONNECTED ? "YES" : "NO");
    Serial.printf("SSID: %s\n",
                  WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "not connected");
    Serial.printf("IP address: %s\n",
                  WiFi.status() == WL_CONNECTED
                    ? WiFi.localIP().toString().c_str() : "0.0.0.0");
    Serial.printf("Saved Wi-Fi configuration: %s\n",
                  wifi_configured ? "YES" : "NO");
    return;
  }

  size_t rawCommandStart = 0;
  while (rawCommandStart < rawCommand.length() &&
         isspace(static_cast<unsigned char>(rawCommand[rawCommandStart]))) {
    ++rawCommandStart;
  }
  if (rawCommand.length() >= rawCommandStart + 5 &&
      rawCommand.substring(rawCommandStart, rawCommandStart + 5)
        .equalsIgnoreCase("auth ")) {
    serialCliAuthenticated = false;
    const String password = rawCommand.substring(rawCommandStart + 5);
    if (!httpCredentialsConfigured()) {
      Serial.println("ERROR: Set the admin password in the web interface first.");
    } else if (!verifyHttpPassword(password)) {
      Serial.println("ERROR: Invalid admin password.");
    } else {
      serialCliAuthenticated = true;
      serialCliLastAuthenticatedAt = millis();
      Serial.println("SUCCESS: Wi-Fi commands unlocked for 5 minutes.");
    }
    return;
  }

  if (command.equalsIgnoreCase("logout")) {
    serialCliAuthenticated = false;
    Serial.println("SUCCESS: Wi-Fi commands locked.");
    return;
  }

  const bool wifiSetPrefix = command.length() >= 8 &&
    command.substring(0, 8).equalsIgnoreCase("wifi set");
  const bool wifiSet = wifiSetPrefix &&
    (command.length() == 8 ||
     isspace(static_cast<unsigned char>(command[8])));
  const bool wifiClear = command.equalsIgnoreCase("wifi clear");
  const bool factoryDefault = command.equalsIgnoreCase("factory default");
  if (!wifiSet && !wifiClear && !factoryDefault) {
    Serial.println("ERROR: Unknown command. Type 'help' for available commands.");
    return;
  }

  if (!serialCliAuthenticated ||
      millis() - serialCliLastAuthenticatedAt >= SERIAL_CLI_AUTH_TIMEOUT_MS) {
    serialCliAuthenticated = false;
    Serial.println("ERROR: Authenticate with 'auth <admin-password>' first.");
    return;
  }
  serialCliLastAuthenticatedAt = millis();

  if (factoryDefault) {
    if (!factoryReset()) {
      Serial.println("ERROR: Factory reset failed; see serial log.");
      return;
    }
    Serial.println("SUCCESS: Factory defaults restored; restarting.");
  } else if (wifiClear) {
    clearWifiConfig();
    Serial.println("SUCCESS: Saved Wi-Fi settings cleared; restarting.");
  } else {
    size_t position = 8;
    String ssid;
    String password;
    if (!readSerialArgument(command, position, ssid) ||
        !readSerialArgument(command, position, password)) {
      Serial.println("ERROR: Use wifi set <ssid> <password>; quote values containing spaces.");
      return;
    }
    while (position < command.length() &&
           isspace(static_cast<unsigned char>(command[position]))) {
      ++position;
    }
    if (position != command.length() || ssid.isEmpty() ||
        ssid.length() > 32 || password.length() > 64) {
      Serial.println("ERROR: SSID must be 1-32 characters and password at most 64 characters.");
      return;
    }
    if (!saveWifiConfig(ssid, password, wifiUseDhcp, wifiStaticIp,
                        wifiGateway, wifiSubnet, wifiDns)) {
      Serial.println("ERROR: Failed to save Wi-Fi settings.");
      return;
    }
    Serial.println("SUCCESS: Wi-Fi settings saved; restarting.");
  }

  Serial.flush();
  delay(250);
  ESP.restart();
}

static void checkSerialCommands() {
  static String commandBuffer;
  static bool discardingOverflow = false;
  static bool previousCharacterWasCarriageReturn = false;
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' && previousCharacterWasCarriageReturn) {
      previousCharacterWasCarriageReturn = false;
      continue;
    }
    previousCharacterWasCarriageReturn = character == '\r';
    if (character == '\r' || character == '\n') {
      Serial.println();
      if (discardingOverflow) {
        Serial.println("ERROR: Serial CLI command exceeds 256 characters.");
      } else if (!commandBuffer.isEmpty()) {
        executeSerialCommand(commandBuffer);
      }
      Serial.print("> ");
      commandBuffer = "";
      discardingOverflow = false;
    } else if (!discardingOverflow && character != '\r') {
      if (commandBuffer.length() < 256) {
        commandBuffer += character;
      } else {
        commandBuffer = "";
        discardingOverflow = true;
      }
    }
  }
}

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
  mdnsStarted = MDNS.begin(gatewayHostname.c_str());
  if (mdnsStarted) {
    MDNS.addService("http", "tcp", WEB_PORT);
    const String setupUrl = "http://" + gatewayHostname + ".local";
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
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  const unsigned long serialWaitStarted = millis();
  while (!Serial && millis() - serialWaitStarted < 2000) {
    delay(10);
  }
#endif
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
  WiFi.setHostname(gatewayHostname.c_str());

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
  Serial.println("Serial console ready. Type 'help' for commands.");
  Serial.print("> ");
}

void loop() {
  checkSerialCommands();
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

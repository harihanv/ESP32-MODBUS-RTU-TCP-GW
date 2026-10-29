#include "config.h"
#include <cstring>

const char* AP_SSID      = "AP-modbus-gw";
String gatewayHostname = DEFAULT_GATEWAY_HOSTNAME;
String ap_password = "initpass";

UARTConfig uartCfg;
uint16_t modbusTcpPort = MODBUS_DEFAULT_TCP_PORT;
uint32_t modbusRtuTimeoutMs = MODBUS_DEFAULT_RTU_TIMEOUT_MS;
uint32_t modbusTcpTimeoutMs = MODBUS_DEFAULT_TCP_TIMEOUT_MS;
String wifi_ssid = "";
String wifi_password = "";
bool wifi_configured = false;
bool wifiUseDhcp = true;
String wifiStaticIp = "";
String wifiGateway = "";
String wifiSubnet = "";
String wifiDns = "";

String http_user;

unsigned long reqCount = 0;
unsigned long errCount = 0;
unsigned long tcpConnections = 0;
std::atomic<unsigned long> rtuTxCount{0};
std::atomic<unsigned long> rtuRxCount{0};
std::atomic<unsigned long> tcpTxCount{0};
std::atomic<unsigned long> tcpRxCount{0};
unsigned long startMs = 0;

static Preferences preferences;
static String authPassword;
static bool authConfigured = false;
static std::deque<String> logBuffer;
static SemaphoreHandle_t logMutex = nullptr;
static std::atomic<LogLevel> serialLogLevel{LogLevel::INFO};
static std::atomic<LogLevel> webLogLevel{LogLevel::INFO};

bool validMdnsHostname(const String& hostname) {
  if (hostname.isEmpty() || hostname.length() > 63 ||
      hostname[0] == '-' || hostname[hostname.length() - 1] == '-') {
    return false;
  }
  for (size_t i = 0; i < hostname.length(); ++i) {
    const char character = hostname[i];
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '-')) {
      return false;
    }
  }
  return true;
}

static LogLevel parseLogLevel(const char* level) {
  if (strcmp(level, "error") == 0) return LogLevel::ERROR;
  if (strcmp(level, "warning") == 0) return LogLevel::WARNING;
  if (strcmp(level, "debug") == 0) return LogLevel::DEBUG;
  return LogLevel::INFO;
}

static bool logLevelValid(LogLevel level) {
  return static_cast<uint8_t>(level) <=
         static_cast<uint8_t>(LogLevel::DEBUG);
}

static String formatLogEntry(const String& msg, const char* level) {
  return "[" + String((millis() - startMs) / 1000) + "s] [" +
         String(level) + "] " + msg;
}

bool initLogging() {
  if (logMutex) return true;
  logMutex = xSemaphoreCreateMutex();
  return logMutex != nullptr;
}

LogLevel configuredSerialLogLevel() {
  return serialLogLevel.load();
}

LogLevel configuredWebLogLevel() {
  return webLogLevel.load();
}

bool saveLogLevels(LogLevel serialLevel, LogLevel webLevel) {
  if (!logLevelValid(serialLevel) || !logLevelValid(webLevel)) return false;

  Preferences logPreferences;
  if (!logPreferences.begin("modbus-gw", false)) {
    addLog("LOG: could not open non-volatile storage", "error");
    return false;
  }
  const uint8_t storedLevels =
    (static_cast<uint8_t>(serialLevel) << 2) |
    static_cast<uint8_t>(webLevel);
  const size_t bytesWritten =
    logPreferences.putUChar("log_levels", storedLevels);
  const bool verified =
    bytesWritten == 1 &&
    logPreferences.getUChar("log_levels", UINT8_MAX) == storedLevels;
  logPreferences.end();
  if (!verified) {
    addLog("LOG: configured levels could not be verified in NVS", "error");
    return false;
  }

  serialLogLevel.store(serialLevel);
  webLogLevel.store(webLevel);
  addLog("LOG: output levels updated", "success");
  return true;
}

bool copyLogs(std::deque<String>& logs) {
  if (!logMutex || xSemaphoreTake(logMutex, portMAX_DELAY) != pdTRUE)
    return false;
  logs = logBuffer;
  xSemaphoreGive(logMutex);
  return true;
}

bool clearLogs() {
  if (!logMutex || xSemaphoreTake(logMutex, portMAX_DELAY) != pdTRUE)
    return false;
  logBuffer.clear();
  xSemaphoreGive(logMutex);
  return true;
}

static bool persistHttpAuth(Preferences& store, const String& user,
                            const String& password) {
  const size_t userBytes = store.putString("http_user", user);
  const size_t passwordBytes = store.putString("http_pass", password);
  const bool verified =
    userBytes == user.length() && passwordBytes == password.length() &&
    store.getString("http_user", "") == user &&
    store.getString("http_pass", "") == password;
  if (!verified) return false;

  bool obsoleteKeysRemoved = true;
  if (store.isKey("auth_salt") && !store.remove("auth_salt"))
    obsoleteKeysRemoved = false;
  if (store.isKey("auth_hash") && !store.remove("auth_hash"))
    obsoleteKeysRemoved = false;
  if (store.isKey("auth_iter") && !store.remove("auth_iter"))
    obsoleteKeysRemoved = false;
  if (!obsoleteKeysRemoved) {
    addLog("AUTH: could not remove an obsolete hashed credential from NVS",
           "warning");
  }
  return true;
}

void addLog(const String &msg, const char* level) {
  const LogLevel severity = parseLogLevel(level);
  const bool serialEnabled = static_cast<uint8_t>(severity) <=
    static_cast<uint8_t>(serialLogLevel.load());
  const bool webEnabled = static_cast<uint8_t>(severity) <=
    static_cast<uint8_t>(webLogLevel.load());
  if (!serialEnabled && !webEnabled) {
    return;
  }

  const String entry = formatLogEntry(msg, level);
  if (serialEnabled) Serial.println(entry);
  if (!webEnabled) return;
  if (!logMutex || xSemaphoreTake(logMutex, portMAX_DELAY) != pdTRUE) {
    Serial.println("LOG: failed to lock the in-memory log buffer");
    return;
  }
  if (logBuffer.size() >= MAX_LOG_ENTRIES) {
    logBuffer.pop_front();
  }
  logBuffer.push_back(entry);
  xSemaphoreGive(logMutex);
}

void addSerialLog(const String& msg, const char* level) {
  if (static_cast<uint8_t>(parseLogLevel(level)) <=
      static_cast<uint8_t>(serialLogLevel.load())) {
    Serial.println(formatLogEntry(msg, level));
  }
}

String bytesToHex(const uint8_t* data, size_t len) {
  String result = "";
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) result += "0";
    result += String(data[i], HEX);
    if (i < len - 1) result += " ";
  }
  result.toUpperCase();
  return result;
}

void loadConfig() {
  preferences.begin("modbus-gw", false);
  gatewayHostname = preferences.getString(
    "hostname", DEFAULT_GATEWAY_HOSTNAME);
  if (!validMdnsHostname(gatewayHostname)) {
    addLog("SETTINGS: saved hostname is invalid; using default hostname",
           "warning");
    gatewayHostname = DEFAULT_GATEWAY_HOSTNAME;
  }
  uartCfg.baud = preferences.getUInt("baud", MODBUS_DEFAULT_BAUD);
  uartCfg.stop_bits = preferences.getUChar("stop_bits", MODBUS_DEFAULT_STOPBITS);
  uartCfg.data_bits = preferences.getUChar("data_bits", MODBUS_DEFAULT_DATABITS);
  uartCfg.parity = preferences.getChar("parity", MODBUS_DEFAULT_PARITY);
  modbusTcpPort = preferences.getUShort("tcp_port", MODBUS_DEFAULT_TCP_PORT);
  modbusRtuTimeoutMs = preferences.getUInt("rtu_timeout_ms",
                                          MODBUS_DEFAULT_RTU_TIMEOUT_MS);
  modbusTcpTimeoutMs = preferences.getUInt("tcp_timeout_ms",
                                          MODBUS_DEFAULT_TCP_TIMEOUT_MS);
  reqCount = preferences.getULong("req_count", 0);
  errCount = preferences.getULong("err_count", 0);
  wifi_ssid = preferences.getString("wifi_ssid", "");
  wifi_password = preferences.getString("wifi_password", "");
  wifi_configured = (wifi_ssid.length() > 0);
  wifiUseDhcp = preferences.getBool("wifi_dhcp", true);
  wifiStaticIp = preferences.getString("wifi_static_ip", "");
  wifiGateway = preferences.getString("wifi_gateway", "");
  wifiSubnet = preferences.getString("wifi_subnet", "");
  wifiDns = preferences.getString("wifi_dns", "");
  const uint8_t storedLogLevels = preferences.getUChar(
    "log_levels",
    (static_cast<uint8_t>(LogLevel::INFO) << 2) |
      static_cast<uint8_t>(LogLevel::INFO));
  const bool logLevelsValid = storedLogLevels <= 0x0F;
  const LogLevel loadedSerialLevel =
    logLevelsValid &&
        (storedLogLevels >> 2) <= static_cast<uint8_t>(LogLevel::DEBUG)
      ? static_cast<LogLevel>(storedLogLevels >> 2)
      : LogLevel::INFO;
  const LogLevel loadedWebLevel =
    logLevelsValid &&
        (storedLogLevels & 0x03) <= static_cast<uint8_t>(LogLevel::DEBUG)
      ? static_cast<LogLevel>(storedLogLevels & 0x03)
      : LogLevel::INFO;
  serialLogLevel.store(loadedSerialLevel);
  webLogLevel.store(loadedWebLevel);

  const String storedHttpUser = preferences.getString("http_user", "");
  http_user = "admin";
  authPassword = preferences.getString("http_pass", "");
  authConfigured = authPassword.length() > 0;
  if (authConfigured && storedHttpUser != http_user) {
    const size_t usernameBytes = preferences.putString("http_user", http_user);
    if (usernameBytes != http_user.length() ||
        preferences.getString("http_user", "") != http_user) {
      addLog("AUTH: failed to normalize stored username to admin", "error");
    } else {
      addLog("AUTH: stored username normalized to admin", "info");
    }
  }
  preferences.remove("ap_password");
  preferences.end();

  if (authConfigured && authPassword.length() > 0)
    addLog("AUTH: plaintext login credential loaded from NVS", "info");
}

void saveUartConfig() {
  preferences.begin("modbus-gw", false);
  preferences.putUInt("baud", uartCfg.baud);
  preferences.putUChar("stop_bits", uartCfg.stop_bits);
  preferences.putUChar("data_bits", uartCfg.data_bits);
  preferences.putChar("parity", uartCfg.parity);
  preferences.end();
}

bool saveGatewaySettings(const UARTConfig& uart, uint16_t tcpPort,
                         uint32_t rtuTimeoutMs, uint32_t tcpTimeoutMs,
                         const String& hostname) {
  if (rtuTimeoutMs == 0 || tcpTimeoutMs == 0) {
    addLog("SETTINGS: RTU and TCP timeouts must be greater than zero", "error");
    return false;
  }
  if (!validMdnsHostname(hostname)) {
    addLog("SETTINGS: hostname must be a valid 1-63 character DNS label",
           "error");
    return false;
  }

  if (!preferences.begin("modbus-gw", false)) {
    addLog("SETTINGS: Preferences.begin failed", "error");
    return false;
  }

  preferences.putUInt("baud", uart.baud);
  preferences.putUChar("stop_bits", uart.stop_bits);
  preferences.putUChar("data_bits", uart.data_bits);
  preferences.putChar("parity", uart.parity);
  preferences.putUShort("tcp_port", tcpPort);
  preferences.putUInt("rtu_timeout_ms", rtuTimeoutMs);
  preferences.putUInt("tcp_timeout_ms", tcpTimeoutMs);
  const size_t hostnameBytes = preferences.putString("hostname", hostname);

  const bool verified = hostnameBytes == hostname.length() &&
    preferences.getString("hostname", "") == hostname &&
    preferences.getUInt("baud", 0) == uart.baud &&
    preferences.getUChar("stop_bits", 0) == uart.stop_bits &&
    preferences.getUChar("data_bits", 0) == uart.data_bits &&
    preferences.getChar("parity", '\0') == uart.parity &&
    preferences.getUShort("tcp_port", 0) == tcpPort &&
    preferences.getUInt("rtu_timeout_ms", 0) == rtuTimeoutMs &&
    preferences.getUInt("tcp_timeout_ms", 0) == tcpTimeoutMs;
  preferences.end();

  if (!verified) {
    addLog("SETTINGS: saved values did not verify in NVS", "error");
    return false;
  }

  uartCfg = uart;
  modbusTcpPort = tcpPort;
  modbusRtuTimeoutMs = rtuTimeoutMs;
  modbusTcpTimeoutMs = tcpTimeoutMs;
  gatewayHostname = hostname;
  addLog("SETTINGS: gateway hostname, serial, TCP port, and timeout settings saved",
         "success");
  return true;
}

bool saveWifiConfig(const String& ssid, const String& password, bool useDhcp,
                    const String& ip, const String& gateway,
                    const String& subnet, const String& dns) {
  if (!preferences.begin("modbus-gw", false)) {
    addLog("WIFI_CONFIG: Preferences.begin failed", "error");
    return false;
  }
  const size_t ssid_bytes = preferences.putString("wifi_ssid", ssid);
  const size_t password_bytes = preferences.putString("wifi_password", password);
  const size_t ip_bytes = preferences.putString("wifi_static_ip", ip);
  const size_t gateway_bytes = preferences.putString("wifi_gateway", gateway);
  const size_t subnet_bytes = preferences.putString("wifi_subnet", subnet);
  const size_t dns_bytes = preferences.putString("wifi_dns", dns);
  const size_t dhcp_bytes = preferences.putBool("wifi_dhcp", useDhcp);
  const bool writes_succeeded = ssid_bytes == ssid.length() &&
                                password_bytes == password.length() &&
                                ip_bytes == ip.length() &&
                                gateway_bytes == gateway.length() &&
                                subnet_bytes == subnet.length() &&
                                dns_bytes == dns.length() && dhcp_bytes == 1;
  const String stored_ssid = preferences.getString("wifi_ssid", "");
  const String stored_password = preferences.getString("wifi_password", "");
  const bool values_verified =
    stored_ssid == ssid && stored_password == password &&
    preferences.getString("wifi_static_ip", "") == ip &&
    preferences.getString("wifi_gateway", "") == gateway &&
    preferences.getString("wifi_subnet", "") == subnet &&
    preferences.getString("wifi_dns", "") == dns &&
    preferences.getBool("wifi_dhcp", !useDhcp) == useDhcp;
  preferences.end();

  if (!writes_succeeded || !values_verified) {
    addLog("WIFI_CONFIG: NVS verification failed (SSID bytes=" +
           String(ssid_bytes) + "/" + String(ssid.length()) +
           ", password bytes=" + String(password_bytes) + "/" +
           String(password.length()) + ", values match=" +
           String(values_verified ? "yes" : "no") + ")", "error");
    return false;
  }

  addLog("WIFI_CONFIG: SSID and password verified in NVS", "success");
  wifi_ssid = ssid;
  wifi_password = password;
  wifi_configured = true;
  wifiUseDhcp = useDhcp;
  wifiStaticIp = ip;
  wifiGateway = gateway;
  wifiSubnet = subnet;
  wifiDns = dns;
  return true;
}

void clearWifiConfig() {
  preferences.begin("modbus-gw", false);
  preferences.remove("wifi_ssid");
  preferences.remove("wifi_password");
  preferences.remove("wifi_dhcp");
  preferences.remove("wifi_static_ip");
  preferences.remove("wifi_gateway");
  preferences.remove("wifi_subnet");
  preferences.remove("wifi_dns");
  preferences.end();
  wifi_ssid = "";
  wifi_password = "";
  wifi_configured = false;
  wifiUseDhcp = true;
  wifiStaticIp = "";
  wifiGateway = "";
  wifiSubnet = "";
  wifiDns = "";
  serialLogLevel.store(LogLevel::INFO);
  webLogLevel.store(LogLevel::INFO);
}

bool httpCredentialsConfigured() {
  return authConfigured;
}

bool verifyHttpPassword(const String& password) {
  return authConfigured && password.length() <= 64 &&
         password == authPassword;
}

bool saveHttpAuth(const String& user, const String& pass) {
  if (user != "admin" || pass.length() == 0 || pass.length() > 64) {
    return false;
  }
  Preferences authPreferences;
  if (!authPreferences.begin("modbus-gw", false)) {
    addLog("AUTH: could not open non-volatile storage", "error");
    return false;
  }
  const bool saved = persistHttpAuth(authPreferences, user, pass);
  authPreferences.end();
  if (!saved) {
    addLog("AUTH: credential storage verification failed", "error");
    return false;
  }
  http_user = user;
  authPassword = pass;
  authConfigured = true;
  addLog("AUTH: username and plaintext password saved to NVS", "success");
  return true;
}

void saveStats() {
  preferences.begin("modbus-gw", false);
  preferences.putULong("req_count", reqCount);
  preferences.putULong("err_count", errCount);
  preferences.end();
}

bool factoryReset() {
  if (!preferences.begin("modbus-gw", false)) {
    addLog("Factory reset failed: could not open preferences", "error");
    return false;
  }
  const bool cleared = preferences.clear();
  preferences.end();
  if (!cleared) {
    addLog("Factory reset failed: could not clear preferences", "error");
    return false;
  }

  uartCfg.baud = MODBUS_DEFAULT_BAUD;
  uartCfg.stop_bits = MODBUS_DEFAULT_STOPBITS;
  uartCfg.data_bits = MODBUS_DEFAULT_DATABITS;
  uartCfg.parity = MODBUS_DEFAULT_PARITY;
  modbusTcpPort = MODBUS_DEFAULT_TCP_PORT;
  modbusRtuTimeoutMs = MODBUS_DEFAULT_RTU_TIMEOUT_MS;
  modbusTcpTimeoutMs = MODBUS_DEFAULT_TCP_TIMEOUT_MS;
  wifi_ssid = "";
  wifi_password = "";
  wifi_configured = false;
  wifiUseDhcp = true;
  wifiStaticIp = "";
  wifiGateway = "";
  wifiSubnet = "";
  wifiDns = "";
  gatewayHostname = DEFAULT_GATEWAY_HOSTNAME;
  http_user = "";
  authPassword = "";
  authConfigured = false;
  reqCount = 0;
  errCount = 0;
  addLog("Factory reset complete: defaults restored; reboot required", "warning");
  return true;
}

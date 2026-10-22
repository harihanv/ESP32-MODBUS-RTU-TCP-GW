#include "web_server.h"
#include "ota_handler.h"
#include "modbus_tcp.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

static AsyncWebServer server(WEB_PORT);
static std::atomic<bool> restartScheduled(false);
static std::atomic<uint32_t> restartAt(0);
static constexpr size_t WIFI_CONFIG_BODY_LIMIT = 512;
static constexpr size_t REQUEST_BODY_LIMIT = 256;
static constexpr uint32_t SESSION_TIMEOUT_MS = 30UL * 60UL * 1000UL;
static constexpr uint32_t DIAGNOSTIC_OPERATION_TOKEN_TTL_MS = 120000;
static String activeSessionToken;
static uint32_t sessionExpiresAt = 0;
static std::atomic<bool> otaUploadBusy(false);
static std::atomic<bool> authSetupActive(false);
static std::atomic<uint8_t> wifiScanState(0);
static uint32_t wifiScanStartedAt = 0;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 25000;

struct OtaUploadState {
  bool ownsLock = false;
  bool started = false;
  bool finished = false;
  int status = 500;
  String error;
};

static void releaseOtaUpload(AsyncWebServerRequest *req,
                             OtaUploadState *state) {
  if (!state) return;
  if (state->started && !state->finished) abortOtaUpdate();
  if (state->ownsLock) otaUploadBusy.store(false);
  req->_tempObject = nullptr;
  delete state;
}

struct WifiConfigRequestBody {
  size_t expected;
  size_t received;
  char data[WIFI_CONFIG_BODY_LIMIT + 1];
};

struct DiagnosticRequestBody {
  size_t expected;
  size_t received;
  char data[769];
};

struct DiagnosticTaskContext {
  uint8_t unitId;
  uint8_t functionCode;
  uint16_t address;
  uint16_t quantity;
};

static bool parseConfiguredAddress(const String& value, IPAddress& address) {
  if (value.length() > 15 || !address.fromString(value)) return false;
  return address[0] != 0 || address[1] != 0 ||
         address[2] != 0 || address[3] != 0;
}

enum class DiagnosticJobState : uint8_t {
  IDLE,
  PENDING,
  COMPLETE
};
static std::atomic<bool> diagnosticTaskActive(false);
static std::atomic<DiagnosticJobState> diagnosticJobState{
  DiagnosticJobState::IDLE
};
static String diagnosticJobToken;
static String diagnosticJobResponse;
static int diagnosticJobHttpStatus = 200;
static uint32_t diagnosticJobExpiresAt = 0;

static void processDiagnosticRequest(void *parameter) {
  std::unique_ptr<DiagnosticTaskContext> context(
    static_cast<DiagnosticTaskContext *>(parameter));
  if (!beginRtuDiagnostic()) {
    diagnosticJobHttpStatus = 503;
    diagnosticJobResponse =
      "{\"success\":false,\"error\":\"Unable to acquire RTU bus for diagnostics\"}";
    diagnosticJobState.store(DiagnosticJobState::COMPLETE);
    diagnosticTaskActive.store(false);
    context.reset();
    vTaskDelete(nullptr);
    return;
  }

  uint16_t registerValues[125];
  ModbusRtuExchangeTrace trace;
  const bool success = rtu_read_registers(
    context->unitId, context->functionCode, context->address,
    context->quantity, registerValues, &trace);
  endRtuDiagnostic();

  DynamicJsonDocument result(4096);
  result["success"] = success;
  result["unit_id"] = context->unitId;
  result["function_code"] = context->functionCode;
  result["address"] = context->address;
  result["quantity"] = context->quantity;
  result["error_code"] = trace.error_code;
  if (trace.request_frame_len > 0) {
    result["request_rtu_frame"] =
      bytesToHex(trace.request_frame, trace.request_frame_len);
    result["request_pdu"] =
      bytesToHex(trace.request_frame + 1, trace.request_frame_len - 3);
  }
  if (trace.response_frame_len > 0) {
    result["response_rtu_frame"] =
      bytesToHex(trace.response_frame, trace.response_frame_len);
    if (trace.response_frame_len > 3) {
      result["response_pdu"] =
        bytesToHex(trace.response_frame + 1, trace.response_frame_len - 3);
    }
  }
  if (success) {
    JsonArray values = result.createNestedArray("registers");
    for (uint16_t i = 0; i < context->quantity; ++i) {
      values.add(registerValues[i]);
    }
  } else if (trace.error_code == Modbus::TIMEOUT ||
             trace.error_code == Modbus::GATEWAY_TARGET_NO_RESP) {
    result["error"] = "RTU timeout: no response received within 1 second";
  } else if (trace.error_code == Modbus::REQUEST_QUEUE_FULL ||
             trace.error_code == Modbus::EMPTY_MESSAGE ||
             trace.request_frame_len == 0) {
    result["error"] = "RTU busy or transmit failure; request was not sent";
  } else if (trace.response_frame_len == 0) {
    result["error"] = "RTU exchange failed with eModbus error 0x" +
      String(trace.error_code, HEX);
  } else if (trace.response_frame_len >= 5 &&
             trace.response_frame[1] ==
               static_cast<uint8_t>(context->functionCode | 0x80)) {
    result["error"] = "RTU exception response code " +
      String(trace.response_frame[2], HEX);
  } else {
    result["error"] = "RTU response was invalid; inspect Logs & System";
  }

  diagnosticJobHttpStatus = 200;
  serializeJson(result, diagnosticJobResponse);
  diagnosticJobState.store(DiagnosticJobState::COMPLETE);
  diagnosticTaskActive.store(false);
  context.reset();
  vTaskDelete(nullptr);
}

static bool hasValidSession(AsyncWebServerRequest *req) {
  const String cookie = req->header("Cookie");
  const String cookieName = "gw_session=";
  int start = cookie.indexOf(cookieName);
  if (start >= 0) {
    start += cookieName.length();
    int end = cookie.indexOf(';', start);
    if (end < 0) end = cookie.length();
    const String token = cookie.substring(start, end);
    if (activeSessionToken.length() > 0 && token == activeSessionToken &&
        static_cast<int32_t>(sessionExpiresAt - millis()) > 0) {
      sessionExpiresAt = millis() + SESSION_TIMEOUT_MS;
      return true;
    }
  }
  return false;
}

static inline bool checkAuth(AsyncWebServerRequest *req) {
  if (hasValidSession(req)) return true;
  req->send(401, "application/json", "{\"error\":\"Login required\"}");
  return false;
}

static String createSessionToken() {
  char token[33];
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return String(token);
}

static void sendSessionCreated(AsyncWebServerRequest *req) {
  activeSessionToken = createSessionToken();
  sessionExpiresAt = millis() + SESSION_TIMEOUT_MS;
  AsyncWebServerResponse *response = req->beginResponse(
    200, "application/json", "{\"success\":true}");
  response->addHeader("Set-Cookie", "gw_session=" + activeSessionToken +
    "; Path=/; HttpOnly; SameSite=Strict");
  response->addHeader("Cache-Control", "no-store");
  req->send(response);
}

static String createOperationToken() {
  char token[33];
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return String(token);
}

static void sendLoginPage(AsyncWebServerRequest *req) {
  AsyncWebServerResponse *response = req->beginResponse(200, "text/html", LOGIN_PAGE);
  response->addHeader("Cache-Control", "no-store");
  req->send(response);
}

void handleWebServerRestart() {
  if (restartScheduled.load() &&
      static_cast<int32_t>(millis() - restartAt.load()) >= 0) {
    ESP.restart();
  }
}

bool wifiNetworkScanInProgress() {
  return wifiScanState.load() != 0;
}

void initWebServer() {
  server.on("/api/auth/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    StaticJsonDocument<64> doc;
    doc["configured"] = httpCredentialsConfigured();
    String output;
    serializeJson(doc, output);
    AsyncWebServerResponse *response =
      req->beginResponse(200, "application/json", output);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
  });

  server.on("/login", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (hasValidSession(req)) {
      req->redirect("/");
      return;
    }
    sendLoginPage(req);
  });

  server.on("/api/auth/setup", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    WifiConfigRequestBody *body =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (!body || body->received == 0) {
      req->send(400, "application/json", "{\"error\":\"Account setup body is missing\"}");
      return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body->data, body->received) ||
        !doc["password"].is<const char*>()) {
      req->send(400, "application/json",
        "{\"error\":\"Password is required\"}");
      return;
    }
    const String password = doc["password"].as<String>();
    if (password.length() == 0 || password.length() > 64) {
      req->send(400, "application/json",
        "{\"error\":\"Password must be 1-64 characters\"}");
      return;
    }
    bool setupExpected = false;
    if (!authSetupActive.compare_exchange_strong(setupExpected, true)) {
      req->send(409, "application/json",
        "{\"error\":\"Account setup is already in progress\"}");
      return;
    }
    if (httpCredentialsConfigured()) {
      authSetupActive.store(false);
      req->send(409, "application/json",
        "{\"error\":\"An account is already configured\"}");
      return;
    }
    if (!saveHttpAuth("admin", password)) {
      authSetupActive.store(false);
      req->send(500, "application/json",
        "{\"error\":\"Could not save the administrator account. Check device logs and retry\"}");
      return;
    }
    authSetupActive.store(false);
    addLog("AUTH: initial administrator account created", "success");
    sendSessionCreated(req);
  }, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    if (req->getResponse()) return;
    if (total == 0 || total > REQUEST_BODY_LIMIT) {
      req->send(413, "application/json", "{\"error\":\"Account setup body exceeds 256 bytes\"}");
      return;
    }
    WifiConfigRequestBody *body =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (index == 0) {
      if (body) {
        req->send(400, "application/json", "{\"error\":\"Unexpected duplicate account setup body\"}");
        return;
      }
      body = static_cast<WifiConfigRequestBody *>(
        calloc(1, sizeof(WifiConfigRequestBody)));
      if (!body) {
        req->send(500, "application/json", "{\"error\":\"Unable to process account setup\"}");
        return;
      }
      body->expected = total;
      req->_tempObject = body;
    }
    if (!body || index > total || body->expected != total ||
        body->received != index || len > total - index) {
      req->send(400, "application/json", "{\"error\":\"Incomplete account setup request\"}");
      return;
    }
    memcpy(body->data + index, data, len);
    body->received += len;
    body->data[body->received] = '\0';
  });

  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    WifiConfigRequestBody *body =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (!body || body->received == 0) {
      req->send(400, "application/json", "{\"error\":\"Login request body is missing\"}");
      return;
    }

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body->data, body->received) ||
        !doc["password"].is<const char*>()) {
      req->send(400, "application/json", "{\"error\":\"Password is required\"}");
      return;
    }

    if (!httpCredentialsConfigured()) {
      req->send(409, "application/json",
        "{\"error\":\"No administrator account is configured\"}");
      return;
    }
    const String password = doc["password"].as<String>();
    if (password.length() == 0 || password.length() > 64) {
      req->send(400, "application/json",
        "{\"error\":\"Password must be 1-64 characters\"}");
      return;
    }
    const unsigned long verificationStartedAt = millis();
    const bool valid = verifyHttpPassword(password);
    char verificationMessage[96];
    snprintf(verificationMessage, sizeof(verificationMessage),
             "AUTH: password verification %s in %lu ms",
             valid ? "succeeded" : "failed",
             millis() - verificationStartedAt);
    addLog(verificationMessage, valid ? "info" : "warning");
    if (!valid) {
      addLog("AUTH: rejected invalid login", "warning");
      req->send(401, "application/json",
        "{\"success\":false,\"error\":\"Invalid username or password\"}");
      return;
    }
    sendSessionCreated(req);
    addLog("AUTH: login session issued", "success");
  }, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    if (req->getResponse()) return;
    if (total == 0 || total > REQUEST_BODY_LIMIT) {
      req->send(413, "application/json", "{\"error\":\"Login request body must be 1-256 bytes\"}");
      return;
    }
    WifiConfigRequestBody *body =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (index == 0) {
      if (body) {
        req->send(400, "application/json", "{\"error\":\"Unexpected duplicate login body\"}");
        return;
      }
      body = static_cast<WifiConfigRequestBody *>(
        calloc(1, sizeof(WifiConfigRequestBody)));
      if (!body) {
        req->send(500, "application/json", "{\"error\":\"Unable to process login request\"}");
        return;
      }
      body->expected = total;
      req->_tempObject = body;
    }
    if (!body || index > total || body->expected != total ||
        body->received != index || len > total - index) {
      req->send(400, "application/json", "{\"error\":\"Incomplete login request body\"}");
      return;
    }
    memcpy(body->data + index, data, len);
    body->received += len;
    body->data[body->received] = '\0';
  });

  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest *req) {
    activeSessionToken = "";
    sessionExpiresAt = 0;
    AsyncWebServerResponse *response = req->beginResponse(
      200, "application/json", "{\"success\":true}");
    response->addHeader("Set-Cookie",
      "gw_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    req->send(response);
    addLog("AUTH: user logged out", "info");
  });

  server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest *req) {
    StaticJsonDocument<256> doc;
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    doc["ready"] = true;
    doc["wifi_connected"] = wifiConnected;
    doc["setup_mode"] = !wifiConnected;
    if (wifiConnected) {
      doc["ip"] = WiFi.localIP().toString();
      doc["dashboard_url"] = "http://" + WiFi.localIP().toString() + "/";
    }
    String output;
    serializeJson(doc, output);
    AsyncWebServerResponse *response =
      req->beginResponse(200, "application/json", output);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!hasValidSession(req)) {
      sendLoginPage(req);
      return;
    }
    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    const bool inSetupMode = !wifiConnected;

    if (inSetupMode) {
      AsyncWebServerResponse *response =
        req->beginResponse(200, "text/html", WIFI_SETUP_PAGE);
      response->addHeader("Cache-Control", "no-store");
      req->send(response);
      return;
    }

    AsyncWebServerResponse *response =
      req->beginResponse(200, "text/html", HTML_PAGE);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    StaticJsonDocument<512> doc;
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["uptime_s"] = (millis() - startMs) / 1000;
    doc["request_count"] = reqCount + errCount;
    doc["response_count"] = reqCount;
    doc["error_count"] = errCount;
    doc["rtu_tx_count"] = rtuTxCount.load();
    doc["rtu_rx_count"] = rtuRxCount.load();
    doc["tcp_tx_count"] = tcpTxCount.load();
    doc["tcp_rx_count"] = tcpRxCount.load();
    doc["version"] = FIRMWARE_VERSION;
    doc["state"] = modbusPathState();
    doc["rtu_ready"] = rtuReady;
    doc["tcp_bridge_ready"] = modbusReady;
    doc["ip"] = WiFi.status() == WL_CONNECTED ?
                WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["ssid"] = wifi_ssid;
    doc["tcp_port"] = modbusTcpPort;
    doc["rtu_timeout_ms"] = modbusRtuTimeoutMs;
    doc["tcp_timeout_ms"] = modbusTcpTimeoutMs;
    doc["communication_mode"] = modbusActivityState();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/ota/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    StaticJsonDocument<192> doc;
    doc["available"] = otaAvailable();
    doc["maximum_image_bytes"] = otaMaximumImageSize();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/modbus/config-mode", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    if (!req->hasParam("active")) {
      req->send(400, "application/json",
        "{\"error\":\"Configuration mode state is required\"}");
      return;
    }
    const String value = req->getParam("active")->value();
    if (value != "0" && value != "1") {
      req->send(400, "application/json",
        "{\"error\":\"Configuration mode state must be 0 or 1\"}");
      return;
    }
    if (value == "1" && WiFi.status() != WL_CONNECTED) {
      req->send(503, "application/json",
        "{\"error\":\"Configuration mode is unavailable until Wi-Fi is connected\"}");
      return;
    }
    if (!setModbusConfigurationMode(value == "1")) {
      req->send(503, "application/json",
        "{\"error\":\"Unable to change Modbus bridge mode\"}");
      return;
    }
    StaticJsonDocument<96> responseDoc;
    responseDoc["success"] = true;
    responseDoc["state"] = modbusPathState();
    String responseBody;
    serializeJson(responseDoc, responseBody);
    req->send(200, "application/json", responseBody);
  });

  server.on("/api/ota", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    OtaUploadState *state =
      static_cast<OtaUploadState *>(req->_tempObject);
    if (!hasValidSession(req)) {
      if (state) releaseOtaUpload(req, state);
      checkAuth(req);
      return;
    }
    if (!state) {
      req->send(400, "application/json",
                "{\"error\":\"No firmware image was received\"}");
      return;
    }
    if (state->error.length() > 0 || !state->finished) {
      if (state->error.length() == 0) {
        state->error = "Firmware upload was incomplete";
      }
      const int status = state->status;
      StaticJsonDocument<256> doc;
      doc["success"] = false;
      doc["error"] = state->error;
      String out;
      serializeJson(doc, out);
      addLog("OTA: firmware update failed: " + state->error, "error");
      releaseOtaUpload(req, state);
      req->send(status, "application/json", out);
      return;
    }

    releaseOtaUpload(req, state);
    AsyncWebServerResponse *response = req->beginResponse(
      200, "application/json", "{\"success\":true,\"rebooting\":true}");
    response->addHeader("Connection", "close");
    req->send(response);
    addLog("OTA: firmware image verified; rebooting into updated firmware",
           "success");
    restartAt.store(millis() + 1500);
    restartScheduled.store(true);
  }, [](AsyncWebServerRequest *req, const String &filename, size_t index,
        uint8_t *data, size_t len, bool final) {
    OtaUploadState *state =
      static_cast<OtaUploadState *>(req->_tempObject);
    if (index == 0) {
      if (state) {
        state->error = "Unexpected duplicate firmware upload";
        state->status = 400;
        return;
      }
      state = new (std::nothrow) OtaUploadState();
      if (!state) {
        req->send(500, "application/json",
                  "{\"error\":\"Unable to allocate firmware upload state\"}");
        return;
      }
      req->_tempObject = state;
      req->onDisconnect([req]() {
        OtaUploadState *disconnectedState =
          static_cast<OtaUploadState *>(req->_tempObject);
        if (!disconnectedState) return;
        addLog("OTA: upload connection closed before completion", "warning");
        releaseOtaUpload(req, disconnectedState);
      });
      bool expected = false;
      if (!otaUploadBusy.compare_exchange_strong(expected, true)) {
        state->status = 409;
        state->error = "Another firmware update is already in progress";
        return;
      }
      state->ownsLock = true;
      if (!hasValidSession(req)) {
        state->status = 401;
        state->error = "Login required";
        return;
      }
      if (!otaAvailable()) {
        state->status = 503;
        state->error = "No inactive firmware partition is available";
        return;
      }
      if (!beginOtaUpdate()) {
        state->error = otaError();
        if (state->error.length() == 0) {
          state->error = "Unable to start firmware update";
        }
        return;
      }
      state->started = true;
      addLog("OTA: receiving firmware image '" + filename + "'", "info");
    }

    if (!state) return;
    if (state->error.length() > 0) return;
    if (!hasValidSession(req)) {
      state->status = 401;
      state->error = "Login required";
      abortOtaUpdate();
      state->started = false;
      return;
    }

    if (len > 0 && writeOtaUpdate(data, len) != len) {
      state->error = otaError();
      if (state->error.length() == 0) {
        state->error = "Unable to write firmware image";
      }
      abortOtaUpdate();
      state->started = false;
      return;
    }
    if (final) {
      if (!finishOtaUpdate()) {
        state->error = otaError();
        if (state->error.length() == 0) {
          state->error = "Firmware image verification failed";
        }
        abortOtaUpdate();
        state->started = false;
        return;
      }
      state->finished = true;
    }
  });

  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    DynamicJsonDocument doc(MAX_LOG_ENTRIES * 320 + 256);
    JsonArray arr = doc.createNestedArray("logs");
    std::deque<String> logs;
    if (!copyLogs(logs)) {
      req->send(500, "application/json",
        "{\"error\":\"Unable to read the system log buffer\"}");
      return;
    }
    for (const auto& log : logs) arr.add(log);
    if (doc.overflowed()) {
      addLog("LOG: system log response exceeded its JSON buffer", "error");
      req->send(500, "application/json",
        "{\"error\":\"Unable to serialize the system log buffer\"}");
      return;
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    if (!clearLogs()) {
      req->send(500, "application/json",
        "{\"error\":\"Unable to clear the system log buffer\"}");
      return;
    }
    req->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/logging", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    if (!req->hasParam("serial") || !req->hasParam("web")) {
      req->send(400, "application/json",
        "{\"error\":\"Serial and web log levels from 0 to 3 are required\"}");
      return;
    }
    const String serialValue = req->getParam("serial")->value();
    const String webValue = req->getParam("web")->value();
    if (serialValue.length() != 1 || serialValue[0] < '0' ||
        serialValue[0] > '3' || webValue.length() != 1 ||
        webValue[0] < '0' || webValue[0] > '3') {
      req->send(400, "application/json",
        "{\"error\":\"Log levels must be 0 (error), 1 (warning), 2 (info), or 3 (debug)\"}");
      return;
    }
    const LogLevel serialLevel =
      static_cast<LogLevel>(serialValue[0] - '0');
    const LogLevel webLevel = static_cast<LogLevel>(webValue[0] - '0');
    if (!saveLogLevels(serialLevel, webLevel)) {
      req->send(500, "application/json",
        "{\"error\":\"Failed to save the logging levels to non-volatile storage\"}");
      return;
    }
    req->send(200, "application/json", "{\"success\":true,\"saved\":true}");
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    StaticJsonDocument<512> doc;
    doc["baud"] = uartCfg.baud;
    doc["data_bits"] = uartCfg.data_bits;
    doc["parity"] = String(uartCfg.parity);
    doc["stop_bits"] = uartCfg.stop_bits;
    doc["tcp_port"] = modbusTcpPort;
    doc["rtu_timeout_ms"] = modbusRtuTimeoutMs;
    doc["tcp_timeout_ms"] = modbusTcpTimeoutMs;
    doc["ssid"] = wifi_ssid;
    doc["wifi_dhcp"] = wifiUseDhcp;
    doc["wifi_static_ip"] = wifiStaticIp;
    doc["wifi_gateway"] = wifiGateway;
    doc["wifi_subnet"] = wifiSubnet;
    doc["wifi_dns"] = wifiDns;
    doc["serial_log_level"] =
      static_cast<uint8_t>(configuredSerialLogLevel());
    doc["web_log_level"] = static_cast<uint8_t>(configuredWebLogLevel());
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    if (!checkAuth(req)) return;

    WifiConfigRequestBody *requestBody =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (!requestBody || requestBody->received == 0) {
      req->send(400, "application/json", "{\"error\":\"Settings request body is missing\"}");
      return;
    }

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, requestBody->data, requestBody->received)) {
      req->send(400, "application/json", "{\"error\":\"Invalid settings JSON\"}");
      return;
    }
    if (!doc["baud"].is<uint32_t>() || !doc["data_bits"].is<uint8_t>() ||
        !doc["parity"].is<const char*>() || !doc["stop_bits"].is<uint8_t>() ||
        !doc["tcp_port"].is<uint16_t>() || !doc["rtu_timeout_ms"].is<uint32_t>() ||
        !doc["tcp_timeout_ms"].is<uint32_t>()) {
      req->send(400, "application/json", "{\"error\":\"Serial, TCP, and timeout settings are required\"}");
      return;
    }

    UARTConfig candidate;
    candidate.baud = doc["baud"].as<uint32_t>();
    candidate.data_bits = doc["data_bits"].as<uint8_t>();
    candidate.parity = doc["parity"].as<const char*>()[0];
    candidate.stop_bits = doc["stop_bits"].as<uint8_t>();
    const uint16_t tcpPort = doc["tcp_port"].as<uint16_t>();
    const uint32_t rtuTimeoutMs = doc["rtu_timeout_ms"].as<uint32_t>();
    const uint32_t tcpTimeoutMs = doc["tcp_timeout_ms"].as<uint32_t>();

    if (candidate.baud < 300 || candidate.baud > 115200 ||
        (candidate.data_bits != 7 && candidate.data_bits != 8) ||
        (candidate.parity != 'N' && candidate.parity != 'E' && candidate.parity != 'O') ||
        (candidate.stop_bits != 1 && candidate.stop_bits != 2) ||
        tcpPort == 0 || rtuTimeoutMs == 0 || tcpTimeoutMs == 0) {
      req->send(400, "application/json", "{\"error\":\"Invalid serial settings, TCP port, or timeout values\"}");
      return;
    }

    if (!saveGatewaySettings(candidate, tcpPort, rtuTimeoutMs, tcpTimeoutMs)) {
      req->send(500, "application/json", "{\"error\":\"Failed to save settings to non-volatile storage\"}");
      return;
    }

    req->send(200, "application/json", "{\"success\":true,\"saved\":true,\"rebooting\":true}");
    restartAt.store(millis() + 1500);
    restartScheduled.store(true);
  }, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    if (req->getResponse()) return;
    if (total == 0 || total > REQUEST_BODY_LIMIT) {
      req->send(413, "application/json", "{\"error\":\"Settings body must be 1-256 bytes\"}");
      return;
    }

    WifiConfigRequestBody *requestBody =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (index == 0) {
      if (requestBody) {
        req->send(400, "application/json", "{\"error\":\"Unexpected duplicate settings body\"}");
        return;
      }
      requestBody = static_cast<WifiConfigRequestBody *>(
        calloc(1, sizeof(WifiConfigRequestBody)));
      if (!requestBody) {
        req->send(500, "application/json", "{\"error\":\"Unable to process settings body\"}");
        return;
      }
      requestBody->expected = total;
      req->_tempObject = requestBody;
    }
    if (!requestBody || index > total || requestBody->expected != total ||
        requestBody->received != index || len > total - index) {
      req->send(400, "application/json", "{\"error\":\"Incomplete settings body\"}");
      return;
    }
    memcpy(requestBody->data + index, data, len);
    requestBody->received += len;
    requestBody->data[requestBody->received] = '\0';
  });

  server.on("/api/diagnostics/rtu/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    if (!req->hasParam("id")) {
      req->send(400, "application/json",
        "{\"error\":\"Diagnostic ID is required\"}");
      return;
    }
    const String suppliedToken = req->getParam("id")->value();
    if (diagnosticJobToken.isEmpty() ||
        suppliedToken != diagnosticJobToken) {
      req->send(404, "application/json",
        "{\"error\":\"Diagnostic operation was not found\"}");
      return;
    }
    if (static_cast<int32_t>(millis() - diagnosticJobExpiresAt) >= 0) {
      diagnosticJobToken = "";
      diagnosticJobResponse = "";
      diagnosticJobState.store(DiagnosticJobState::IDLE);
      req->send(410, "application/json",
        "{\"error\":\"Diagnostic result expired\"}");
      return;
    }

    const DiagnosticJobState state = diagnosticJobState.load();
    if (state == DiagnosticJobState::PENDING) {
      req->send(200, "application/json", "{\"pending\":true}");
      return;
    }
    if (state != DiagnosticJobState::COMPLETE) {
      req->send(404, "application/json",
        "{\"error\":\"Diagnostic result is no longer available\"}");
      return;
    }

    AsyncWebServerResponse *response = req->beginResponse(
      diagnosticJobHttpStatus, "application/json", diagnosticJobResponse);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
  });

  server.on("/api/diagnostics/rtu", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    if (!checkAuth(req)) return;
    if (!modbusReady) {
      req->send(503, "application/json", "{\"error\":\"RTU is unavailable until Wi-Fi is connected\"}");
      return;
    }

    DiagnosticRequestBody *body =
      static_cast<DiagnosticRequestBody *>(req->_tempObject);
    if (!body || body->received == 0) {
      req->send(400, "application/json", "{\"error\":\"Diagnostic request body is missing\"}");
      return;
    }

    StaticJsonDocument<768> doc;
    if (deserializeJson(doc, body->data, body->received) ||
        !doc["unit_id"].is<uint16_t>() ||
        !doc["function_code"].is<uint8_t>() ||
        !doc["address"].is<uint16_t>() ||
        !doc["quantity"].is<uint16_t>()) {
      req->send(400, "application/json",
        "{\"error\":\"unit_id, function_code, address and quantity are required\"}");
      return;
    }

    const uint16_t unitId = doc["unit_id"].as<uint16_t>();
    const uint8_t functionCode = doc["function_code"].as<uint8_t>();
    const uint16_t address = doc["address"].as<uint16_t>();
    const uint16_t quantity = doc["quantity"].as<uint16_t>();
    if (unitId < 1 || unitId > 247 ||
        (functionCode != MODBUS_FC_READ_HOLDING_REGISTERS &&
         functionCode != MODBUS_FC_READ_INPUT_REGISTERS) ||
        quantity < 1 || quantity > 125 ||
        static_cast<uint32_t>(address) + quantity > 65536UL) {
      req->send(400, "application/json",
        "{\"error\":\"Use unit ID 1-247, function 03 or 04, valid address and quantity 1-125\"}");
      return;
    }

    bool taskExpected = false;
    if (!diagnosticTaskActive.compare_exchange_strong(taskExpected, true)) {
      req->send(409, "application/json",
        "{\"error\":\"An RTU diagnostic is already in progress\"}");
      return;
    }

    DiagnosticTaskContext *context =
      new (std::nothrow) DiagnosticTaskContext{
        static_cast<uint8_t>(unitId),
        functionCode,
        address,
        quantity
      };
    if (!context) {
      diagnosticTaskActive.store(false);
      req->send(503, "application/json",
        "{\"error\":\"Unable to start RTU diagnostic task\"}");
      return;
    }

    diagnosticJobToken = createOperationToken();
    diagnosticJobExpiresAt = millis() + DIAGNOSTIC_OPERATION_TOKEN_TTL_MS;
    diagnosticJobResponse = "";
    diagnosticJobHttpStatus = 200;
    diagnosticJobState.store(DiagnosticJobState::PENDING);
    const BaseType_t taskStarted = xTaskCreatePinnedToCore(
      processDiagnosticRequest, "rtu_diagnostic", 8192, context, 1, nullptr, 1);
    if (taskStarted != pdPASS) {
      delete context;
      diagnosticTaskActive.store(false);
      diagnosticJobState.store(DiagnosticJobState::IDLE);
      diagnosticJobToken = "";
      req->send(503, "application/json",
        "{\"error\":\"Unable to start RTU diagnostic task\"}");
      return;
    }

    StaticJsonDocument<96> responseDoc;
    responseDoc["success"] = true;
    responseDoc["diagnostic_id"] = diagnosticJobToken;
    String responseBody;
    serializeJson(responseDoc, responseBody);
    AsyncWebServerResponse *response =
      req->beginResponse(202, "application/json", responseBody);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
  }, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    if (req->getResponse()) return;
    if (total == 0 || total > 768) {
      req->send(413, "application/json", "{\"error\":\"Diagnostic request must be 1-768 bytes\"}");
      return;
    }

    DiagnosticRequestBody *body =
      static_cast<DiagnosticRequestBody *>(req->_tempObject);
    if (index == 0) {
      if (body) {
        req->send(400, "application/json", "{\"error\":\"Unexpected duplicate diagnostic body\"}");
        return;
      }
      body = static_cast<DiagnosticRequestBody *>(
        calloc(1, sizeof(DiagnosticRequestBody)));
      if (!body) {
        req->send(500, "application/json", "{\"error\":\"Unable to process diagnostic request\"}");
        return;
      }
      body->expected = total;
      req->_tempObject = body;
    }
    if (!body || index > total || body->expected != total ||
        body->received != index || len > total - index) {
      req->send(400, "application/json", "{\"error\":\"Incomplete diagnostic request body\"}");
      return;
    }
    memcpy(body->data + index, data, len);
    body->received += len;
    body->data[body->received] = '\0';
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (WiFi.status() == WL_CONNECTED && !checkAuth(req)) return;
    addLog("SYSTEM: reboot requested from web UI", "warning");
    req->send(200, "application/json", "{\"success\":true,\"rebooting\":true}");
    restartAt.store(millis() + 1500);
    restartScheduled.store(true);
  });

  server.on("/api/modbus/read", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!checkAuth(req)) return;
      if (!modbusReady) {
        req->send(503, "application/json", "{\"error\":\"Modbus gateway is unavailable during Wi-Fi setup\"}");
        return;
      }
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len) ||
          !doc["slave_id"].is<uint8_t>() ||
          !doc["address"].is<uint16_t>() ||
          !doc["quantity"].is<uint16_t>()) {
        req->send(400, "application/json", "{\"error\":\"Invalid register-read request\"}");
        return;
      }
      const uint8_t slave = doc["slave_id"].as<uint8_t>();
      const uint16_t addr = doc["address"].as<uint16_t>();
      const uint16_t qty = doc["quantity"].as<uint16_t>();
      if (slave < 1 || slave > 247 || qty < 1 || qty > 125 ||
          static_cast<uint32_t>(addr) + qty > 65536UL) {
        req->send(400, "application/json", "{\"error\":\"Invalid unit ID, address or quantity\"}");
        return;
      }

      if (!beginRtuDiagnostic()) {
        req->send(503, "application/json",
          "{\"error\":\"Unable to acquire RTU bus for diagnostics\"}");
        return;
      }
      uint16_t res[125];
      bool ok = rtu_read_registers(slave, MODBUS_FC_READ_HOLDING_REGISTERS, addr, qty, res);
      endRtuDiagnostic();

      DynamicJsonDocument resp(1024);
      resp["success"] = ok;
      if (ok) {
        JsonArray vals = resp.createNestedArray("values");
        for (uint16_t i = 0; i < qty; i++) vals.add(res[i]);
      } else {
        resp["error"] = "RTU read failed; check Logs & System";
      }
      String out;
      serializeJson(resp, out);
      req->send(ok ? 200 : 504, "application/json", out);
    }
  );

  server.on("/api/modbus/write", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!checkAuth(req)) return;
      if (!modbusReady) {
        req->send(503, "application/json", "{\"error\":\"Modbus gateway is unavailable during Wi-Fi setup\"}");
        return;
      }
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len) ||
          !doc["slave_id"].is<uint8_t>() ||
          !doc["address"].is<uint16_t>() ||
          !doc["value"].is<uint16_t>()) {
        req->send(400, "application/json", "{\"error\":\"Invalid register-write request\"}");
        return;
      }
      const uint8_t slave = doc["slave_id"].as<uint8_t>();
      const uint16_t address = doc["address"].as<uint16_t>();
      const uint16_t value = doc["value"].as<uint16_t>();
      if (slave < 1 || slave > 247) {
        req->send(400, "application/json", "{\"error\":\"Invalid unit ID\"}");
        return;
      }
      if (!beginRtuDiagnostic()) {
        req->send(503, "application/json",
          "{\"error\":\"Unable to acquire RTU bus for diagnostics\"}");
        return;
      }
      const bool success = rtu_write_single_register(slave, address, value);
      endRtuDiagnostic();
      req->send(success ? 200 : 504, "application/json",
        success ? "{\"success\":true}" : "{\"success\":false,\"error\":\"RTU write failed; check Logs & System\"}");
    }
  );

  server.on("/api/auth/change", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    if (!checkAuth(req)) return;
    WifiConfigRequestBody *body =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (!body || body->received == 0) {
      req->send(400, "application/json",
        "{\"error\":\"Credential update body is missing\"}");
      return;
    }
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, body->data, body->received) ||
        !doc["password"].is<const char*>()) {
      req->send(400, "application/json",
        "{\"error\":\"New password is required\"}");
      return;
    }
    const String password = doc["password"].as<String>();
    if (password.length() == 0 || password.length() > 64) {
      req->send(400, "application/json",
        "{\"error\":\"Password must be 1-64 characters\"}");
      return;
    }
    if (!saveHttpAuth("admin", password)) {
      req->send(500, "application/json",
        "{\"error\":\"Failed to save login credentials to non-volatile storage\"}");
      return;
    }
    activeSessionToken = "";
    sessionExpiresAt = 0;
    AsyncWebServerResponse *response = req->beginResponse(
      200, "application/json", "{\"success\":true}");
    response->addHeader("Set-Cookie",
      "gw_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
    addLog("AUTH: credentials changed; existing session revoked", "success");
  }, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    if (req->getResponse()) return;
    if (total == 0 || total > REQUEST_BODY_LIMIT) {
      req->send(413, "application/json",
        "{\"error\":\"Credential update body exceeds 256 bytes\"}");
      return;
    }
    WifiConfigRequestBody *body =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (index == 0) {
      if (body) {
        req->send(400, "application/json",
          "{\"error\":\"Unexpected duplicate credential update body\"}");
        return;
      }
      body = static_cast<WifiConfigRequestBody *>(
        calloc(1, sizeof(WifiConfigRequestBody)));
      if (!body) {
        req->send(500, "application/json",
          "{\"error\":\"Unable to process credential update\"}");
        return;
      }
      body->expected = total;
      req->_tempObject = body;
    }
    if (!body || index > total || body->expected != total ||
        body->received != index || len > total - index) {
      req->send(400, "application/json",
        "{\"error\":\"Incomplete credential update request\"}");
      return;
    }
    memcpy(body->data + index, data, len);
    body->received += len;
    body->data[body->received] = '\0';
  });

  server.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    clearWifiConfig();
    addLog("WIFI_CONFIG: credentials cleared; restarting into setup mode",
           "warning");
    req->send(200, "application/json",
      "{\"success\":true,\"reset\":true,\"rebooting\":true}");
    restartAt.store(millis() + 1500);
    restartScheduled.store(true);
  });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);

    uint8_t expectedScanState = 0;
    if (wifiScanState.compare_exchange_strong(expectedScanState, 1)) {
      wifiScanStartedAt = millis();
      WiFi.scanDelete();
      if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
      WiFi.enableSTA(true);
      int16_t started = WiFi.scanNetworks(true, false, false, 1000);
      if (started == WIFI_SCAN_FAILED) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.enableSTA(true);
        started = WiFi.scanNetworks(true, false, false, 1000);
      }
      if (started == WIFI_SCAN_FAILED) {
        wifiScanState.store(0);
        addLog("WIFI: unable to start an asynchronous network scan", "error");
        req->send(503, "application/json",
          "{\"error\":\"Unable to start a Wi-Fi network scan\"}");
        return;
      }
      wifiScanState.store(2);
      if (started == WIFI_SCAN_RUNNING) {
        req->send(202, "application/json",
          "{\"scanning\":true,\"networks\":[]}");
        return;
      }
    } else if (expectedScanState == 1) {
      req->send(202, "application/json",
        "{\"scanning\":true,\"networks\":[]}");
      return;
    } else if (expectedScanState == 3) {
      req->send(202, "application/json",
        "{\"scanning\":true,\"networks\":[]}");
      return;
    }

    const int16_t scanResult = WiFi.scanComplete();
    if (scanResult == WIFI_SCAN_RUNNING) {
      if (millis() - wifiScanStartedAt >= WIFI_SCAN_TIMEOUT_MS) {
        expectedScanState = 2;
        if (!wifiScanState.compare_exchange_strong(expectedScanState, 3)) {
          req->send(202, "application/json",
            "{\"scanning\":true,\"networks\":[]}");
          return;
        }
        WiFi.scanDelete();
        wifiScanState.store(0);
        addLog("WIFI: network scan timed out", "warning");
        req->send(504, "application/json",
          "{\"error\":\"Wi-Fi scan timed out. Please try again.\"}");
        return;
      }
      req->send(202, "application/json",
        "{\"scanning\":true,\"networks\":[]}");
      return;
    }
    if (scanResult < 0) {
      expectedScanState = 2;
      if (!wifiScanState.compare_exchange_strong(expectedScanState, 3)) {
        req->send(202, "application/json",
          "{\"scanning\":true,\"networks\":[]}");
        return;
      }
      WiFi.scanDelete();
      wifiScanState.store(0);
      addLog("WIFI: network scan failed with status " +
             String(scanResult), "warning");
      req->send(503, "application/json",
        "{\"error\":\"Wi-Fi scan failed. Please try again.\"}");
      return;
    }

    expectedScanState = 2;
    if (!wifiScanState.compare_exchange_strong(expectedScanState, 3)) {
      req->send(202, "application/json",
        "{\"scanning\":true,\"networks\":[]}");
      return;
    }

    DynamicJsonDocument doc(8192);
    doc["scanning"] = false;
    JsonArray networks = doc.createNestedArray("networks");
    for (int16_t index = 0; index < scanResult; ++index) {
      const String ssid = WiFi.SSID(index);
      if (ssid.isEmpty()) continue;
      JsonObject network;
      bool networkFound = false;
      for (JsonObject existing : networks) {
        if (existing["ssid"].as<String>() == ssid) {
          network = existing;
          networkFound = true;
          break;
        }
      }
      if (!networkFound) network = networks.createNestedObject();
      if (!networkFound ||
          WiFi.RSSI(index) > network["rssi"].as<int>()) {
        network["ssid"] = ssid;
        network["rssi"] = WiFi.RSSI(index);
        network["secure"] = WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
      }
    }
    WiFi.scanDelete();
    wifiScanState.store(0);

    String output;
    if (doc.overflowed()) {
      addLog("WIFI: could not serialize scan results", "error");
      req->send(500, "application/json",
        "{\"error\":\"Could not return Wi-Fi scan results\"}");
      return;
    }
    serializeJson(doc, output);
    req->send(200, "application/json", output);
  });

  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    if (!factoryReset()) {
      req->send(500, "application/json", "{\"error\":\"Factory reset failed; see serial log\"}");
      return;
    }
    req->send(200, "application/json", "{\"success\":true,\"rebooting\":true}");
    restartAt.store(millis() + 1500);
    restartScheduled.store(true);
  });

  server.on("/api/wifi/config", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->getResponse()) return;
    if (!checkAuth(req)) return;

    WifiConfigRequestBody *requestBody =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    String ssid;
    String password;
    bool useDhcp = true;
    String ipText, gatewayText, subnetText, dnsText;

    if (requestBody && requestBody->received > 0) {
      addLog("WIFI_CONFIG: received " + String(requestBody->received) +
             " body bytes", "info");
      StaticJsonDocument<512> doc;
      DeserializationError parseError =
        deserializeJson(doc, requestBody->data, requestBody->received);
      if (parseError) {
        addLog("WIFI_CONFIG: rejected invalid JSON body", "warning");
        req->send(400, "application/json", "{\"error\":\"Invalid JSON body\"}");
        return;
      }
      if (!doc["ssid"].is<const char*>() || !doc["password"].is<const char*>()) {
        req->send(400, "application/json", "{\"error\":\"SSID and password must be strings\"}");
        return;
      }
      ssid = doc["ssid"].as<String>();
      password = doc["password"].as<String>();

      if (doc.containsKey("dhcp")) {
        if (!doc["dhcp"].is<bool>()) {
          req->send(400, "application/json", "{\"error\":\"DHCP setting must be a boolean\"}");
          return;
        }
        useDhcp = doc["dhcp"].as<bool>();
      }
      if (!useDhcp) {
        if (!doc["ip"].is<const char*>() ||
            !doc["gateway"].is<const char*>() ||
            !doc["subnet"].is<const char*>() ||
            !doc["dns"].is<const char*>()) {
          req->send(400, "application/json", "{\"error\":\"Static IP, gateway, subnet mask, and DNS are required\"}");
          return;
        }
        IPAddress ipAddress, gatewayAddress, subnetAddress, dnsAddress;
        ipText = doc["ip"].as<String>();
        gatewayText = doc["gateway"].as<String>();
        subnetText = doc["subnet"].as<String>();
        dnsText = doc["dns"].as<String>();
        if (!parseConfiguredAddress(ipText, ipAddress) ||
            !parseConfiguredAddress(gatewayText, gatewayAddress) ||
            !parseConfiguredAddress(subnetText, subnetAddress) ||
            !parseConfiguredAddress(dnsText, dnsAddress)) {
          req->send(400, "application/json", "{\"error\":\"Static network settings must be valid non-zero IPv4 addresses\"}");
          return;
        }
        ipText = ipAddress.toString();
        gatewayText = gatewayAddress.toString();
        subnetText = subnetAddress.toString();
        dnsText = dnsAddress.toString();
      }
    } else if (req->hasArg("ssid")) {
      ssid = req->arg("ssid");
      password = req->arg("password");
    } else {
      addLog("WIFI_CONFIG: request reached handler without a body", "warning");
      req->send(400, "application/json", "{\"error\":\"No request body received\"}");
      return;
    }

    if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
      req->send(400, "application/json", "{\"error\":\"SSID must be 1-32 characters and password at most 64 characters\"}");
      return;
    }

    if (!saveWifiConfig(ssid, password, useDhcp, ipText, gatewayText,
                        subnetText, dnsText)) {
      addLog("WIFI_CONFIG: failed to save credentials to NVS", "error");
      req->send(500, "application/json", "{\"error\":\"Failed to save Wi-Fi settings to non-volatile storage\"}");
      return;
    }

    addLog("WIFI_CONFIG: credentials saved; scheduling reboot", "success");
    req->send(200, "application/json", "{\"success\":true,\"saved\":true,\"rebooting\":true}");
    restartAt.store(millis() + 1500);
    restartScheduled.store(true);
  }, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    if (req->getResponse()) return;
    if (total == 0 || total > WIFI_CONFIG_BODY_LIMIT) {
      addLog("WIFI_CONFIG: rejected request body size " + String(total), "warning");
      req->send(413, "application/json", "{\"error\":\"Wi-Fi configuration body must be 1-512 bytes\"}");
      return;
    }

    WifiConfigRequestBody *requestBody =
      static_cast<WifiConfigRequestBody *>(req->_tempObject);
    if (index == 0) {
      if (requestBody) {
        req->send(400, "application/json", "{\"error\":\"Unexpected duplicate request body\"}");
        return;
      }
      requestBody = static_cast<WifiConfigRequestBody *>(
        calloc(1, sizeof(WifiConfigRequestBody)));
      if (!requestBody) {
        addLog("WIFI_CONFIG: unable to allocate request body buffer", "error");
        req->send(500, "application/json", "{\"error\":\"Unable to process Wi-Fi configuration body\"}");
        return;
      }
      requestBody->expected = total;
      req->_tempObject = requestBody;
    }

    if (!requestBody || index > total || requestBody->expected != total ||
        requestBody->received != index || len > total - index) {
      addLog("WIFI_CONFIG: malformed or out-of-order body chunk", "warning");
      req->send(400, "application/json", "{\"error\":\"Incomplete Wi-Fi configuration body\"}");
      return;
    }

    memcpy(requestBody->data + index, data, len);
    requestBody->received += len;
    requestBody->data[requestBody->received] = '\0';
  });

  server.onNotFound([](AsyncWebServerRequest *req) {
    const wifi_mode_t mode = WiFi.getMode();
    if (req->method() == HTTP_GET &&
        WiFi.status() != WL_CONNECTED &&
        (mode == WIFI_AP || mode == WIFI_AP_STA)) {
      req->redirect("/");
      return;
    }
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
}

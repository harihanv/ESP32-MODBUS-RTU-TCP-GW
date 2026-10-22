#include "modbus_tcp.h"
#include "ModbusBridgeWiFi.h"
#include "modbus_rtu.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static ModbusBridgeWiFi modbusBridge;
static SemaphoreHandle_t rtuBusMutex = nullptr;
enum class GatewayState : uint8_t {
  INITIALIZATION,
  CONFIGURATION,
  DATA_TRANSFER
};
enum class ModbusActivity : uint8_t {
  IDLE,
  TCP_TRANSACTION,
  RTU_DIAGNOSTIC
};
static std::atomic<GatewayState> gatewayState{GatewayState::INITIALIZATION};
static std::atomic<ModbusActivity> modbusActivity{ModbusActivity::IDLE};
static std::atomic<bool> configurationMode{false};
static std::atomic<bool> configurationTransitioning{false};
static std::atomic<uint32_t> configurationLeaseUntil{0};
static std::atomic<bool> wifiInitialized{false};
static std::atomic<bool> rtuInitialized{false};
static std::atomic<bool> tcpInitialized{false};
static constexpr uint32_t CONFIGURATION_LEASE_MS = 30000;

static const char* gatewayStateName(GatewayState state) {
  switch (state) {
    case GatewayState::INITIALIZATION: return "INITIALIZATION";
    case GatewayState::CONFIGURATION: return "CONFIGURATION";
    case GatewayState::DATA_TRANSFER: return "DATA_TRANSFER";
    default: return "UNKNOWN";
  }
}

static void transitionGatewayState(GatewayState next, const String& reason,
                                   const char* level = "info") {
  const GatewayState previous = gatewayState.exchange(next);
  if (previous != next) {
    addLog("STATE: " + String(gatewayStateName(previous)) + " -> " +
           gatewayStateName(next) + "; " + reason, level);
  }
}

static void updateReadyState(const String& reason) {
  if (configurationTransitioning.load()) {
    transitionGatewayState(GatewayState::INITIALIZATION,
                           "configuration transition in progress");
  } else if (configurationMode.load()) {
    transitionGatewayState(GatewayState::CONFIGURATION, reason, "warning");
  } else if (wifiInitialized.load() && rtuInitialized.load() &&
             tcpInitialized.load()) {
    transitionGatewayState(GatewayState::DATA_TRANSFER, reason, "success");
  } else {
    String detail = reason + "; prerequisites: Wi-Fi ";
    detail += wifiInitialized.load() ? "ready" : "not ready";
    detail += ", RTU ";
    detail += rtuInitialized.load() ? "ready" : "not ready";
    detail += ", TCP bridge ";
    detail += tcpInitialized.load() ? "ready" : "not ready";
    transitionGatewayState(GatewayState::INITIALIZATION, detail);
  }
}

class GuardedModbusRtuClient final : public ModbusClient {
protected:
  Modbus::Error addRequestM(ModbusMessage, uint32_t) override {
    return Modbus::SERVER_DEVICE_BUSY;
  }

  ModbusMessage syncRequestM(ModbusMessage request, uint32_t token) override {
    if (!rtuBusMutex) return busyResponse(request);
    for (;;) {
      if (configurationMode.load() ||
          gatewayState.load() != GatewayState::DATA_TRANSFER ||
          modbusActivity.load() == ModbusActivity::RTU_DIAGNOSTIC) {
        return busyResponse(request);
      }
      if (xSemaphoreTake(rtuBusMutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
      if (configurationMode.load() ||
          gatewayState.load() != GatewayState::DATA_TRANSFER ||
          modbusActivity.load() == ModbusActivity::RTU_DIAGNOSTIC) {
        xSemaphoreGive(rtuBusMutex);
        return busyResponse(request);
      }
      break;
    }

    modbusActivity.store(ModbusActivity::TCP_TRANSACTION);
    ModbusMessage rtuFrame = request;
    RTUutils::addCRC(rtuFrame);
    addLog("RTU TX: " + bytesToHex(rtuFrame.data(), rtuFrame.size()), "debug");
    ++rtuTxCount;
    ModbusMessage response = rtuClient.syncRequest(request, token);
    if (response.size() > 0) ++rtuRxCount;
    modbusActivity.store(ModbusActivity::IDLE);
    xSemaphoreGive(rtuBusMutex);
    return response;
  }

private:
  static ModbusMessage busyResponse(const ModbusMessage& request) {
    ModbusMessage response;
    response.setError(request.getServerID(), request.getFunctionCode(),
                      Modbus::SERVER_DEVICE_BUSY);
    return response;
  }
};

static GuardedModbusRtuClient guardedRtuClient;

bool beginRtuDiagnostic() {
  if (!rtuBusMutex || gatewayState.load() != GatewayState::DATA_TRANSFER ||
      xSemaphoreTake(rtuBusMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (configurationMode.load() ||
      gatewayState.load() != GatewayState::DATA_TRANSFER) {
    xSemaphoreGive(rtuBusMutex);
    return false;
  }
  modbusActivity.store(ModbusActivity::RTU_DIAGNOSTIC);
  addLog("RTU: diagnostic owns the bus; TCP bridge requests receive device-busy",
         "info");
  return true;
}

void endRtuDiagnostic() {
  if (!rtuBusMutex ||
      modbusActivity.load() != ModbusActivity::RTU_DIAGNOSTIC) {
    return;
  }
  modbusActivity.store(ModbusActivity::IDLE);
  addLog("RTU: diagnostic complete; returning to " +
         String(modbusPathState()), "info");
  xSemaphoreGive(rtuBusMutex);
}

bool setModbusConfigurationMode(bool active) {
  if (!rtuBusMutex) return false;
  const bool wasActive = configurationMode.load();
  if (active) {
    configurationTransitioning.store(false);
    configurationMode.store(true);
    transitionGatewayState(GatewayState::CONFIGURATION,
                           "configuration opened; TCP-to-RTU forwarding paused",
                           "warning");
  } else if (wasActive) {
    configurationTransitioning.store(true);
    transitionGatewayState(GatewayState::INITIALIZATION,
                           "configuration closed; validating readiness");
  }
  if (xSemaphoreTake(rtuBusMutex, portMAX_DELAY) != pdTRUE) {
    configurationMode.store(wasActive);
    configurationTransitioning.store(false);
    updateReadyState("configuration transition failed; readiness reevaluated");
    return false;
  }
  configurationMode.store(active);
  configurationTransitioning.store(false);
  configurationLeaseUntil.store(active ? millis() + CONFIGURATION_LEASE_MS : 0);
  updateReadyState(active ? "configuration opened; TCP-to-RTU forwarding paused"
                          : "configuration complete; readiness reevaluated");
  xSemaphoreGive(rtuBusMutex);
  return true;
}

void updateGatewayInitializationState(bool wifiReady, bool rtuIsInitialized,
                                      bool tcpIsInitialized) {
  wifiInitialized.store(wifiReady);
  rtuInitialized.store(rtuIsInitialized);
  tcpInitialized.store(tcpIsInitialized);
  updateReadyState("startup prerequisites evaluated");
  const GatewayState current = gatewayState.load();
  if (!wifiReady || !rtuIsInitialized || !tcpIsInitialized) {
    modbusReady = false;
  } else if (current == GatewayState::DATA_TRANSFER) {
    modbusReady = true;
  }
}

void updateGatewayNetworkState(bool wifiReady) {
  wifiInitialized.store(wifiReady);
  updateReadyState(wifiReady ? "Wi-Fi connected; readiness reevaluated"
                             : "Wi-Fi disconnected");
  const GatewayState current = gatewayState.load();
  if (!wifiReady) {
    modbusReady = false;
  } else if (current == GatewayState::DATA_TRANSFER) {
    modbusReady = true;
  }
}

void maintainModbusPathState() {
  if (!configurationMode.load() ||
      static_cast<int32_t>(millis() - configurationLeaseUntil.load()) < 0 ||
      !rtuBusMutex ||
      xSemaphoreTake(rtuBusMutex, 0) != pdTRUE) {
    return;
  }

  if (configurationMode.load() &&
      static_cast<int32_t>(millis() - configurationLeaseUntil.load()) >= 0) {
    configurationMode.store(false);
    configurationLeaseUntil.store(0);
    transitionGatewayState(GatewayState::INITIALIZATION,
                           "configuration heartbeat expired", "warning");
    updateReadyState("configuration lease expired; readiness reevaluated");
  }
  xSemaphoreGive(rtuBusMutex);
}

const char* modbusPathState() {
  return gatewayStateName(gatewayState.load());
}

const char* modbusActivityState() {
  switch (modbusActivity.load()) {
    case ModbusActivity::TCP_TRANSACTION: return "TCP_TRANSACTION";
    case ModbusActivity::RTU_DIAGNOSTIC: return "RTU_DIAGNOSTIC";
    case ModbusActivity::IDLE: return "IDLE";
    default: return "IDLE";
  }
}

static ModbusMessage logBridgeRequest(ModbusMessage message) {
  ++tcpRxCount;
  addLog("TCP RX PDU (unit ID + function + data): " +
         bytesToHex(message.data(), message.size()), "debug");
  return message;
}

static ModbusMessage logBridgeResponse(ModbusMessage message) {
  ++tcpTxCount;
  Modbus::Error error = message.getError();
  uint8_t errorCode = static_cast<uint8_t>(error);
  if (error == Modbus::SERVER_DEVICE_BUSY) {
    addLog("MODBUS: TCP request rejected because gateway is not in Data Transfer",
           "warning");
  } else if (error == Modbus::SUCCESS ||
      (errorCode >= 1 && errorCode <= 0x0B)) {
    ModbusMessage rtuFrame = message;
    RTUutils::addCRC(rtuFrame);
    addLog("RTU RX: " + bytesToHex(rtuFrame.data(), rtuFrame.size()),
           error == Modbus::SUCCESS ? "debug" : "warning");
  } else {
    addLog("RTU ERROR: eModbus error 0x" + String(errorCode, HEX),
           "warning");
    const uint8_t exceptionCode =
      (error == Modbus::TIMEOUT || error == Modbus::GATEWAY_TARGET_NO_RESP)
        ? Modbus::GATEWAY_TARGET_NO_RESP
        : (error == Modbus::REQUEST_QUEUE_FULL ||
           error == Modbus::SERVER_DEVICE_BUSY)
          ? Modbus::SERVER_DEVICE_BUSY
          : Modbus::GATEWAY_PATH_UNAVAIL;
    message.setError(message.getServerID(),
                     static_cast<uint8_t>(message.getFunctionCode() & 0x7F),
                     static_cast<Modbus::Error>(exceptionCode));
    error = message.getError();
    errorCode = static_cast<uint8_t>(error);
  }

  addLog("TCP TX PDU (unit ID + function + data): " +
         bytesToHex(message.data(), message.size()),
         error == Modbus::SUCCESS ? "debug" : "warning");
  if (error == Modbus::SUCCESS) {
    reqCount++;
  } else {
    errCount++;
  }
  return message;
}

bool initModbusTCP() {
  tcpInitialized.store(false);
  modbusReady = false;
  rtuBusMutex = xSemaphoreCreateMutex();
  if (!rtuBusMutex) {
    addLog("MODBUS: unable to create RTU bus state mutex", "error");
    return false;
  }

  for (uint16_t unitId = 1; unitId <= 247; ++unitId) {
    const uint8_t id = static_cast<uint8_t>(unitId);
    if (!modbusBridge.attachServer(id, id, ANY_FUNCTION_CODE, &guardedRtuClient) ||
        !modbusBridge.addRequestFilter(id, logBridgeRequest) ||
        !modbusBridge.addResponseFilter(id, logBridgeResponse)) {
      addLog("MODBUS: failed to attach eModbus bridge for unit ID " +
             String(id), "error");
      return false;
    }
  }

  if (!modbusBridge.start(modbusTcpPort, 10, modbusTcpTimeoutMs)) {
    addLog("MODBUS: eModbus TCP server failed to start on port " +
           String(modbusTcpPort) + " with timeout " +
           String(modbusTcpTimeoutMs) + " ms", "error");
    modbusReady = false;
    return false;
  }

  tcpInitialized.store(true);
  modbusReady = true;
  updateReadyState("TCP bridge initialized");
  addLog("MODBUS: eModbus TCP-to-RTU bridge listening on port " +
         String(modbusTcpPort) + " with " + String(modbusTcpTimeoutMs) +
         " ms response timeout", "success");
  return true;
}

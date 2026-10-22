#include "modbus_rtu.h"
#include <atomic>
#include <cstring>

ModbusClientRTU rtuClient(MODBUS_RTS_PIN);
bool modbusReady = false;
bool rtuReady = false;

static std::atomic<uint32_t> requestToken{1};

static uint32_t serialFormat() {
  const bool sevenBits = uartCfg.data_bits == 7;
  const bool twoStopBits = uartCfg.stop_bits == 2;

  if (uartCfg.parity == 'E') {
    if (sevenBits) return twoStopBits ? SERIAL_7E2 : SERIAL_7E1;
    return twoStopBits ? SERIAL_8E2 : SERIAL_8E1;
  }
  if (uartCfg.parity == 'O') {
    if (sevenBits) return twoStopBits ? SERIAL_7O2 : SERIAL_7O1;
    return twoStopBits ? SERIAL_8O2 : SERIAL_8O1;
  }
  if (sevenBits) return twoStopBits ? SERIAL_7N2 : SERIAL_7N1;
  return twoStopBits ? SERIAL_8N2 : SERIAL_8N1;
}

static void logRtuError(Modbus::Error error, uint8_t unitId, uint8_t functionCode) {
  const uint8_t code = static_cast<uint8_t>(error);
  if (error == Modbus::TIMEOUT || error == Modbus::GATEWAY_TARGET_NO_RESP) {
    addLog("RTU TIMEOUT: unit ID " + String(unitId) +
           " function " + String(functionCode, HEX) +
           " did not respond within " + String(modbusRtuTimeoutMs) + " ms",
           "warning");
  } else {
    addLog("RTU ERROR: unit ID " + String(unitId) +
           " function " + String(functionCode, HEX) +
           " eModbus error 0x" + String(code, HEX), "warning");
  }
  errCount++;
}

bool initModbusRTU() {
  rtuReady = false;
  RTUutils::prepareHardwareSerial(Serial2, MODBUS_RX_BUF_SIZE);
  Serial2.begin(uartCfg.baud, serialFormat(), MODBUS_RX_PIN, MODBUS_TX_PIN);
  rtuClient.setTimeout(modbusRtuTimeoutMs);
  rtuClient.begin(Serial2, 1);
  rtuReady = true;
  addLog("RTU: eModbus client started on UART2 at " +
         String(uartCfg.baud) + " baud", "success");
  return rtuReady;
}

bool rtu_read_registers(uint8_t slave_id, uint8_t fc, uint16_t address,
                        uint16_t quantity, uint16_t* result,
                        ModbusRtuExchangeTrace* trace) {
  if (trace) {
    trace->request_frame_len = 0;
    trace->response_frame_len = 0;
    trace->error_code = static_cast<uint8_t>(Modbus::SUCCESS);
  }
  if (!rtuReady || !result ||
      (fc != MODBUS_FC_READ_HOLDING_REGISTERS &&
       fc != MODBUS_FC_READ_INPUT_REGISTERS) ||
      quantity == 0 || quantity > 125 ||
      static_cast<uint32_t>(address) + quantity > 65536UL) {
    addLog("RTU read rejected: invalid state or register-read parameters", "warning");
    return false;
  }

  ModbusMessage request(slave_id, fc, address, quantity);
  if (!request) {
    addLog("RTU read rejected: eModbus could not build the request", "warning");
    return false;
  }

  ModbusMessage requestFrame = request;
  RTUutils::addCRC(requestFrame);
  ++rtuTxCount;
  addLog("RTU TX: " + bytesToHex(requestFrame.data(),
                                 requestFrame.size()), "debug");
  if (trace && requestFrame.size() <= sizeof(trace->request_frame)) {
    trace->request_frame_len = requestFrame.size();
    memcpy(trace->request_frame, requestFrame.data(), trace->request_frame_len);
  }

  const uint32_t token = requestToken.fetch_add(1);
  ModbusMessage response = rtuClient.syncRequest(request, token);
  if (response.size() > 0) ++rtuRxCount;
  const Modbus::Error error = response.getError();
  const uint8_t errorCode = static_cast<uint8_t>(error);
  if (trace) trace->error_code = errorCode;

  if (error != Modbus::SUCCESS) {
    if (errorCode >= 1 && errorCode <= 0x0B) {
      ModbusMessage responseFrame = response;
      RTUutils::addCRC(responseFrame);
      addLog("RTU RX exception: " + bytesToHex(responseFrame.data(),
                                               responseFrame.size()), "warning");
      if (trace && responseFrame.size() <= sizeof(trace->response_frame)) {
        trace->response_frame_len = responseFrame.size();
        memcpy(trace->response_frame, responseFrame.data(),
               trace->response_frame_len);
      }
      errCount++;
    } else {
      logRtuError(error, slave_id, fc);
    }
    if (error == Modbus::REQUEST_QUEUE_FULL ||
        error == Modbus::EMPTY_MESSAGE) {
      if (trace) trace->request_frame_len = 0;
    }
    return false;
  }

  ModbusMessage responseFrame = response;
  RTUutils::addCRC(responseFrame);
  addLog("RTU RX: " + bytesToHex(responseFrame.data(),
                                 responseFrame.size()), "debug");
  if (trace && responseFrame.size() <= sizeof(trace->response_frame)) {
    trace->response_frame_len = responseFrame.size();
    memcpy(trace->response_frame, responseFrame.data(),
           trace->response_frame_len);
  }

  const size_t expectedSize = 3 + static_cast<size_t>(quantity) * 2;
  if (response.size() != expectedSize || response[0] != slave_id ||
      response[1] != fc || response[2] != quantity * 2) {
    addLog("RTU RX error: register response has invalid unit, function, or byte count",
           "warning");
    errCount++;
    return false;
  }

  for (uint16_t index = 0; index < quantity; ++index) {
    result[index] =
      (static_cast<uint16_t>(response[3 + index * 2]) << 8) |
      response[4 + index * 2];
  }
  reqCount++;
  return true;
}

bool rtu_write_single_register(uint8_t slave_id, uint16_t address,
                               uint16_t value) {
  if (!rtuReady) return false;
  ModbusMessage request(slave_id, MODBUS_FC_WRITE_SINGLE_REGISTER, address, value);
  if (!request) return false;

  ModbusMessage requestFrame = request;
  RTUutils::addCRC(requestFrame);
  ++rtuTxCount;
  addLog("RTU TX: " + bytesToHex(requestFrame.data(), requestFrame.size()), "debug");

  ModbusMessage response =
    rtuClient.syncRequest(request, requestToken.fetch_add(1));
  if (response.size() > 0) ++rtuRxCount;
  const Modbus::Error error = response.getError();
  if (error != Modbus::SUCCESS) {
    logRtuError(error, slave_id, MODBUS_FC_WRITE_SINGLE_REGISTER);
    return false;
  }
  if (response.size() != request.size() ||
      memcmp(response.data(), request.data(), request.size()) != 0) {
    addLog("RTU RX error: write-register response does not echo the request",
           "warning");
    errCount++;
    return false;
  }

  ModbusMessage responseFrame = response;
  RTUutils::addCRC(responseFrame);
  addLog("RTU RX: " + bytesToHex(responseFrame.data(), responseFrame.size()), "debug");
  reqCount++;
  return true;
}

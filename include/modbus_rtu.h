#pragma once

#include <Arduino.h>
#include "ModbusClientRTU.h"
#include "RTUutils.h"
#include "config.h"

#define MODBUS_FC_READ_HOLDING_REGISTERS   0x03
#define MODBUS_FC_READ_INPUT_REGISTERS     0x04
#define MODBUS_FC_WRITE_SINGLE_COIL        0x05
#define MODBUS_FC_WRITE_SINGLE_REGISTER    0x06
#define MODBUS_FC_WRITE_MULTIPLE_REGISTERS 0x10

struct ModbusRtuExchangeTrace {
  uint8_t request_frame[256];
  size_t request_frame_len;
  uint8_t response_frame[256];
  size_t response_frame_len;
  uint8_t error_code;
};

extern ModbusClientRTU rtuClient;
extern bool modbusReady;
extern bool rtuReady;

bool initModbusRTU();
bool rtu_read_registers(uint8_t slave_id, uint8_t fc, uint16_t address,
                        uint16_t quantity, uint16_t* result,
                        ModbusRtuExchangeTrace* trace = nullptr);
bool rtu_write_single_register(uint8_t slave_id, uint16_t address, uint16_t value);

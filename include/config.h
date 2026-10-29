#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <deque>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "driver/uart.h"

#define FIRMWARE_VERSION "2.1.0"
#define DEFAULT_GATEWAY_HOSTNAME "esp32-modbus-gw"

// Hardware Configuration (Native ESP-IDF RS485 Half-Duplex)
#define MODBUS_UART_NUM      UART_NUM_2
#define MODBUS_TX_PIN        17
#define MODBUS_RX_PIN        18
#define MODBUS_RTS_PIN       21   // Connects to MAX485 DE & !RE
#define MODBUS_CTS_PIN       UART_PIN_NO_CHANGE

#define MODBUS_RX_BUF_SIZE   512
#define MODBUS_TX_BUF_SIZE   512

// Defaults
#define MODBUS_DEFAULT_BAUD     9600
#define MODBUS_DEFAULT_STOPBITS 1
#define MODBUS_DEFAULT_DATABITS 8
#define MODBUS_DEFAULT_PARITY   'N'

#define WEB_PORT                  80
#define MODBUS_DEFAULT_TCP_PORT    502
#define MODBUS_DEFAULT_RTU_TIMEOUT_MS 500
#define MODBUS_DEFAULT_TCP_TIMEOUT_MS 1000
#define MAX_LOG_ENTRIES           100

enum class LogLevel : uint8_t {
  ERROR = 0,
  WARNING = 1,
  INFO = 2,
  DEBUG = 3
};

extern const char* AP_SSID;
extern String ap_password;
extern String gatewayHostname;

struct UARTConfig {
  uint32_t baud;
  uint8_t stop_bits;
  uint8_t data_bits;
  char parity;
};

extern UARTConfig uartCfg;
extern uint16_t modbusTcpPort;
extern uint32_t modbusRtuTimeoutMs;
extern uint32_t modbusTcpTimeoutMs;
extern String wifi_ssid;
extern String wifi_password;
extern bool wifi_configured;
extern bool wifiUseDhcp;
extern String wifiStaticIp;
extern String wifiGateway;
extern String wifiSubnet;
extern String wifiDns;

extern String http_user;

extern unsigned long reqCount;
extern unsigned long errCount;
extern unsigned long tcpConnections;
extern std::atomic<unsigned long> rtuTxCount;
extern std::atomic<unsigned long> rtuRxCount;
extern std::atomic<unsigned long> tcpTxCount;
extern std::atomic<unsigned long> tcpRxCount;
extern unsigned long startMs;

void loadConfig();
bool initLogging();
LogLevel configuredSerialLogLevel();
LogLevel configuredWebLogLevel();
bool saveLogLevels(LogLevel serialLevel, LogLevel webLevel);
bool copyLogs(std::deque<String>& logs);
bool clearLogs();
void saveUartConfig();
bool saveGatewaySettings(const UARTConfig& uart, uint16_t tcpPort,
                         uint32_t rtuTimeoutMs, uint32_t tcpTimeoutMs,
                         const String& hostname);
bool validMdnsHostname(const String& hostname);
bool saveWifiConfig(const String& ssid, const String& password, bool useDhcp,
                    const String& ip, const String& gateway,
                    const String& subnet, const String& dns);
void clearWifiConfig();
bool httpCredentialsConfigured();
bool verifyHttpPassword(const String& password);
bool saveHttpAuth(const String& user, const String& pass);
void saveStats();
bool factoryReset();
void addLog(const String &msg, const char* level = "info");
void addSerialLog(const String& msg, const char* level = "info");
String bytesToHex(const uint8_t* data, size_t len);

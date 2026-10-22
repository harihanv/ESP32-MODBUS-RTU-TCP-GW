#pragma once

bool initModbusTCP();
bool beginRtuDiagnostic();
void endRtuDiagnostic();
bool setModbusConfigurationMode(bool active);
void updateGatewayInitializationState(bool wifiReady, bool rtuInitialized,
                                      bool tcpInitialized);
void updateGatewayNetworkState(bool wifiReady);
void maintainModbusPathState();
const char* modbusPathState();
const char* modbusActivityState();

#pragma once
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "modbus_rtu.h"
#include "web_pages.h"

void initWebServer();
void handleWebServerRestart();
bool wifiNetworkScanInProgress();

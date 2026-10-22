#pragma once
#include <Arduino.h>

bool otaAvailable();
size_t otaMaximumImageSize();
bool beginOtaUpdate();
size_t writeOtaUpdate(uint8_t* data, size_t length);
bool finishOtaUpdate();
void abortOtaUpdate();
String otaError();
void initOTA();

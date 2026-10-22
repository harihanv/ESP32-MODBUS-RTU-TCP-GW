#include "ota_handler.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include "config.h"

bool otaAvailable() {
  return esp_ota_get_next_update_partition(nullptr) != nullptr;
}

size_t otaMaximumImageSize() {
  const esp_partition_t* partition =
    esp_ota_get_next_update_partition(nullptr);
  return partition ? partition->size : 0;
}

bool beginOtaUpdate() {
  return otaAvailable() && Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
}

size_t writeOtaUpdate(uint8_t* data, size_t length) {
  return Update.write(data, length);
}

bool finishOtaUpdate() {
  return Update.end(true);
}

void abortOtaUpdate() {
  Update.abort();
}

String otaError() {
  return String(Update.errorString());
}

void initOTA() {
  if (otaAvailable()) {
    addLog("OTA: authenticated firmware updates enabled; maximum image size " +
           String(otaMaximumImageSize()) + " bytes", "success");
  } else {
    addLog("OTA: no inactive firmware partition is available", "error");
  }
}

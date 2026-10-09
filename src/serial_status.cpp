#include "serial_status.h"
#include <esp_system.h>

#ifndef PYRRISMA_PIO_ENV
#define PYRRISMA_PIO_ENV "unknown"
#endif

const char *serialStatusColorName(ActuatorColor color) {
  switch (color) {
    case ACTUATOR_COLOR_RED:
      return "red";
    case ACTUATOR_COLOR_GREEN:
      return "green";
    case ACTUATOR_COLOR_BLUE:
      return "blue";
    case ACTUATOR_COLOR_YELLOW:
      return "yellow";
    default:
      return "?";
  }
}

const char *serialStatusColorNameU8(uint8_t color) {
  if (color >= ACTUATOR_COLOR_COUNT)
    return "?";
  return serialStatusColorName((ActuatorColor)color);
}

void serialStatusBanner(const char *roleTitle) {
  Serial.println();
  Serial.println("============================================================");
  Serial.printf("  PYRRISMA — %s\n", roleTitle ? roleTitle : "NODE");
  Serial.printf("  PlatformIO env: %s\n", PYRRISMA_PIO_ENV);
  Serial.printf("  Serial: 115200 baud (pio device monitor -e %s)\n", PYRRISMA_PIO_ENV);
  Serial.printf("  Uptime: boot @ millis=%lu\n", (unsigned long)millis());
  Serial.printf("  Reset reason: %d (ESP-IDF esp_reset_reason)\n", (int)esp_reset_reason());
  Serial.println("  Log tags: [HB] heartbeat  [ESPNOW] radio  [SOL] solenoids  [DMX] lighting");
  Serial.println("============================================================");
  Serial.println();
}

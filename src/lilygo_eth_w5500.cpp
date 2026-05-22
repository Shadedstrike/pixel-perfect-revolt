#include "lilygo_eth_w5500.h"
#include "mqtt_config.h"
#include <WiFi.h>
#include <ETH.h>

#if LILYGO_ETH_BOARD != 0

#if defined(ARDUINO_EVENT_ETH_GOT_IP)
#define LILYGO_EVT_ETH_GOT_IP ARDUINO_EVENT_ETH_GOT_IP
#define LILYGO_EVT_ETH_DISCONN ARDUINO_EVENT_ETH_DISCONNECTED
#define LILYGO_EVT_ETH_STOP ARDUINO_EVENT_ETH_STOP
#define LILYGO_EVT_ETH_START ARDUINO_EVENT_ETH_START
#define LILYGO_EVT_ETH_CONNECTED ARDUINO_EVENT_ETH_CONNECTED
#elif defined(SYSTEM_EVENT_ETH_GOT_IP)
#define LILYGO_EVT_ETH_GOT_IP SYSTEM_EVENT_ETH_GOT_IP
#define LILYGO_EVT_ETH_DISCONN SYSTEM_EVENT_ETH_DISCONNECTED
#define LILYGO_EVT_ETH_STOP SYSTEM_EVENT_ETH_STOP
#define LILYGO_EVT_ETH_START SYSTEM_EVENT_ETH_START
#define LILYGO_EVT_ETH_CONNECTED SYSTEM_EVENT_ETH_CONNECTED
#else
#error "This Arduino-ESP32 core does not expose known ETH event IDs (try ESP32 Arduino 2.0.11+ or 3.x)."
#endif

static volatile bool s_ethGotIp = false;
static bool s_handlerRegistered = false;

static void onEthEvent(WiFiEvent_t event) {
  switch (event) {
  case LILYGO_EVT_ETH_START:
    ETH.setHostname("pyrrisma-eth");
    break;
  case LILYGO_EVT_ETH_CONNECTED:
    Serial.println("[ETH] link up");
    break;
  case LILYGO_EVT_ETH_GOT_IP:
    s_ethGotIp = true;
    Serial.printf("[ETH] IP %s\n", ETH.localIP().toString().c_str());
    break;
  case LILYGO_EVT_ETH_DISCONN:
    Serial.println("[ETH] disconnected");
    s_ethGotIp = false;
    break;
  case LILYGO_EVT_ETH_STOP:
    s_ethGotIp = false;
    break;
  default:
    break;
  }
}

bool lilygoEthW5500Begin() {
  if (!s_handlerRegistered) {
    WiFi.onEvent(onEthEvent);
    s_handlerRegistered = true;
  }

#if LILYGO_ETH_BOARD == 1
  // T-ETH-Elite ESP32-S3 — LilyGO utilities.h (W5500 on SPI3)
  return ETH.begin(ETH_PHY_W5500, 1, 45, 14, -1, SPI3_HOST, 48, 47, 21);
#elif LILYGO_ETH_BOARD == 2
  // T-ETH-Lite ESP32-S3
  return ETH.begin(ETH_PHY_W5500, 1, 9, 13, 14, SPI3_HOST, 10, 11, 12);
#else
#error "LILYGO_ETH_BOARD must be 0 (WiFi), 1 (T-ETH-Elite S3), or 2 (T-ETH-Lite S3)"
#endif
}

bool lilygoEthW5500Connected() { return s_ethGotIp; }

IPAddress lilygoEthLocalIP() { return ETH.localIP(); }

#else

bool lilygoEthW5500Begin() { return false; }
bool lilygoEthW5500Connected() { return false; }
IPAddress lilygoEthLocalIP() { return IPAddress(0, 0, 0, 0); }

#endif

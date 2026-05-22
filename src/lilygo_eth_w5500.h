#ifndef LILYGO_ETH_W5500_H
#define LILYGO_ETH_W5500_H

#include <Arduino.h>
#include <IPAddress.h>

// Implemented in lilygo_eth_w5500.cpp when LILYGO_ETH_BOARD is non-zero (see mqtt_config.h).
bool lilygoEthW5500Begin();
bool lilygoEthW5500Connected();
IPAddress lilygoEthLocalIP();

#endif

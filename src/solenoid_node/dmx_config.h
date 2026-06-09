#ifndef SOLENOID_NODE_DMX_CONFIG_H
#define SOLENOID_NODE_DMX_CONFIG_H

// SparkFun RS-485 breakout (BOB-10124) on ESP32-S3 DevKitC-1:
//   DI  -> DMX_TX_PIN (default GPIO 4)
//   RO  -> DMX_RX_PIN (default GPIO 5)
//   DE + RE (jumpered) -> DMX_RTS_PIN (default GPIO 21)
//   MCP23017 I2C uses GPIO 18=SDA, GPIO 17=SCL — do not wire RS-485 to 17/18.
//   VCC -> 3.3 V, GND -> common GND
//   A/B -> DMX+ / DMX- to first fixture IN (Shehds par), OUT daisy-chains to bubble machine.
//
// Set each fixture's DMX address on the unit (DIP/display). Defaults below assume:
//   Shehds RGBWA+UV par @ address 1, 6-channel mode (R,G,B,W,Amber,UV)
//   Bubble machine @ address 7, 1 channel (0=off, 128-255=blow)

#ifndef DMX_TX_PIN
#define DMX_TX_PIN 4
#endif
#ifndef DMX_RX_PIN
#define DMX_RX_PIN 5
#endif
#ifndef DMX_RTS_PIN
#define DMX_RTS_PIN 21
#endif

#ifndef DMX_PAR_START_ADDR
#define DMX_PAR_START_ADDR 1
#endif

// Offsets within the par footprint (0-based from DMX_PAR_START_ADDR).
#ifndef DMX_PAR_CH_RED
#define DMX_PAR_CH_RED 0
#endif
#ifndef DMX_PAR_CH_GREEN
#define DMX_PAR_CH_GREEN 1
#endif
#ifndef DMX_PAR_CH_BLUE
#define DMX_PAR_CH_BLUE 2
#endif
#ifndef DMX_PAR_CH_WHITE
#define DMX_PAR_CH_WHITE 3
#endif
#ifndef DMX_PAR_CH_AMBER
#define DMX_PAR_CH_AMBER 4
#endif
#ifndef DMX_PAR_CH_UV
#define DMX_PAR_CH_UV 5
#endif

#ifndef DMX_BUBBLE_ADDR
#define DMX_BUBBLE_ADDR 7
#endif
#ifndef DMX_BUBBLE_ON_LEVEL
#define DMX_BUBBLE_ON_LEVEL 255
#endif

#ifndef DMX_LEVEL_FULL
#define DMX_LEVEL_FULL 255
#endif

// How many slots to transmit (start code + slots). Must cover last used address.
#ifndef DMX_SLOT_COUNT
#define DMX_SLOT_COUNT 8
#endif

#ifndef DMX_REFRESH_HZ
#define DMX_REFRESH_HZ 40
#endif

#ifndef DMX_DEBUG_HB_MS
#define DMX_DEBUG_HB_MS 5000
#endif

#endif

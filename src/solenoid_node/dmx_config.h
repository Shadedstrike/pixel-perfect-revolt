#ifndef SOLENOID_NODE_DMX_CONFIG_H

#define SOLENOID_NODE_DMX_CONFIG_H



// SparkFun RS-485 breakout (BOB-10124) on ESP32-S3 DevKitC-1:

//   DI  -> DMX_TX_PIN (default GPIO 4)

//   RO  -> DMX_RX_PIN (default GPIO 5)

//   DE + RE (jumpered) -> DMX_RTS_PIN (default GPIO 21)

//   MCP23017 I2C uses GPIO 18=SDA, GPIO 17=SCL — do not wire RS-485 to 17/18.

//

// Daisy chain: Shehds PAR @ address 1 (OUT) -> bubble machine @ address 7 (IN).

//

// IMPORTANT: Set bubble machine menu d001 to 007 (address 7). If left at 001 it reads

// the PAR channels: red->CH1 fan spin, amber->CH5 green LED, etc.

// Shehds RGBWA+UV par @ address 1, 6-channel mode (R,G,B,W,Amber,UV)

//

// Bubble machine @ address 7, 6-channel mode (manual labels on unit):

//   1CH fan, 2CH macro, 3CH white, 4CH blue*, 5CH green, 6CH red*

//   *This fixture wires 4CH/6CH red-blue swapped vs manual — see dmxOutputSetBubbleLevels().



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

#ifndef DMX_PAR_CHANNEL_COUNT

#define DMX_PAR_CHANNEL_COUNT 6

#endif



#ifndef DMX_BUBBLE_ADDR

#define DMX_BUBBLE_ADDR 7

#endif



#ifndef DMX_BUBBLE_CH_FAN

#define DMX_BUBBLE_CH_FAN 0

#endif

#ifndef DMX_BUBBLE_CH_MACRO

#define DMX_BUBBLE_CH_MACRO 1

#endif

#ifndef DMX_BUBBLE_CH_WHITE

#define DMX_BUBBLE_CH_WHITE 2

#endif

#ifndef DMX_BUBBLE_CH_BLUE

#define DMX_BUBBLE_CH_BLUE 3

#endif

#ifndef DMX_BUBBLE_CH_GREEN

#define DMX_BUBBLE_CH_GREEN 4

#endif

#ifndef DMX_BUBBLE_CH_RED

#define DMX_BUBBLE_CH_RED 5

#endif

#ifndef DMX_BUBBLE_CHANNEL_COUNT

#define DMX_BUBBLE_CHANNEL_COUNT 6

#endif



#ifndef DMX_BUBBLE_FAN_MAX

#define DMX_BUBBLE_FAN_MAX 255

#endif

// Manual 1CH: 0-9 off, 10-255 variable wind.
#ifndef DMX_BUBBLE_FAN_MIN

#define DMX_BUBBLE_FAN_MIN 10

#endif



#ifndef DMX_LEVEL_FULL

#define DMX_LEVEL_FULL 255

#endif



// Slot 0 = start code; covers PAR @1 (6ch) and bubble @7 (6ch).

#ifndef DMX_SLOT_COUNT

#define DMX_SLOT_COUNT 13

#endif



#ifndef DMX_REFRESH_HZ

#define DMX_REFRESH_HZ 40

#endif



#endif


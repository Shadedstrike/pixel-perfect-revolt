#ifndef RHYTHM_SD_PINS_H
#define RHYTHM_SD_PINS_H

// MicroSD on LILYGO T-ETH series (SPI) — see LilyGO-T-ETH-Series examples/utilities.h
//
// T-ETH-Elite ESP32-S3 (default below). Override in platformio.ini build_flags if needed:
//   -DRHYTHM_SD_CS_PIN=12 -DRHYTHM_SD_SCK_PIN=10 ...
//
// T-ETH-Lite ESP32-S3 uses: CS=42, SCK=7, MISO=5, MOSI=6

// CS moved 12 -> 21.  *** REQUIRES REWIRE: SD module CS lead 12 -> 21. ***
//
// Why it had to move: GPIO 12 is BTN_PINS[1] = IDX_LEFT[1] = the left blue key.
// SPI drives CS push-pull, so while a song is loaded a button press cannot pull
// the line down and reads return chip-select traffic instead — phantom blue
// presses firing the relay at random. Masking it (pinOwnedBySd, buttons.cpp)
// stopped the phantoms but killed the key during song mode.
//
// Why 21 and not 14: on T-ETH-Elite the W5500 sits on 45(CS) 14(INT) 48(SCK)
// 47(MISO) 21(MOSI) — see lilygo_eth_w5500.cpp. Ethernet is compiled out
// (LILYGO_ETH_BOARD 0), but the chip is still physically wired. GPIO 14 is the
// W5500's INT *output* and would fight our CS. GPIO 21 is the W5500's MOSI,
// which is an *input* at the W5500 — nothing drives it from that end, and its
// CS (45) is never asserted, so the chip ignores whatever appears there.
//
// Rest of the controller map: I2S 4/6/13, I2C 17/18, keys
// 2/5/7/8/12/15/16/38/39/46, SD 9/10/11, USB 19/20, UART0 43/44, flash 26-32.
#ifndef RHYTHM_SD_CS_PIN
#define RHYTHM_SD_CS_PIN 21
#endif
#ifndef RHYTHM_SD_SCK_PIN
#define RHYTHM_SD_SCK_PIN 10
#endif
#ifndef RHYTHM_SD_MISO_PIN
#define RHYTHM_SD_MISO_PIN 9
#endif
// If MISO matches a BTN_PINS entry, that key is ignored as a button while SD is mounted (see readLevelDebounced). Elite default: MISO is GPIO9 — not a button if keys use GPIO15 instead.
#ifndef RHYTHM_SD_MOSI_PIN
#define RHYTHM_SD_MOSI_PIN 11
#endif

// Set to 0 in build_flags to skip SD entirely (LittleFS / synth only).
#ifndef RHYTHM_ENABLE_SD
#define RHYTHM_ENABLE_SD 1
#endif

// SPI clock for SD (Hz). 10 MHz is safe; 20–25 MHz often works on short traces / good cards.
#ifndef RHYTHM_SD_SPI_HZ
#define RHYTHM_SD_SPI_HZ 20000000
#endif

#endif

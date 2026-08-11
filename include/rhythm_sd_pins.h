#ifndef RHYTHM_SD_PINS_H
#define RHYTHM_SD_PINS_H

// MicroSD on LILYGO T-ETH series (SPI) — see LilyGO-T-ETH-Series examples/utilities.h
//
// T-ETH-Elite ESP32-S3 (default below). Override in platformio.ini build_flags if needed:
//   -DRHYTHM_SD_CS_PIN=12 -DRHYTHM_SD_SCK_PIN=10 ...
//
// T-ETH-Lite ESP32-S3 uses: CS=42, SCK=7, MISO=5, MOSI=6

// SD CS stays on GPIO 12. The LEFT BLUE KEY moved 12 -> 42 instead (BTN_PINS[1]).
// Same conflict resolved from the other end, and it matches how the hardware was
// actually rewired — no further wire changes needed.
//
// Why something had to move: GPIO 12 was BTN_PINS[1] = IDX_LEFT[1] = the left blue key.
// SPI drives CS push-pull, so while a song is loaded a button press cannot pull
// the line down and reads return chip-select traffic instead — phantom blue
// presses firing the relay at random. Masking it (pinOwnedBySd, buttons.cpp)
// stopped the phantoms but killed the key during song mode.
//
// Why the KEY moved and not CS: GPIO 12 is hardwired to CS inside the SD module —
// it cannot be reassigned. The button's switch lead is the only movable end.
//
// Why 42 for the key: on the actual breakout, the only terminals that can be
// grounded without crashing the board are GPIO 9, 10, 40, 41, 42. 9/10/12 are
// already SD MISO/SCK/CS, so the usable set is 40/41/42. All three are full
// input+output GPIOs with internal pull-ups on ESP32-S3 (there are no input-only
// pins on the S3 — that is an ESP32-classic property). They are JTAG
// MTDO/MTDI/MTMS, but hardware JTAG is already unavailable because MTCK (GPIO 39)
// is a key, and debugging happens over USB-CDC.
//
// Rejected, for the record:
//   14 — W5500 INT, an output *from* the Ethernet chip.
//   21 — not usably broken out on this breakout.
//   45/47/48 — W5500 CS/MISO/SCK. See lilygo_eth_w5500.cpp. Ethernet is compiled
//        out (LILYGO_ETH_BOARD 0) but the chip is still physically wired.
//
// pinOwnedBySd() still masks 9/10/11/12 while SD is mounted. None of those is a
// key any more, so nothing gets masked — the guard is now purely defensive.
//
// Rest of the controller map: I2S 4/6/13, I2C 17/18, keys
// 2/5/7/8/15/16/38/39/42/46, SD 9/10/11/12, USB 19/20, UART0 43/44, flash 26-32.
#ifndef RHYTHM_SD_CS_PIN
#define RHYTHM_SD_CS_PIN 12
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

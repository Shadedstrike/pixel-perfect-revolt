#ifndef RHYTHM_SD_PINS_H
#define RHYTHM_SD_PINS_H

// MicroSD on LILYGO T-ETH series (SPI) — see LilyGO-T-ETH-Series examples/utilities.h
//
// T-ETH-Elite ESP32-S3 (default below). Override in platformio.ini build_flags if needed:
//   -DRHYTHM_SD_CS_PIN=12 -DRHYTHM_SD_SCK_PIN=10 ...
//
// T-ETH-Lite ESP32-S3 uses: CS=42, SCK=7, MISO=5, MOSI=6

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

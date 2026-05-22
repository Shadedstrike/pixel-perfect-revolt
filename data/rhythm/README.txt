Rhythm game: songs live only on the microSD (FAT32): /rhythm/*.mp3. Menu scan and playback use SD only.
If a file is missing or SD fails, the built-in synthetic guide plays instead.

Elite SD wiring (SPI): SCK=10, MISO=9, MOSI=11, CS=12 (override in include/rhythm_sd_pins.h or build_flags).

T-ETH-Lite ESP32-S3 SD pins differ (CS=42, SCK=7, MISO=5, MOSI=6) — set those in build_flags or rhythm_sd_pins.h.

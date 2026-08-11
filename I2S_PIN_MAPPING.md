# Controller GPIO map (ESP32-S3, LILYGO T-ETH Elite)

This file used to list an I2S plan that was never what shipped. Below is the map
as it exists in code. Verify against `src/config.h`, `include/rhythm_sd_pins.h`
and `src/lilygo_eth_w5500.cpp` before changing anything.

## In use

| GPIO | Function | Defined in |
|------|----------|------------|
| 2, 5, 7, 8 | Keys — 2/5/7/8 (right idx 7, left idx 2/3, right idx 9) | `config.cpp` `BTN_PINS` |
| 4 | I2S DATA (DIN) | `config.h` `I2S_DATA` |
| 6 | I2S LRCK (WSEL) | `config.h` `I2S_LRCK` |
| 9 | SD MISO | `rhythm_sd_pins.h` |
| 10 | SD SCK | `rhythm_sd_pins.h` |
| 11 | SD MOSI | `rhythm_sd_pins.h` |
| 12 | Key — left blue (`IDX_LEFT[1]`) | `config.cpp` `BTN_PINS[1]` |
| 13 | I2S BCLK | `config.h` `I2S_BCLK` |
| 15, 16, 38, 39, 46 | Keys | `config.cpp` `BTN_PINS` |
| 17 | I2C SDA — LCD + PCA9685 | `config.h` |
| 18 | I2C SCL — LCD + PCA9685 | `config.h` |
| 42 | **SD CS** | `rhythm_sd_pins.h` |

Full key list: `BTN_PINS = {38, 12, 5, 7, 16, 46, 39, 2, 15, 8}`
`IDX_LEFT = {0,1,2,3}` → GPIO 38, 12, 5, 7 · `IDX_RIGHT = {6,7,8,9}` → GPIO 39, 2, 15, 8
`IDX_FRONT_L = 4` → GPIO 16 · `IDX_FRONT_R = 5` → GPIO 46

## Reserved — do not reuse

| GPIO | Why |
|------|-----|
| 14 | W5500 **INT** — an output *from* the W5500; contends with anything we drive |
| 45, 47, 48 | W5500 CS / MISO / SCK |
| 19, 20 | USB D− / D+ (`ARDUINO_USB_CDC_ON_BOOT=1`) |
| 43, 44 | UART0 TX / RX |
| 26–32 | SPI flash |
| 33–37 | Reserved if the module has octal PSRAM |
| 0, 3, 45, 46 | Strapping pins (46 is used as a key anyway — it reads fine, but never drive it at boot) |

Ethernet is compiled out (`LILYGO_ETH_BOARD 0` in `mqtt_config.h`) but the W5500 is
still physically wired, which is why its pins stay reserved.

## The GPIO 12 / SD CS conflict (fixed — needs a rewire)

SD CS was on **GPIO 12**, which is also the **left blue key**. SPI drives CS
push-pull, so while a song was loaded:

- a press could not pull the line down — the SPI driver held it
- reads returned chip-select traffic, i.e. **phantom blue presses** that fired the
  relay and blue DMX at random during songs

`pinOwnedBySd()` in `buttons.cpp` now masks any pin the SD peripheral owns, which
stops the phantoms. But masking alone left the blue key dead during song mode, so
**CS moved to GPIO 42**.

**Action required: move the SD module's CS lead from GPIO 12 to GPIO 42.** Until
that wire moves, the SD card will not mount.

### Why 42 — measured, not assumed

On this breakout the only terminals that can be grounded **without crashing the
board** are **GPIO 9, 10, 40, 41, 42**. 9 and 10 are already SD MISO/SCK, so the
usable set is **40 / 41 / 42**.

All three are full input+output GPIOs on ESP32-S3 — the input-only 34–39 range is an
**ESP32-classic** property and does not apply to the S3. They are JTAG MTDO/MTDI/MTMS,
but hardware JTAG is already unavailable because MTCK (GPIO 39) is a key, and
debugging goes over USB-CDC.

Rejected, for the record:

| Candidate | Why not |
|-----------|---------|
| 14 | W5500 **INT** — an output *from* the Ethernet chip; would fight our CS |
| 21 | Not usably broken out on this breakout (grounding it crashes the board) |
| 45, 47, 48 | W5500 CS / MISO / SCK |

## Raspberry Pi breakout — terminal ↔ ESP32-S3 GPIO

**The breakout's silkscreen labels are Raspberry Pi GPIO numbers and functions, NOT
ESP32-S3 GPIO numbers.** They do not correspond. The terminal→ESP32-S3 mapping has
never been recorded — it has to be measured on the bench (procedure below).

Terminal labels as printed (preserved from the original notes):

**Left side (Pi function labels):**
A `PCMdout` · B `PCMdin` ⚠️ · C `PCMfs` ⚠️ · D `PWM1` · E `PWM0`
(⚠️ = observed to crash the board when driven — cause never established.)
Terminal D was found to map to PCMdout.

**Right side, top (T) to bottom (A), Pi GPIO numbers:**

| T | S | R | Q | P | O | N | M | L | K |
|---|---|---|---|---|---|---|---|---|---|
| +5V | GND | 3.3V | GPIO18 | GPIO23 | GPIO24 | GPIO25 ⚠️ | GPIO16 | GPIO26 | GPIO06 |

| J | I | H | G | F | E | D | C | B | A |
|---|---|---|---|---|---|---|---|---|---|
| GPIO05 | GPIO17 | ECLK | GPIO27 | EDAT | GPIO22 | GPIO04 | 3.3V | GND | +5V |

### Finding which terminal is a given ESP32-S3 GPIO

The firmware has a pin-identification mode (`src/debug.cpp`) that pulls every GPIO up
and reports which one goes low:

1. Run it and watch the serial monitor for
   `Touch PCMfs or PCMdin pins on the breakout board to GND to identify them.`
2. Short a breakout terminal to GND with a jumper.
3. It prints `>>> GPIO N detected! (touched to GND) <<<`.
4. Record terminal letter → GPIO N.

That is how to locate **GPIO 12** (the left blue key, which currently also carries SD
CS) and **GPIO 42** (the new SD CS). Do both before cutting anything, and write the
result into the table above so the next person doesn't have to repeat it.

Only **GPIO 9, 10, 40, 41, 42** can be grounded here without crashing the board, so
those are the only terminals worth probing.

The definitive source is LILYGO's schematic for T-ETH-Elite ESP32-S3, in the
`/schematic` folder of
[Xinyuan-LilyGO/LilyGO-T-ETH-Series](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series).
The published docs only state that the 40-pin header "follows the Raspberry Pi pinout as
closely as possible" with 23 GPIOs exposed — they do not give the per-pin mapping.

## Picking a new pin later

Anything not listed above. Check `pinOwnedBySd()` and `BTN_PINS` first — a pin that
is both a key and a peripheral line cannot be read as a key while that peripheral
is active, and that failure looks exactly like a firmware bug.

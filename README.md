# Pyrrisma (Pixel_perfect_revolt)

ESP32-S3 rhythm controller + remote actuator node over **ESP-NOW**. The controller runs the synth, LCD, rhythm game, and button LEDs. The actuator node drives solenoids, motors, and DMX (PAR + bubble machine).

## Two boards — two firmware images

| Board | PlatformIO env | Default COM | Role |
|-------|----------------|-------------|------|
| Controller | `esp32-s3-devkitc-1` | COM18 | Buttons, synth, LCD, ESP-NOW TX |
| Actuator | `solenoid-node` | COM23 | Solenoids, motors, DMX, ESP-NOW RX |

### Build, upload, and monitor

Run from the project root (`Pixel_perfect_revolt`). Default COM ports are set in `platformio.ini` (COM18 / COM23). If upload fails, run `pio device list` and use `--upload-port COMx`.

**Controller**

```powershell
pio run -e esp32-s3-devkitc-1                    # build only
pio run -e esp32-s3-devkitc-1 -t upload          # build + flash (COM18)
pio device monitor -e esp32-s3-devkitc-1         # serial log (115200; look for [ESPNOW] tx)
```

**Actuator**

```powershell
pio run -e solenoid-node                         # build only
pio run -e solenoid-node -t upload               # build + flash (COM23)
pio device monitor -e solenoid-node              # serial log (115200; look for [ESPNOW] rx)
```

**Upload + monitor in one go** (stop monitor with Ctrl+C, then re-run upload if needed):

```powershell
pio run -e esp32-s3-devkitc-1 -t upload && pio device monitor -e esp32-s3-devkitc-1
pio run -e solenoid-node -t upload && pio device monitor -e solenoid-node
```

**Wrong COM port**

```powershell
pio run -e esp32-s3-devkitc-1 -t upload --upload-port COM5
pio run -e solenoid-node -t upload --upload-port COM7
```

**Both boards must run matching `ACTUATOR_PROTO_VERSION`** (see `include/actuator_protocol.h`). After changing shared protocol or ESP-NOW settings, flash both.

---

## How to change settings

Most tunables use `#ifndef NAME` / `#define NAME` so you can override from **`platformio.ini`** without editing headers:

```ini
build_flags =
    -DESPNOW_WIFI_CHANNEL=11
    -DBUBBLE_PARTY_MS=3000
    -DDMX_BUBBLE_ADDR=7
```

Controller overrides go under `[env:esp32-s3-devkitc-1]`. Actuator overrides under `[env:solenoid-node]`. Shared headers (`include/actuator_config.h`) apply to **both** envs when that file is compiled in.

Constants in `.cpp` files (e.g. `src/leds.cpp`, `src/rhythm_game.cpp`) require editing source directly.

### Entering rhythm mode — the hold countdown

Hold the **four bottom keys** (yellow 38 + 39, blue 42 + 2 — slots 0/1, which the code calls "top" but sit at the bottom of the panel) for **5 s**. While held, the
LCD is taken over by `lcdRetroEnterCountdown()`:

```
[sprite border marching right ]
  PRESS 5 MORE SEC
R Y T H E M  M 0 D E     <- lightly glitched
[sprite border marching left  ]
```

- Counts **5 → 1**, never displays 0 (it enters at 0).
- Row 2 corrupts at most **two** columns per frame, and only columns holding a
  letter, so the phrase stays readable. Glitch positions come from a hash of
  (frame, column) rather than a fresh random each redraw — otherwise slow I2C
  writes make it read as noise instead of glitch.
- Borders use CGRAM slots **0–3**. Safe: meltdown also uses 0–3 but only during
  play, and the play lane uses 4–5.
- `lcdPollMs` drops to 60 ms while the countdown is up, or the sprite march looks
  broken at the normal 500 ms.
- Releasing any of the four keys resets the timer to zero — there is no partial
  credit.

Exit is still the **yellow pair only** (3 s → menu, 4 s → synth). That's a subset of
the enter gesture, which is harmless because the exit check only runs once the phase
is no longer `RG_NORMAL`.

### Input timing — `inputFastPoll()` (`src/main.cpp`)

Buttons used to be sampled once per `loop()`. In song mode that same loop decodes MP3
and drives the LCD over I2C, so a tap that began and ended inside one long frame was
never observed — no ESP-NOW ON/OFF was sent and **the relay and solenoids did not react
to fast taps during songs**, while working fine outside song mode.

`inputFastPoll()` samples all ten keys, latches edges, and pushes side-column holds
straight to the actuator link. It runs once per `loop()` **and** from inside
`rhythmGameAudioPump()`, so actuator latency no longer depends on frame length.

- Edges are sticky until `inputFastPollTake()` consumes them, so a press/release pair
  inside one frame survives. Both `edgeDown` and `edgeUp` can therefore be true in the
  same frame — consumers must treat them independently.
- `[INPUT] worst poll gap NNms (song=1)` prints every 5 s. Single-digit ms in song mode
  means it is working; 40 ms+ means the pump is not running often enough and sampling
  should move to a timer ISR.
- The 35 ms debounce lockout in `readLevelDebounced()` is wall-clock based, so polling
  faster only detects transitions sooner — it cannot introduce bounce.

---

## Settings reference

### `include/actuator_config.h` — ESP-NOW & actuator behavior (both boards)

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `ESPNOW_WIFI_CHANNEL` | `1` | WiFi/ESP-NOW radio channel. **Must match on controller and actuator.** Try `6` or `11` if interference. |
| `ACTUATOR_HB_MS` | `5000` | Serial heartbeat log interval on both sides. |
| `ACTUATOR_PULSE_MS` | `250` | Legacy; unused (hold-while-pressed replaced pulse). |
| `BUBBLE_HOLD_TRIGGER_MS` | `2000` | Hold **any** button this long → one bubble fan burst. |
| `BUBBLE_PARTY_MS` | `2000` | How long the bubble **fan** runs per burst (not stackable; re-arm after all buttons released). |
| `BUBBLE_FAN_RAMP_UP_MS` | `1500` | Fan DMX 1CH ramps from min → max over this time (soft start). |
| `BUBBLE_FAN_RAMP_DOWN_MS` | `500` | Fan ramps down in the last portion of the burst. |
| `ACTUATOR_IDLE_DMX_MS` | `100` | While synth idle, controller sends idle keepalive every 10 Hz; actuator runs local PAR/bubble animation. |
| `ACTUATOR_RGB_SYNC_MS` | `50` | While side buttons held, combined RGB DMX packet rate (~20 Hz). |
| `ACTUATOR_RX_FAILSAFE_MS` | `3000` | **Actuator only:** if a solenoid is ON and no ESP-NOW for this long → force all outputs off. |

---

### `include/actuator_protocol.h` — wireless packet format

| Setting | Value | What it changes |
|---------|-------|-----------------|
| `ACTUATOR_PROTO_VERSION` | `4` | Must match on both boards. Bump + reflash both when changing packet layout. |
| `ACTUATOR_ON_*` | — | Command types: color ON/OFF, bubble party, bubble kill, idle DMX, idle end, RGB hold. |

Color mapping (side buttons → actuator):

| Button column | `ActuatorColor` | Solenoid MCP ch | Motor | Relay |
|---------------|-----------------|-----------------|-------|-------|
| Yellow (top) | `YELLOW` | 3 | — | yes |
| Blue | `BLUE` | 2 | — (**solenoid disabled in firmware**) | yes |
| Green | `GREEN` | 1 | green driver | yes |
| Red (bottom) | `RED` | 0 | red driver | yes |

Yellow also runs the yellow motor driver. The relay on GPIO 16 is closed while
**any** of the four is held.

---

### `src/solenoid_node/dmx_config.h` — DMX hardware & fixtures (actuator)

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `DMX_TX_PIN` | `4` | RS-485 DI |
| `DMX_RX_PIN` | `5` | RS-485 RO |
| `DMX_RTS_PIN` | `21` | RS-485 DE/RE |
| `DMX_PAR_START_ADDR` | `1` | Shehds PAR start address (6ch RGBWA+UV). |
| `DMX_PAR_CH_*` | 0–5 | Channel offsets within PAR (R,G,B,W,Amber,UV). |
| `DMX_BUBBLE_ADDR` | `7` | Bubble machine start address — **set menu `d001` = 007 on unit**. |
| `DMX_BUBBLE_CH_*` | 0–5 | Bubble: fan, macro, white, blue, green, red (see manual). |
| `DMX_BUBBLE_FAN_MIN` | `10` | Manual: 1CH 0–9 = off; first “on” wind level. |
| `DMX_BUBBLE_FAN_MAX` | `255` | Max fan DMX during burst. Lower (e.g. `180`) = less current, fewer bubbles. |
| `DMX_LEVEL_FULL` | `255` | Button-hold color level for PAR/bubble RGB. |
| `DMX_SLOT_COUNT` | `13` | DMX frame size (PAR @1 + bubble @7, 6ch each + start code). |
| `DMX_REFRESH_HZ` | `40` | Continuous DMX transmit rate (keeps fixtures from timing out). |

**Bubble machine manual (6ch @ addr 7):**

| Ch | Function | Firmware |
|----|----------|----------|
| 1 | Fan 0–9 off, 10–255 variable | Ramped burst only |
| 2 | LED macros | Always `0` (we drive RGB on ch 3–6) |
| 3 | White | Available, usually `0` |
| 4–6 | Blue / Green / Red | Live + latched colors (4/6 swapped vs manual — fixed in code) |

**DMX chain:** PAR OUT → bubble IN.

---

### `src/solenoid_node/motor_config.h` — motor GPIO (actuator)

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `MOTOR_STBY_PIN` | `13` | TB6612 STBY (also in `platformio.ini`). LOW at boot, HIGH after init. |
| `MOTOR_RED_IN1/IN2` | 42 / 11 | Red motor direction |
| `MOTOR_GREEN_IN1/IN2` | 40 / 41 | Green motor direction |
| `MOTOR_YELLOW_IN1/IN2` | 7 / 10 | Yellow motor direction |

Do **not** use GPIO 4 or 5 for motors — those are DMX UART.

---

### `src/solenoid_node/solenoid_output.h` — air solenoids (actuator)

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `I2C_SDA` / `I2C_SCL` | 18 / 17 | MCP23017 I2C (do not conflict with DMX pins). |
| `MCP23017_ADDR` | `0x20` | I2C address |
| MCP physical ch 0–3 | — | yellow, blue, green, red solenoids (rewired order) |

The firmware translates logical color to physical channel at this boundary, so all
other code remains red/green/blue/yellow. Boot drives every solenoid **LOW**. Blue
is not fired unless `SOLENOID_BLUE_ENABLED=1`.

On a normal color press the solenoid opens immediately, its pump starts 50 ms
later, and release stops the pump immediately while holding the air solenoid open
for another 2 seconds. These timers are non-blocking.

Holding both front buttons for 40 seconds enters latched **PURGE MODE**. All three
pumps reverse and continue until the actuator node is rebooted. Purge intentionally
ignores later normal actuator commands.

---

### `src/solenoid_node/relay_config.h` — 5V relay (actuator)

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `RELAY_PIN` | `16` | Relay module IN (also in `platformio.ini`) |
| `RELAY_ACTIVE_LOW` | `0` | Drive HIGH to close — **must match the module jumper** |
| `RELAY_OPEN_DRAIN` | `0` | Always push-pull. See warning below. |
| `RELAY_SELFTEST_CYCLES` | `3` | Boot self-test clicks. Set `0` for installation. |

Hardware: SRD-05VDC-SL-C module ("1 Relay Module high/low level trigger"), jumper on **H**.
Wiring: `IN` → GPIO 16, `DC+` → **5V** (own supply — the coil draws ~71 mA), `DC-` → **its own short wire
to an ESP `GND` pin**.

**Jumper must match `RELAY_ACTIVE_LOW`** (`H`→`0`, `L`→`1`) or the relay sits closed at idle and opens on
press — inverted.

### Ground bonding is not optional

The module compares `IN` against **its own `DC-`**. Without a solid shared reference the GPIO level means
nothing at the module. This was a real fault here: the relay ignored GPIO 16 entirely and instead fired
whenever a *solenoid* switched, because solenoid current through the shared ground return shifted the
module's reference. It fired for red/green/yellow and did nothing for blue (whose solenoid is disabled) —
which looked exactly like a firmware bug. Probing `GND`↔`DC-` with a meter also fired it, because the
meter was momentarily acting as the missing bond.

Keep the module's ground return **off the solenoid return path** (star ground). Ground bounce from the
solenoids can otherwise energize the relay uncommanded — the one failure direction that matters, since
de-energized is the safe state.

`H` is the preferred jumper position: it triggers ~1.5 V above ground so 3.3 V logic has margin, and a
floating pin (boot, reset, ESP unpowered) reads LOW = de-energized.

Earlier notes in git history claimed `H` could not source enough current at 3.3 V. Those measurements
predate finding the ground fault and are not reliable — these modules are routinely driven from 3.3 V.

### Verifying

`relayOutputSelfTest()` runs in `setup()` before ESP-NOW, solenoids and DMX exist, and toggles the pin
directly — so it isolates the GPIO→module path from the rest of the system. Three clean click-in /
click-out cycles means the relay is genuinely driven by GPIO 16. Set `-DRELAY_SELFTEST_CYCLES=0` once
commissioned.

**Never set `RELAY_OPEN_DRAIN=1`.** The module's input side is `5V ──[R]──►|LED──── IN`, so releasing
the pin lets IN rise to the module's 5 V rail. ESP32-S3 pads clamp at VDD+0.3 (~3.6 V) and are not 5 V
tolerant. Push-pull holds the pin at 0 V / 3.3 V so it is never released and never sees 5 V.

Fires on **any** color hold, blue included — closed while at least one button is
down, open when the last one is released (held colors are tracked as a bitmask, so
overlapping presses don't cut it short). Boot state is **open**, and the pin is
parked before it becomes an output so the relay doesn't click on reset. The ESP-NOW
failsafe opens it too.

Free GPIO left on this node: 1, 2, 6, 8, 9, 12, 14, 39, 47 (15 is the `MOTOR_STBY_PIN`
fallback). Never use 0/3/45/46 — strapping pins sit at the wrong level during boot.

---

### `src/config.h` + `src/config.cpp` — controller hardware

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `I2C_SDA` / `I2C_SCL` | 17 / 18 | LCD I2C |
| `PCA_A_ADDR` / `PCA_B_ADDR` | 0x40 / 0x60 | PCA9685 LED drivers |
| `LED_INVERT` | `true` | Common-anode LEDs |
| `GAMMA` | `2.2` | LED brightness curve |
| `PCA_FREQ` | `1000` Hz | PWM frequency |
| `FRONT_LED_BRIGHTNESS_SCALE` | `0.40` | Dims GPIO 16 & 46 button LEDs only |
| `BTN_PINS[10]` | 38,12,5,7,16,46,39,2,15,8 | Physical GPIO map |
| `I2S_BCLK/LRCK/DATA` | 13 / 6 / 4 | UDA1334A DAC pins |
| `SR`, `MASTER_VOL`, `GAIN_L/R`, etc. | see file | Synth sample rate, volume, envelope, vibrato |

---

### `src/leds.cpp` — controller LED & idle timing (edit source)

| Setting | Default | What it changes |
|---------|---------|-----------------|
| `IDLE_AFTER_MS` | `10000` | No buttons this long → **synth idle** (flame animation, idle DMX, bubble kill). |
| `PRESS_FADE_MS` | `1000` | Release tail fade on button LEDs |
| `IDLE_V_MIN` / `IDLE_V_MAX` | 0.08 / 0.28 | Idle breathe brightness range |
| `IDLE_LFO_HZ` | ~0.31 | Idle breathe speed |
| `FLAME_UPDATE_MS` | `75` | Warm/cool flame step rate |
| `idleMode` | `1` | Starting idle animation (0–11). Auto-cycles every 2 min in `main.cpp`. |
| `PRESS_V` | `1.0` | Brightness when button pressed |
| `RAINBOW_HZ` | `0.2` | Front-button rainbow sweep (5 s) |

**Hardcoded in `main.cpp` (not `#define`):**

| Behavior | Value |
|----------|-------|
| Idle mode auto-advance | Every **120 s** while idle |
| PYRRISMA welcome after idle | Idle ≥ **25 s** before exit |
| Scale change (16+46 hold) | **1.5 s** |
| Waveform preview (green hold) | **1.5 s** |
| Pitch shift repeat (`PITCH_SHIFT_INTERVAL_MS`) | **122 ms** |
| Pitch offset range (`leftDegOff` / `rightDegOff`) | **±24**, and stops early at the Hz ceiling |

**Pitch ceiling.** `src/scales.cpp` clamps output to `kMaxSynthHz` (≈748 Hz — `220 × 2²
× 0.85`) so pitch-up can't get shrill. That clamps the *frequency*; nothing used to clamp
the *offset*, so past the ceiling `leftDegOff`/`rightDegOff` kept counting to +24 while the
pitch stayed put — and shifting back down did nothing audible until you'd pressed down
enough times to get back under the clamp. It read as "stuck on octave 24, can't tune out."

`scaleIdxAtCeiling()` now stops the offset climbing once the note actually being played
hits the ceiling, so the offset can never strand above the audible range and down-shift
always responds on the first press. Down-shift logic is unchanged.

---

### `include/rhythm_sd_pins.h` — rhythm game SD card (controller)

Override in `[env:esp32-s3-devkitc-1]` `build_flags` if using T-ETH-Lite pins.

| Setting | Default (Elite) | What it changes |
|---------|-----------------|-----------------|
| `RHYTHM_SD_CS_PIN` | `12` | SD chip select (hardwired in the SD module — not reassignable) |
| `RHYTHM_SD_SCK_PIN` | 10 | SPI clock |
| `RHYTHM_SD_MISO_PIN` | 9 | SPI MISO |
| `RHYTHM_SD_MOSI_PIN` | 11 | SPI MOSI |
| `RHYTHM_ENABLE_SD` | `1` | `0` = no SD / rhythm from flash only |
| `RHYTHM_SD_SPI_HZ` | 20 MHz | SD SPI speed |

> **Rewired: the left blue button's switch lead moved from GPIO 12 to GPIO 42.**
> Until that wire moves, the SD card will not mount and song mode has no audio.

**Why it moved.** GPIO 12 was both SD CS *and* `BTN_PINS[1]` = `IDX_LEFT[1]`, the
left blue key. SPI drives CS push-pull, so with a song loaded a press could not pull
the line down, and reads returned chip-select traffic — **phantom blue presses firing
the relay and blue DMX at random during songs**. `pinOwnedBySd()` in `buttons.cpp`
masks any pin the SD peripheral owns (CS/SCK/MISO/MOSI, not just MISO as before),
which stops the phantoms, but masking alone left the key dead during song mode.

**The key moved, not CS** — GPIO 12 is hardwired to chip select inside the SD module
and cannot be reassigned, so the button's switch lead was the only movable end.

**Why 42:** on the actual breakout, the only terminals that can be grounded without
crashing the board are GPIO **9, 10, 40, 41, 42** — and 9/10/12 are already SD
MISO/SCK/CS. All of 40/41/42 are full input+output with internal pull-ups on ESP32-S3
(input-only 34–39 is an *ESP32-classic* property, not S3). They are JTAG
MTDO/MTDI/MTMS, but JTAG is already gone because MTCK (GPIO 39) is a key. GPIO 14 is
*not* an alternative — it is the W5500 INT output — and GPIO 21 is not usably broken
out. See `I2S_PIN_MAPPING.md`.

Three places encode this pin, all updated together — `BTN_PINS[1]` in `config.cpp`,
the `MAP[1]` LED channel row, and the GPIO-keyed colour switch in
`getPressColorForGPIO()` (`leds.cpp`). Miss that last one and the key reads correctly
but its LED stays dark.

---

### `src/rhythm_game.cpp` — rhythm game timing (edit source)

| Constant | Default | What it changes |
|----------|---------|-----------------|
| `RG_ENTER_HOLD_MS` | 10000 | All 10 buttons held together for 10 s → enter rhythm mode. The `s_eightHoldStart` variable name is historic. |
| `RG_EXIT_HOLD_MENU_MS` | 3000 | Bottom pair hold → song menu |
| `RG_EXIT_HOLD_IDLE_MS` | 4000 | Bottom pair hold → exit to synth idle |
| `RG_UI_IDLE_TO_SYNTH_MS` | 25000 | Menu/results AFK → synth idle |
| `RG_PLAY_AFK_AFTER_MS` | 15000 | Playing: no input → AFK prompt |
| `RG_PLAY_AFK_PROMPT_MS` | 10000 | AFK prompt duration |
| `RG_AFK_FADE_OUT_MS` | 3000 | Fade music before leaving play |
| `RG_PAUSE_HOLD_MS` | 3000 | Pause/resume toggle hold |
| `kLiveFeedbackWindowMs` | 4000 | Rolling score window for live % |

---

### Pattern mode (`src/pattern_mode.cpp`)

Hold both yellow buttons for 5 seconds to enter the randomized side-button pattern
game. Hold them again for 5 seconds to exit after releasing the entry gesture.
Pattern, Simon, and rhythm modes are mutually exclusive.

---

### `platformio.ini` — build & upload

| Setting | Where | What it changes |
|---------|-------|-----------------|
| `upload_port` | per env | USB COM port |
| `build_src_filter` | per env | Controller excludes `solenoid_node/`; actuator includes only actuator sources |
| `-DMOTOR_STBY_PIN=13` | solenoid-node | Motor standby GPIO |
| `-DRELAY_PIN=16` | solenoid-node | 5V relay GPIO (any color hold) |
| `-DRELAY_ACTIVE_LOW=0` | solenoid-node | Drive HIGH to close — matches module jumper on `H` |
| `-DRELAY_OPEN_DRAIN=0` | solenoid-node | Push-pull; keeps 5V off the GPIO |
| `-DCORE_DEBUG_LEVEL=3` | both | ESP-IDF log verbosity |
| `-DARDUINO_USB_CDC_ON_BOOT=1` | both | USB serial on boot |

---

## Runtime behavior (driven by settings above)

### Side color buttons (controller → actuator)

- **Hold** → solenoid (except blue), relay (all colors), motor (red/green/yellow), PAR live, bubble RGB live with pulse
- **Release** → PAR off; bubble **latches** last color combo
- **RGB sync** sends combined color levels so bubble mixes like PAR

### Bubble fan

- Hold any button **2 s** → fan burst **2 s** (one shot until all buttons released)
- Fan ramps **10 → 255** over 1.5 s, down over last 0.5 s
- **Synth idle** → `BUBBLE_KILL` + PAR/bubble idle animation; fan off
- **Boot** → 8× zero DMX frames, fan explicitly 0

### Safety

- Actuator boot: solenoids off, relay open, DMX safe, motors stopped
- ESP-NOW failsafe: solenoid or relay stuck ON + 3 s silence → all off
- Idle enter: controller sends OFF for all colors + bubble kill

---

## Legacy / unused

| File | Notes |
|------|-------|
| `src/mqtt_config.h` | Old MQTT broker settings; replaced by ESP-NOW |
| `include/mqtt_protocol.h` | Legacy MQTT topics |

---

## Quick troubleshooting

| Symptom | Check |
|---------|-------|
| Actuator dead | Same `ESPNOW_WIFI_CHANNEL`, both on protocol v5, serial `[ESPNOW] rx` on actuator |
| Bubble spins on color press | Bubble menu address **007**, not 001/512 |
| PAR dies while holding | Flash actuator (needs `DMX_REFRESH_HZ` continuous output) |
| Fan hammers PSU | Lower `DMX_BUBBLE_FAN_MAX`, lengthen `BUBBLE_FAN_RAMP_UP_MS`, or factory **P000** pump speed |
| Solenoid stuck open | Watch for `[SAFE] ESP-NOW timeout`; release button (sync sends OFF) |

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
| `ACTUATOR_PROTO_VERSION` | `3` | Must match on both boards. Bump + reflash both when changing packet layout. |
| `ACTUATOR_ON_*` | — | Command types: color ON/OFF, bubble party, bubble kill, idle DMX, idle end, RGB hold. |

Color mapping (side buttons → actuator):

| Button column | `ActuatorColor` | Solenoid MCP ch | Motor |
|---------------|-----------------|-----------------|-------|
| Yellow (top) | `YELLOW` | 3 | — |
| Blue | `BLUE` | 2 | — (**solenoid disabled in firmware**) |
| Green | `GREEN` | 1 | green driver |
| Red (bottom) | `RED` | 0 | red driver |

Yellow also runs the yellow motor driver.

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
| MCP ch 0–3 | — | red, green, blue, yellow solenoids |

Boot: all solenoids driven **LOW**. Blue solenoid is **not fired** (hardcoded in `main.cpp`).

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

---

### `include/rhythm_sd_pins.h` — rhythm game SD card (controller)

Override in `[env:esp32-s3-devkitc-1]` `build_flags` if using T-ETH-Lite pins.

| Setting | Default (Elite) | What it changes |
|---------|-----------------|-----------------|
| `RHYTHM_SD_CS_PIN` | 12 | SD chip select |
| `RHYTHM_SD_SCK_PIN` | 10 | SPI clock |
| `RHYTHM_SD_MISO_PIN` | 9 | SPI MISO |
| `RHYTHM_SD_MOSI_PIN` | 11 | SPI MOSI |
| `RHYTHM_ENABLE_SD` | `1` | `0` = no SD / rhythm from flash only |
| `RHYTHM_SD_SPI_HZ` | 20 MHz | SD SPI speed |

---

### `src/rhythm_game.cpp` — rhythm game timing (edit source)

| Constant | Default | What it changes |
|----------|---------|-----------------|
| `RG_ENTER_HOLD_MS` | 10000 | Both yellow (GPIO 38+39) held → enter rhythm mode |
| `RG_EXIT_HOLD_MENU_MS` | 3000 | Bottom pair hold → song menu |
| `RG_EXIT_HOLD_IDLE_MS` | 4000 | Bottom pair hold → exit to synth idle |
| `RG_UI_IDLE_TO_SYNTH_MS` | 25000 | Menu/results AFK → synth idle |
| `RG_PLAY_AFK_AFTER_MS` | 15000 | Playing: no input → AFK prompt |
| `RG_PLAY_AFK_PROMPT_MS` | 10000 | AFK prompt duration |
| `RG_AFK_FADE_OUT_MS` | 3000 | Fade music before leaving play |
| `RG_PAUSE_HOLD_MS` | 3000 | Pause/resume toggle hold |
| `kLiveFeedbackWindowMs` | 4000 | Rolling score window for live % |

---

### `platformio.ini` — build & upload

| Setting | Where | What it changes |
|---------|-------|-----------------|
| `upload_port` | per env | USB COM port |
| `build_src_filter` | per env | Controller excludes `solenoid_node/`; actuator includes only actuator sources |
| `-DMOTOR_STBY_PIN=13` | solenoid-node | Motor standby GPIO |
| `-DCORE_DEBUG_LEVEL=3` | both | ESP-IDF log verbosity |
| `-DARDUINO_USB_CDC_ON_BOOT=1` | both | USB serial on boot |

---

## Runtime behavior (driven by settings above)

### Side color buttons (controller → actuator)

- **Hold** → solenoid (except blue), motor (red/green/yellow), PAR live, bubble RGB live with pulse
- **Release** → PAR off; bubble **latches** last color combo
- **RGB sync** sends combined color levels so bubble mixes like PAR

### Bubble fan

- Hold any button **2 s** → fan burst **2 s** (one shot until all buttons released)
- Fan ramps **10 → 255** over 1.5 s, down over last 0.5 s
- **Synth idle** → `BUBBLE_KILL` + PAR/bubble idle animation; fan off
- **Boot** → 8× zero DMX frames, fan explicitly 0

### Safety

- Actuator boot: solenoids off, DMX safe, motors stopped
- ESP-NOW failsafe: solenoid stuck ON + 3 s silence → all off
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
| Actuator dead | Same `ESPNOW_WIFI_CHANNEL`, both on protocol v3, serial `[ESPNOW] rx` on actuator |
| Bubble spins on color press | Bubble menu address **007**, not 001/512 |
| PAR dies while holding | Flash actuator (needs `DMX_REFRESH_HZ` continuous output) |
| Fan hammers PSU | Lower `DMX_BUBBLE_FAN_MAX`, lengthen `BUBBLE_FAN_RAMP_UP_MS`, or factory **P000** pump speed |
| Solenoid stuck open | Watch for `[SAFE] ESP-NOW timeout`; release button (sync sends OFF) |

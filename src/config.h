#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ===================== I2C & LCD =====================
#define I2C_SDA 17
#define I2C_SCL 18
const int LCD_COLS=20, LCD_ROWS=4;

// ===================== PCA9685 =======================
#define PCA_A_ADDR 0x40  // driver 0
#define PCA_B_ADDR 0x60  // driver 1

static const bool  LED_INVERT = true;   // common-anode
static const float GAMMA      = 2.2f;
static const uint16_t PCA_FREQ = 1000;  // Hz

struct LedCh { uint8_t drv; uint8_t ch; };
struct RGBMap { LedCh r,g,b; };

// ======= HARD LED MAPPING (from probe) =======
extern RGBMap MAP[10];

// ===================== Buttons =======================
extern const int BTN_PINS[10];
extern const int IDX_LEFT[4];
extern const int IDX_RIGHT[4];
#define IDX_FRONT_L 4            // 16
#define IDX_FRONT_R 5            // 46
// Multiply RGB for 16/46 only (all modes: idle, rhythm, pressed).
static constexpr float FRONT_LED_BRIGHTNESS_SCALE = 0.40f;
// Extra cut applied to the front keys (GPIO 16 / 46) *only in song mode*, where they
// sit right under the player's eyes and read as blinding. Multiplies the above, so
// song mode ends up at 0.40 x 0.80 = 0.32.
// Was 0.80; a further 15% cut after seeing them on the piece. Multiplies the base
// scale, so song mode is 0.40 x 0.68 = 0.272 vs 0.40 elsewhere.
static constexpr float FRONT_LED_SONG_MODE_SCALE = 0.68f;
#define IDX_38 0
#define IDX_11 6

// ===================== I2S Synth =====================
// Using safe GPIOs (avoiding SPI flash/PSRAM range 26-34)
// Safe GPIOs available: 4, 6, 10, 13, 14, 15, 21
// Configuration: GPIO 4 (DIN), GPIO 13 (BCLK), GPIO 6 (WSEL/LRCLK)
// NOTE: GPIO 15 avoided - has boot mode implications and may not work reliably for I2S on ESP32-S3
#define I2S_BCLK 13   // Bit Clock - GPIO 13
#define I2S_LRCK 6    // Left/Right Clock (WSEL) - GPIO 6
#define I2S_DATA 4    // Serial Data (DIN) - GPIO 4
#define SWAP_I2S_LR 0

const int   SR            = 22050;
const size_t BUF_SAMPLES  = 512;
const int   DMA_COUNT     = 16;
// Synth master only — audio.cpp applies it to the oscillators. The rhythm-game MP3
// path does not go through it, so this does not change song-mode playback volume.
// Cut 30% from the previous 0.85.
const float MASTER_VOL    = 0.595f;
const float GAIN_L        = 0.70f;
const float GAIN_R        = 0.70f;
const float AMP_ATTACK    = 0.010f;
const float AMP_RELEASE   = 0.008f;
const float VIB_DEPTH_SEMITONES = 0.12f;
const float VIB_RATE_HZ   = 3.2f;
const float FREQ_GLIDE_TAU_S = 0.040f;

#endif // CONFIG_H


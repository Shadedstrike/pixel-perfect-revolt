# I2S Pin Mapping: LILYGO T-ETH Elite → Raspberry Pi Breakout → ESP32-S3 GPIO

## Currently Used GPIOs
- **I2C**: GPIO 17 (SDA), GPIO 18 (SCL)  
- **Buttons**: GPIO 2, 5, 7, 8, 9, 11, 12, 16, 38, 46
- **Avoid**: GPIO 0, 1, 3, 19-20, 22, 23, 25, 26-34, 35-42, 43-48 (strapping/USB/SPI flash/input-only)

## Available Safe GPIOs for I2S
✅ **GPIO 4, 6, 10, 13, 14, 15, 21**

## Recommended I2S Configuration (Already Set in Code)
| I2S Signal | ESP32-S3 GPIO | Breakout Terminal to Find | Purpose |
|------------|---------------|---------------------------|---------|
| **I2S_DATA** (PCMdin) | **GPIO 13** | **Use pin ID function** | Serial data to DAC |
| **I2S_LRCK** (PCMfs) | **GPIO 15** | **Use pin ID function** | Left/Right clock |
| **I2S_BCLK** | **GPIO 14** | **Use pin ID function** | Bit clock |

## Raspberry Pi Breakout Board Mapping

**⚠️ IMPORTANT**: The breakout board labels are **Raspberry Pi GPIO numbers/functions**, NOT ESP32-S3 GPIO numbers! The actual ESP32-S3 GPIO mapping may be different. You need to use the pin identification function to find which ESP32-S3 GPIO each terminal connects to.

### Left Side Terminals (Raspberry Pi Function Labels)
- **Terminal A**: PCMdout (Raspberry Pi function - find ESP32-S3 GPIO via pin ID)
- **Terminal B**: PCMdin (Raspberry Pi function - find ESP32-S3 GPIO via pin ID) ⚠️ Crashes!
- **Terminal C**: PCMfs (Raspberry Pi function - find ESP32-S3 GPIO via pin ID) ⚠️ Crashes!
- **Terminal D**: PWM1 (Raspberry Pi function - find ESP32-S3 GPIO via pin ID)
- **Terminal E**: PWM0 (Raspberry Pi function - find ESP32-S3 GPIO via pin ID)

**Note**: Terminal D maps to PCMdout (you discovered this). Use pin identification to find which ESP32-S3 GPIO it actually connects to.

### Right Side Terminals (Raspberry Pi GPIO Numbers)
From top (T) to bottom (A):
- **Terminal T**: +5V
- **Terminal S**: GND
- **Terminal R**: 3.3V
- **Terminal Q**: GPIO18 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal P**: GPIO23 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal O**: GPIO24 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal N**: GPIO25 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID) ⚠️ Crashes!
- **Terminal M**: GPIO16 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal L**: GPIO26 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal K**: GPIO06 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal J**: GPIO05 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal I**: GPIO17 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal H**: ECLK
- **Terminal G**: GPIO27 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal F**: EDAT
- **Terminal E**: GPIO22 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal D**: GPIO04 (Raspberry Pi GPIO - find ESP32-S3 GPIO via pin ID)
- **Terminal C**: 3.3V
- **Terminal B**: GND
- **Terminal A**: +5V

**⚠️ CRITICAL**: The labels are Raspberry Pi references. The actual ESP32-S3 GPIO mapping must be discovered using the pin identification function!

## How to Find ESP32-S3 GPIO Mapping for Each Terminal

**The breakout board labels are Raspberry Pi references - you need to find the actual ESP32-S3 GPIO mapping!**

1. **Run pin identification**: The code already has `identifyBreakoutPins()` enabled
2. **Touch terminals to GND**: Systematically touch each terminal (both left and right sides) to GND
3. **Check serial output**: It will show ">>> GPIO X detected!" when you touch the terminal
4. **Map the results**: Write down which terminal letter corresponds to which ESP32-S3 GPIO

**Example**: If you touch terminal D and it shows ">>> GPIO 13 detected!", then terminal D maps to ESP32-S3 GPIO 13.

**Goal**: Find terminals that map to safe ESP32-S3 GPIOs: **4, 6, 10, 13, 14, 15, 21**

## Finding Safe Terminals for I2S

Since terminal D maps to PCMdout (Raspberry Pi function), use pin identification to find which ESP32-S3 GPIO it connects to. Then find terminals for GPIO 13, 14, 15:

### Step 1: Identify Terminal D's ESP32-S3 GPIO
- Touch terminal D (PCMdout) to GND
- Check serial output: ">>> GPIO X detected!"
- If it's GPIO 13, 14, 15, or another safe GPIO (4, 6, 10, 21), you can use it!

### Step 2: Find Terminals for GPIO 13, 14, 15
| I2S Signal | ESP32-S3 GPIO | Breakout Terminal | How to Find |
|------------|---------------|-------------------|-------------|
| I2S_DATA | GPIO 13 | **Find via pin ID** | Touch terminals until you see "GPIO 13 detected!" |
| I2S_LRCK | GPIO 15 | **Find via pin ID** | Touch terminals until you see "GPIO 15 detected!" |
| I2S_BCLK | GPIO 14 | **Find via pin ID** | Touch terminals until you see "GPIO 14 detected!" |

**Alternative**: If terminal D maps to a safe GPIO (like GPIO 13), you can use it! Then find two more terminals for GPIO 14 and 15.

## Purple LILYGO Clone DAC Connections

For your purple LILYGO clone DAC (UDA1334A or similar):

| DAC Pin | Connect To | ESP32-S3 GPIO | Breakout Terminal |
|---------|------------|---------------|-------------------|
| **BCLK** | I2S_BCLK | GPIO 14 | Find via pin ID |
| **LRCLK/WS** | I2S_LRCK | GPIO 15 | Find via pin ID |
| **DIN** | I2S_DATA | GPIO 13 | Find via pin ID |
| **GND** | GND | - | Any GND terminal |
| **VCC** | 3.3V or 5V | - | 3.3V or +5V terminal |

## Next Steps

1. ✅ Code is already configured for GPIO 13, 14, 15
2. 🔍 Run pin identification to find which terminals map to GPIO 13, 14, 15
3. 🔌 Connect your DAC to those terminals (NOT the left side PCMfs/PCMdin terminals!)
4. ✅ Test I2S audio output


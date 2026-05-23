#include "display.h"
#include "audio.h"
#include "scales.h"
#include <math.h>
#include <string.h>

hd44780_I2Cexp lcd;

void lcdPrintStatus(const char* scaleName, float fL, float fR, int8_t offL, int8_t offR){
    lcd.backlight();
    lcd.clear();
  // L1: Scale
  lcd.setCursor(0,0); lcd.print("Scale: ");
  lcd.print(scaleName);
  // L2: Left note
  lcd.setCursor(0,1); lcd.print("L "); lcd.print(noteNameFromHz(fL)); lcd.print(" ");
  lcd.print((int)offL>=0?"+":""); lcd.print(offL);
  // L3: Right note
  lcd.setCursor(0,2); lcd.print("R "); lcd.print(noteNameFromHz(fR)); lcd.print(" ");
  lcd.print((int)offR>=0?"+":""); lcd.print(offR);
  // L4: waveform + hints (20 cols): green 5+9 hold ~1.5s cycles wave; 16/46 = pitch/scale
  lcd.setCursor(0, 3);
  lcd.printf("W:%-4s 5+9h 16/46 Wv", audioWaveShapeName(audioGetWaveShape()));
}

void lcdPrintWaveShapePreview(AudioWaveShape shape) {
  const float TAU = 6.28318530718f;
  char title[21];
  snprintf(title, sizeof(title), "WAVE: %-14s", audioWaveShapeName(shape));

  char g[3][21];
  for (int r = 0; r < 3; r++) {
    memset(g[r], ' ', 20);
    g[r][20] = '\0';
  }

  for (int x = 0; x < 20; x++) {
    float ph = TAU * (float)x / 20.f;
    float v = audioWaveShapeSample(shape, ph);
    if (v > 1.f)
      v = 1.f;
    if (v < -1.f)
      v = -1.f;
    int row = (int)roundf(1.f - v);
    if (row < 0)
      row = 0;
    if (row > 2)
      row = 2;
    g[row][x] = '*';
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(title);
  lcd.setCursor(0, 1);
  lcd.print(g[0]);
  lcd.setCursor(0, 2);
  lcd.print(g[1]);
  lcd.setCursor(0, 3);
  lcd.print(g[2]);
}

// Display scale selection with 3 nearby scales and sparkles around selected
void lcdPrintScaleSelection(uint8_t currentScaleIndex, uint32_t now){
  extern ScaleDef SCALES[];
  extern const uint8_t NUM_SCALES;
  
  lcd.clear();
  
  // Calculate previous, current, and next scale indices
  int prevIdx = (currentScaleIndex == 0) ? -1 : (currentScaleIndex - 1);
  int nextIdx = (currentScaleIndex == NUM_SCALES - 1) ? -1 : (currentScaleIndex + 1);
  
  // Line 1: Previous scale (if exists) - right aligned or centered
  if(prevIdx >= 0){
    lcd.setCursor(0, 0);
    lcd.print("< ");
    lcd.print(SCALES[prevIdx].name);
  }
  
  // Line 2: Current scale (selected) - centered with sparkles
  lcd.setCursor(0, 1);
  // Center the scale name (20 chars wide, scale names are typically 10-20 chars)
  const char* currentName = SCALES[currentScaleIndex].name;
  int nameLen = strlen(currentName);
  int startPos = (20 - nameLen) / 2; // Center it
  if(startPos < 0) startPos = 0;
  
  // Add sparkles around the name
  if(startPos > 0){
    lcd.setCursor(startPos - 1, 1);
    lcd.print("*");
  }
  lcd.setCursor(startPos, 1);
  lcd.print(currentName);
  if(startPos + nameLen < 19){
    lcd.setCursor(startPos + nameLen, 1);
    lcd.print("*");
  }
  
  // Line 3: Next scale (if exists)
  if(nextIdx >= 0){
    lcd.setCursor(0, 2);
    lcd.print(SCALES[nextIdx].name);
    lcd.print(" >");
  }
  
  // Line 4: Add ambient sparkles around the selected scale name
  // Use a simple sparkle pattern that updates over time
  static uint32_t lastSparkleUpdate = 0;
  static uint8_t sparkleChars[20] = {0}; // Track sparkle positions on line 1
  
  if(now - lastSparkleUpdate > 150){ // Update sparkles every 150ms
    lastSparkleUpdate = now;
    
    // Clear sparkle line (line 3, but we'll use line 0 or 2 for sparkles)
    // Actually, let's add sparkles on line 0 and 2 around the current scale
    // Clear and redraw sparkles on lines 0 and 2
    
    // Line 0: Sparkles above current scale (only if no previous scale to avoid overwriting)
    if(prevIdx < 0){
      for(int i = 0; i < 20; i++){
        if(i >= startPos - 2 && i <= startPos + nameLen + 1){
          if(random(0, 100) < 15){ // 15% chance per position
            lcd.setCursor(i, 0);
            const char* sparkleSymbols = ".*+o";
            lcd.print(sparkleSymbols[random(0, 4)]);
          } else {
            lcd.setCursor(i, 0);
            lcd.print(" ");
          }
        }
      }
    }
    
    // Line 3: Sparkles below current scale (line 3 is safe - either empty or has next scale on right)
    if(nextIdx < 0){
      // No next scale, add sparkles on line 3
      for(int i = 0; i < 20; i++){
        if(i >= startPos - 2 && i <= startPos + nameLen + 1){
          if(random(0, 100) < 15){
            lcd.setCursor(i, 3);
            const char* sparkleSymbols = ".*+o";
            lcd.print(sparkleSymbols[random(0, 4)]);
          } else {
            lcd.setCursor(i, 3);
            lcd.print(" ");
          }
        }
      }
    } else {
      // Next scale exists, add sparkles on line 3 in empty area (right side, away from scale name)
      for(int i = startPos + nameLen + 3; i < 20; i++){
        if(random(0, 100) < 10){ // Lower chance
          lcd.setCursor(i, 3);
          const char* sparkleSymbols = ".*+";
          lcd.print(sparkleSymbols[random(0, 3)]);
        }
      }
    }
  }
}

// ===================== LCD Animations =====================

// Sparkle animation - random sparkles appearing and fading
static uint32_t sparklePositions[10] = {0}; // x,y encoded: x*100 + y
static uint32_t sparkleSpawnTimes[10] = {0}; // when sparkle was spawned
static uint32_t lastSparkleSpawn = 0;

void lcdSparkleAnimation(uint32_t now){
  const uint32_t SPAWN_INTERVAL = 200; // spawn new sparkle every 200ms
  const uint32_t MAX_LIFETIME = 800; // sparkle lasts 800ms
  
  // Spawn new sparkles
  if(now - lastSparkleSpawn > SPAWN_INTERVAL){
    lastSparkleSpawn = now;
    // Find empty slot
    for(int i=0; i<10; i++){
      if(sparkleSpawnTimes[i] == 0 || (now - sparkleSpawnTimes[i] > MAX_LIFETIME)){
        sparklePositions[i] = random(0, 20) * 100 + random(0, 4); // x*100 + y
        sparkleSpawnTimes[i] = now;
        break;
      }
    }
  }
  
  // Clear screen and draw sparkles
  lcd.clear();
  for(int i=0; i<10; i++){
    if(sparkleSpawnTimes[i] > 0){
      uint32_t age = now - sparkleSpawnTimes[i];
      if(age > MAX_LIFETIME){
        sparkleSpawnTimes[i] = 0; // Expired
        continue;
      }
      
      int x = sparklePositions[i] / 100;
      int y = sparklePositions[i] % 100;
      float ageRatio = (float)age / MAX_LIFETIME;
      
      // Choose character based on age - more variety with numbers and symbols
      char c;
      if(ageRatio < 0.3f){
        // Small sparkle - use dots, commas, or small numbers
        char small[] = {'.', ',', ':', ';', '0', '1', '2'};
        c = small[random(0, sizeof(small)/sizeof(small[0]))];
      } else if(ageRatio < 0.6f){
        // Medium sparkle - use stars, plus, or numbers
        char medium[] = {'*', '+', 'x', 'X', '3', '4', '5', '6', '7'};
        c = medium[random(0, sizeof(medium)/sizeof(medium[0]))];
      } else if(ageRatio < 0.8f){
        // Large sparkle - use circles or larger numbers
        char large[] = {'o', 'O', '0', '8', '9', '@', '#', '%'};
        c = large[random(0, sizeof(large)/sizeof(large[0]))];
      } else {
        // Fading sparkle - use dim symbols
        char fading[] = {'o', '.', '-', '_', '~'};
        c = fading[random(0, sizeof(fading)/sizeof(fading[0]))];
      }
      
      lcd.setCursor(x, y);
      lcd.print(c);
    }
  }
}

// Glitchy Chinese/Japanese text animation - matrix-style with zalgo effects
static uint32_t glitchFrame = 0;
static uint32_t lastGlitchUpdate = 0;
static uint32_t glitchMode = 0; // 0=top-down, 1=left-right
static int glitchPosition = 0; // Current position in animation
static bool glitchActive = false;

// Multi Spectral Console text variations
const char* consoleTexts[] = {
  "MULTI", "SPECTRAL", "CONSOLE",
  "multi", "spectral", "console",
  "MULT1", "SP3CTR4L", "C0NS0L3",
  "mult1", "sp3ctr4l", "c0ns0l3",
  "MuLtI", "SpEcTrAl", "CoNsOlE",
  "MULTI SPECTRAL", "SPECTRAL CONSOLE", "MULTI CONSOLE",
  "multi spectral", "spectral console", "multi console",
  "MULT1 SP3CTR4L", "SP3CTR4L C0NS0L3", "MULT1 C0NS0L3"
};
const int NUM_CONSOLE_TEXTS = sizeof(consoleTexts) / sizeof(consoleTexts[0]);

// Elements + propane only (name, symbol, color, emission wavelength in nm)
struct ElementGroup {
  const char* name;
  const char* symbol;
  const char* color;
  const char* wavelength; // Emission wavelength in nm
};

const ElementGroup elements[] = {
  {"Sodium", "Na", "Yellow", "589 nm"},
  {"Strontium", "Sr", "Red", "606.8 nm"},
  {"Boron", "B", "Green", "518 nm"},
  {"Propane", "C3H8", "Blue", "470 nm"}
};
const int NUM_ELEMENTS = sizeof(elements) / sizeof(elements[0]);

// Shared flag to prevent console text and element groups from displaying simultaneously
static bool elementGroupsActive = false;
static bool consoleTextActiveGlobal = false;

// Helper function to draw console text in sequence: Multi -> Spectral -> Console
void drawConsoleText(uint32_t now, uint32_t idleStartTime) {
  static uint32_t lastConsoleText = 0;
  static uint32_t consoleTextStartTime = 0;
  static bool consoleTextActive = false;
  static int currentWord = 0; // 0=Multi, 1=Spectral, 2=Console
  static int posX = 0, posY = 0;
  static int textStyle = 0; // 0=normal, 1=lowercase, 2=leetspeak
  
  // Check if element groups are active - if so, don't show console text
  if(elementGroupsActive) {
    consoleTextActive = false; // Reset if element groups take over
    consoleTextActiveGlobal = false;
    return;
  }
  
  // Calculate time since idle started
  uint32_t idleElapsed = (idleStartTime > 0) ? (now - idleStartTime) : 0;
  
  // 25% chance to start showing console text sequence
  if(!consoleTextActive && idleElapsed > 1000 && random(0, 100) < 25) {
    consoleTextActive = true;
    consoleTextStartTime = now;
    currentWord = 0;
    textStyle = random(0, 3); // Random style
    posX = random(0, 12); // Leave room for text
    posY = random(0, 4);
  }
  
  // Update global flag
  consoleTextActiveGlobal = consoleTextActive;
  
  if(consoleTextActive) {
    uint32_t sequenceElapsed = now - consoleTextStartTime;
    const uint32_t WORD_DURATION_MS = 2000; // Each word shows for 2 seconds
    const uint32_t FADE_DURATION_MS = 500; // Fade in/out duration
    
    // Determine which word to show based on elapsed time
    int wordToShow = (sequenceElapsed / WORD_DURATION_MS) % 3;
    
    // Calculate fade progress for current word
    uint32_t wordStartTime = wordToShow * WORD_DURATION_MS;
    uint32_t wordElapsed = sequenceElapsed - wordStartTime;
    float fadeProgress = 0.0f;
    
    if(wordElapsed < FADE_DURATION_MS) {
      // Fade in
      fadeProgress = (float)wordElapsed / FADE_DURATION_MS;
    } else if(wordElapsed < (WORD_DURATION_MS - FADE_DURATION_MS)) {
      // Full display
      fadeProgress = 1.0f;
    } else {
      // Fade out
      fadeProgress = 1.0f - ((float)(wordElapsed - (WORD_DURATION_MS - FADE_DURATION_MS)) / FADE_DURATION_MS);
    }
    
    // Get word text based on style
    const char* wordText = "";
    if(wordToShow == 0) {
      if(textStyle == 0) wordText = "MULTI";
      else if(textStyle == 1) wordText = "multi";
      else wordText = "MULT1";
    } else if(wordToShow == 1) {
      if(textStyle == 0) wordText = "SPECTRAL";
      else if(textStyle == 1) wordText = "spectral";
      else wordText = "SP3CTR4L";
    } else {
      if(textStyle == 0) wordText = "CONSOLE";
      else if(textStyle == 1) wordText = "console";
      else wordText = "C0NS0L3";
    }
    
    // Apply leetspeak swaps during display
    if(textStyle == 2 && fadeProgress > 0.3f) {
      // Apply random leetspeak swaps
      uint32_t swapSeed = (sequenceElapsed / 100);
      randomSeed(swapSeed);
      if(random(0, 100) < 30) {
        // Apply swaps to wordText (we'll handle this in display)
      }
    }
    
    // Display word with fade effect
    if(fadeProgress > 0.1f) {
      lcd.setCursor(posX, posY);
      // Show dots/spaces during fade, then actual text
      if(fadeProgress < 0.5f && random(0, 100) < 50) {
        lcd.print('.');
      } else {
        lcd.print(wordText);
      }
    }
    
    // Reset after all three words have been shown
    if(sequenceElapsed >= (WORD_DURATION_MS * 3)) {
      consoleTextActive = false;
      consoleTextActiveGlobal = false;
      lastConsoleText = now;
    }
  }
}

// Helper function to draw chemical element groups that fade in/out together
void drawElementGroups(uint32_t now) {
  static uint32_t lastElementText = 0;
  static uint32_t elementTextDuration = 0;
  static bool elementTextActive = false;
  static int elementIndex = 0;
  static int posX = 0, posY = 0;
  static float fadeProgress = 0.0f;
  
  // Check if console text is active - if so, don't show element groups
  if(consoleTextActiveGlobal) {
    elementTextActive = false; // Reset if console text takes over
    elementGroupsActive = false;
    return;
  }
  
  uint32_t elapsed = now - lastElementText;
  const uint32_t FADE_IN_MS = 1500; // Increased fade in duration
  const uint32_t FADE_OUT_MS = 1500; // Increased fade out duration
  
  // Element-specific display durations (all elements use same fade in/out, but different display times)
  // Boron gets slightly longer display than Sodium / Strontium / Propane.
  const uint32_t DISPLAY_MS_BASE = 2250; // Base display time (Sodium, Strontium, Propane)
  const uint32_t DISPLAY_MS_BORON = 2450; // Boron gets 0.2s more (200ms)
  
  // 10% chance to show element group (reduced from 20% - 50% reduction)
  if(!elementTextActive && elapsed > 1000 && random(0, 100) < 10) {
    elementTextActive = true;
    elementGroupsActive = true;
    lastElementText = now;
    elementTextDuration = 0;
    elementIndex = random(0, NUM_ELEMENTS);
    posX = random(0, 8); // Leave room for text
    posY = random(0, 2); // Leave room for 2-3 lines
    fadeProgress = 0.0f;
  }
  
  // Update global flag
  elementGroupsActive = elementTextActive;
  
  if(elementTextActive) {
    elementTextDuration = now - lastElementText;
    
    // Get display duration for current element (recalculate each frame)
    uint32_t DISPLAY_MS = (elementIndex == 2) ? DISPLAY_MS_BORON : DISPLAY_MS_BASE; // Element index 2 is Boron
    const uint32_t TOTAL_DURATION_MS = FADE_IN_MS + DISPLAY_MS + FADE_OUT_MS;
    
    // Calculate fade progress
    bool isFadingIn = (elementTextDuration < FADE_IN_MS);
    bool isFadingOut = (elementTextDuration >= (FADE_IN_MS + DISPLAY_MS));
    
    if(isFadingIn) {
      fadeProgress = (float)elementTextDuration / FADE_IN_MS;
    } else if(elementTextDuration < (FADE_IN_MS + DISPLAY_MS)) {
      fadeProgress = 1.0f;
    } else if(elementTextDuration < TOTAL_DURATION_MS) {
      fadeProgress = 1.0f - ((float)(elementTextDuration - FADE_IN_MS - DISPLAY_MS) / FADE_OUT_MS);
    } else {
      elementTextActive = false;
      lastElementText = now;
      fadeProgress = 0.0f;
      return;
    }
    
    // Display element group (name, symbol, color, wavelength) together
    if(fadeProgress > 0.05f) {
      const ElementGroup& elem = elements[elementIndex];
      
      // Helper function to apply glitch effects during fade IN only (leetspeak)
      auto applyFadeInEffect = [](const char* text, float progress) -> void {
        if(progress > 0.7f) {
          // Normal text when mostly faded in
          lcd.print(text);
          return;
        }
        
        // During fade in: apply leetspeak and glitch effects
        int len = strlen(text);
        for(int i = 0; i < len; i++) {
          char c = text[i];
          
          // Leetspeak swaps (30% chance during fade in)
          if(random(0, 100) < 30) {
            if(c == 'A' || c == 'a') c = '4';
            else if(c == 'E' || c == 'e') c = '3';
            else if(c == 'I' || c == 'i') c = '1';
            else if(c == 'O' || c == 'o') c = '0';
            else if(c == 'S' || c == 's') c = '5';
            else if(c == 'T' || c == 't') c = '7';
            else if(c == 'L' || c == 'l') c = '1';
            else if(c == 'R' || c == 'r') c = (random(0, 2) == 0) ? '2' : c;
          }
          
          // Show character or dot during early fade
          if(progress < 0.3f && random(0, 100) < 40) {
            lcd.print('.');
          } else {
            lcd.print(c);
          }
        }
      };
      
      // Helper function to draw dripping text (characters fall downward)
      auto drawDrippingText = [&](const char* text, int startRow, int startCol, float fadeOutProgress) -> void {
        int len = strlen(text);
        // Calculate how far down characters have dripped (0.0 = original position, 1.0 = bottom)
        float dripProgress = fadeOutProgress; // Use fade out progress to control drip
        
        for(int i = 0; i < len; i++) {
          char c = text[i];
          int col = startCol + i;
          
          if(col >= 20) break; // Don't go off screen
          
          // Calculate vertical position: characters drip down at different rates
          // Each character gets a slight random offset for staggered dripping
          randomSeed(elementIndex * 1000 + i * 100 + (uint32_t)(fadeOutProgress * 1000));
          float charDripOffset = (random(0, 30) / 100.0f); // 0.0 to 0.3 offset
          float charDripProgress = dripProgress + charDripOffset;
          if(charDripProgress > 1.0f) charDripProgress = 1.0f;
          
          // Calculate which row this character should be on (dripping down)
          int targetRow = startRow + (int)(charDripProgress * (4 - startRow)); // Drip to bottom row
          if(targetRow >= 4) targetRow = 3; // Clamp to bottom row
          
          // Calculate brightness based on drip progress (fade as it drips)
          float charBrightness = 1.0f - (charDripProgress * 0.8f); // Fade to 20% brightness
          if(charBrightness < 0.2f) charBrightness = 0.2f;
          
          // Only draw if brightness is sufficient
          if(charBrightness > 0.25f && targetRow < 4) {
            lcd.setCursor(col, targetRow);
            
            // Use different characters based on brightness (fade effect)
            if(charBrightness > 0.8f) {
              lcd.print(c); // Full brightness
            } else if(charBrightness > 0.6f) {
              // Medium brightness - use similar character
              if(c >= 'A' && c <= 'Z') lcd.print((char)(c + 32)); // Lowercase
              else if(c >= 'a' && c <= 'z') lcd.print(c);
              else lcd.print(c);
            } else if(charBrightness > 0.4f) {
              // Low brightness - use dots/dashes
              char fadeChars[] = {':', '.', '-', '_'};
              lcd.print(fadeChars[random(0, sizeof(fadeChars)/sizeof(fadeChars[0]))]);
            } else {
              // Very low brightness - dots only
              lcd.print('.');
            }
          }
        }
      };
      
      // Prepare text lines
      char line1[21];
      snprintf(line1, sizeof(line1), "%s = %s", elem.name, elem.symbol);
      char wavelengthLine[21];
      snprintf(wavelengthLine, sizeof(wavelengthLine), "%s", elem.wavelength);
      
      if(isFadingOut) {
        // Fade out: Drip text downward
        float fadeOutProgress = 1.0f - fadeProgress; // 0.0 = start fade out, 1.0 = fully faded
        
        // Clear original positions first (optional - helps prevent ghosting)
        // Then draw dripping text
        
        // Line 1: Name = Symbol (drip down)
        drawDrippingText(line1, posY, posX, fadeOutProgress);
        
        // Line 2: Wavelength (drip down)
        if((posY + 1) < 4) {
          drawDrippingText(wavelengthLine, posY + 1, posX, fadeOutProgress);
        }
        
        // Line 3: Color (drip down)
        if((posY + 2) < 4) {
          drawDrippingText(elem.color, posY + 2, posX, fadeOutProgress);
        }
      } else {
        // Fade in or full display: Normal text with fade-in effects
        // Line 1: Name = Symbol
        lcd.setCursor(posX, posY);
        if(isFadingIn) {
          applyFadeInEffect(line1, fadeProgress);
        } else {
          lcd.print(line1);
        }
        
        // Line 2: Wavelength
        if((posY + 1) < 4) {
          lcd.setCursor(posX, posY + 1);
          if(isFadingIn) {
            applyFadeInEffect(wavelengthLine, fadeProgress);
          } else {
            lcd.print(wavelengthLine);
          }
        }
        
        // Line 3: Color
        if((posY + 2) < 4) {
          lcd.setCursor(posX, posY + 2);
          if(isFadingIn) {
            applyFadeInEffect(elem.color, fadeProgress);
          } else {
            lcd.print(elem.color);
          }
        }
      }
    }
  }
}

// Chinese and Japanese characters for glitch effect
const char* glitchChars[] = {
  // Numbers
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
  // Chinese characters
  "中", "文", "字", "符", "码", "乱", "码", "错", "误", "系", "统",
  "日", "本", "語", "言", "文", "字", "化", "数", "据", "信", "息",
  // Japanese characters  
  "あ", "い", "う", "え", "お", "か", "き", "く", "け", "こ",
  "さ", "し", "す", "せ", "そ", "た", "ち", "つ", "て", "と",
  "な", "に", "ぬ", "ね", "の", "は", "ひ", "ふ", "へ", "ほ",
  // Box drawing and block symbols
  "■", "□", "▣", "▤", "▥", "▦", "▧", "▨", "▩", "▪", "▫",
  "◊", "○", "●", "◐", "◑", "◒", "◓", "◔", "◕", "◖", "◗",
  "▲", "△", "▼", "▽", "◆", "◇", "★", "☆", "※", "§", "¶",
  // Math and technical symbols
  "±", "×", "÷", "≈", "≠", "≤", "≥", "∞", "∑", "∏", "∫",
  "√", "∆", "∇", "∂", "α", "β", "γ", "π", "Ω", "θ", "λ",
  // Currency and other symbols
  "$", "€", "£", "¥", "¢", "©", "®", "™", "°", "℃", "℉",
  // Punctuation and operators
  "!", "@", "#", "%", "&", "*", "?", ":", ";", "<", ">",
  "|", "\\", "/", "{", "}", "[", "]", "(", ")", "~", "`"
};
const int NUM_GLITCH_CHARS = sizeof(glitchChars) / sizeof(glitchChars[0]);

// Zalgo-style combining characters (approximated with ASCII)
const char* zalgoChars[] = {
  // Diacritics and accents
  "'", "`", "^", "~", "\"", ",", ".", "-", "_", "=", "+",
  // More symbols for variety
  ":", ";", "!", "?", "@", "#", "$", "%", "&", "*", "|",
  "\\", "/", "<", ">", "[", "]", "{", "}", "(", ")", "°"
};

void lcdGlitchTextAnimation(uint32_t now, uint32_t idleStartTime){
  const uint32_t FRAME_TIME = 80; // ms per frame - faster for glitch effect
  const uint32_t SPARKLE_DURATION_MIN = 3500; // Minimum sparkle duration: 3.5 seconds
  const uint32_t SPARKLE_DURATION_MAX = 15000; // Maximum sparkle duration: 15 seconds
  const uint32_t GLITCH_DURATION_MIN = 4000; // Minimum glitch text duration: 4 seconds
  const uint32_t GLITCH_DURATION_MAX = 7500; // Maximum glitch text duration: 7.5 seconds
  
  static uint32_t animationStart = 0;
  static uint32_t sparkleDuration = 0; // Random duration for this cycle
  static uint32_t glitchDuration = 0; // Random glitch duration for this cycle
  static bool sparklePhase = true;
  
  if(animationStart == 0){
    animationStart = now;
    // Randomize sparkle duration between min and max
    sparkleDuration = SPARKLE_DURATION_MIN + random(0, SPARKLE_DURATION_MAX - SPARKLE_DURATION_MIN + 1);
    // Randomize glitch text duration between min and max
    glitchDuration = GLITCH_DURATION_MIN + random(0, GLITCH_DURATION_MAX - GLITCH_DURATION_MIN + 1);
    sparklePhase = true;
    glitchActive = false;
    glitchPosition = 0;
    glitchMode = random(0, 2); // Random mode each cycle
  }
  
  uint32_t elapsed = now - animationStart;
  
  // Phase 1: Sparkles only (randomized duration)
  if(elapsed < sparkleDuration){
    sparklePhase = true;
    glitchActive = false;
    lcdSparkleAnimation(now);
    drawConsoleText(now, 0); // Add console text overlay (idleStartTime not available here)
    drawElementGroups(now); // Add element groups
    return;
  }
  
  // Phase 2: Glitch text overlays sparkles
  sparklePhase = false;
  glitchActive = true;
  
  if(now - lastGlitchUpdate > FRAME_TIME){
    lastGlitchUpdate = now;
    glitchFrame++;
    
    if(glitchMode == 0){
      // Top-down mode: columns fill from top to bottom
      glitchPosition++;
      if(glitchPosition > 4) glitchPosition = 0; // Reset after filling screen
    } else {
      // Left-right mode: rows fill from left to right, top to bottom
      glitchPosition++;
      if(glitchPosition > 80) glitchPosition = 0; // Reset after filling all 4 rows (4*20=80)
    }
  }
  
  // First draw sparkles as background
  lcdSparkleAnimation(now);
  
  // Add console text overlay (25% chance)
  drawConsoleText(now, idleStartTime);
  drawElementGroups(now); // Add element groups
  
  // Then overlay glitch text
  if(glitchMode == 0){
    // TOP-DOWN MODE: Fill columns from top to bottom
    for(int col = 0; col < 20; col++){
      int fillHeight = glitchPosition;
      if(fillHeight > 4) fillHeight = 4;
      
      for(int row = 0; row < fillHeight; row++){
        // Random glitch characters
        int charIdx = random(0, NUM_GLITCH_CHARS);
        lcd.setCursor(col, row);
        lcd.print(glitchChars[charIdx]);
        
        // Add zalgo-style glitch above/below sometimes
        if(random(0, 100) < 20 && row > 0){ // 20% chance, not on top row
          int zalgoIdx = random(0, sizeof(zalgoChars)/sizeof(zalgoChars[0]));
          lcd.setCursor(col, row-1);
          lcd.print(zalgoChars[zalgoIdx]);
        }
      }
      
      // Fade effect at bottom
      if(fillHeight > 0 && fillHeight < 4){
        int fadeRow = fillHeight;
        if(random(0, 100) < 50){
          lcd.setCursor(col, fadeRow);
          lcd.print(".");
        }
      }
    }
  } else {
    // LEFT-RIGHT MODE: Fill rows from left to right, top to bottom
    // glitchPosition goes from 0 to 80 (4 rows * 20 cols)
    int currentRow = glitchPosition / 20;
    int currentCol = glitchPosition % 20;
    
    // Fill completed rows fully
    for(int row = 0; row < currentRow && row < 4; row++){
      for(int col = 0; col < 20; col++){
        int charIdx = random(0, NUM_GLITCH_CHARS);
        lcd.setCursor(col, row);
        lcd.print(glitchChars[charIdx]);
      }
    }
    
    // Fill current row up to currentCol
    if(currentRow < 4){
      for(int col = 0; col < currentCol; col++){
        int charIdx = random(0, NUM_GLITCH_CHARS);
        lcd.setCursor(col, currentRow);
        lcd.print(glitchChars[charIdx]);
        
        // Add zalgo glitch above sometimes
        if(random(0, 100) < 15 && currentRow > 0){
          int zalgoIdx = random(0, sizeof(zalgoChars)/sizeof(zalgoChars[0]));
          lcd.setCursor(col, currentRow-1);
          lcd.print(zalgoChars[zalgoIdx]);
        }
      }
      
      // Fade effect at current position
      if(currentCol < 20 && random(0, 100) < 30){
        lcd.setCursor(currentCol, currentRow);
        lcd.print(".");
      }
    }
  }
  
  // Reset animation after full cycle
  if(elapsed > sparkleDuration + glitchDuration){
    animationStart = 0;
    sparkleDuration = 0; // Will be randomized on next cycle
    glitchDuration = 0; // Will be randomized on next cycle
    glitchPosition = 0;
    glitchMode = random(0, 2); // Random mode for next cycle
  }
}

// ===================== Additional LCD Animations =====================

// Particle explosion animation
void lcdParticleExplosion(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastExplosion = 0;
  static float particles[20][4]; // x, y, vx, vy for each particle
  static bool active[20] = {false};
  static uint32_t explosionStart = 0;
  
  const uint32_t EXPLOSION_INTERVAL = 3000; // New explosion every 3 seconds
  const uint32_t PARTICLE_LIFETIME = 2000; // Particles last 2 seconds
  
  // Trigger new explosion
  if(now - lastExplosion > EXPLOSION_INTERVAL){
    lastExplosion = now;
    explosionStart = now;
    // Spawn particles from center
    for(int i=0; i<20; i++){
      float angle = (float)i / 20.0f * 2.0f * M_PI;
      float speed = 0.05f + random(0, 30)/100.0f;
      particles[i][0] = 10.0f; // Center X
      particles[i][1] = 2.0f;  // Center Y
      particles[i][2] = cosf(angle) * speed;
      particles[i][3] = sinf(angle) * speed;
      active[i] = true;
    }
  }
  
  lcd.clear();
  
  // Add console text overlay (25% chance)
  drawConsoleText(now, 0); // idleStartTime not available
drawElementGroups(now); // Add element groups
  
  // Update and draw particles
  uint32_t particleAge = now - explosionStart;
  if(particleAge < PARTICLE_LIFETIME){
    for(int i=0; i<20; i++){
      if(active[i]){
        // Update position
        particles[i][0] += particles[i][2];
        particles[i][1] += particles[i][3];
        particles[i][3] += 0.002f; // Gravity
        
        // Check bounds
        int x = (int)particles[i][0];
        int y = (int)particles[i][1];
        if(x >= 0 && x < 20 && y >= 0 && y < 4){
          float fade = 1.0f - ((float)particleAge / PARTICLE_LIFETIME);
          if(random(0, 100) < (fade * 80)){
            lcd.setCursor(x, y);
            char symbols[] = {'*', '+', 'o', '.', ':', ';'};
            lcd.print(symbols[random(0, sizeof(symbols)/sizeof(symbols[0]))]);
          }
        } else {
          active[i] = false;
        }
      }
    }
  }
}

// Wave visualization
void lcdWaveVisualization(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  const uint32_t UPDATE_INTERVAL = 50; // 20 FPS
  
  if(now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;
  
  float tsec = now / 1000.0f;
  lcd.clear();
  
  // Add console text overlay (25% chance)
  drawConsoleText(now, 0); // idleStartTime not available
drawElementGroups(now); // Add element groups
  
  // Draw multiple sine waves
  for(int row = 0; row < 4; row++){
    for(int col = 0; col < 20; col++){
      float x = (float)col / 20.0f;
      float wave1 = sinf(2.0f * M_PI * (x * 2.0f + tsec * 0.5f));
      float wave2 = sinf(2.0f * M_PI * (x * 3.0f - tsec * 0.7f + row * 0.5f));
      float wave3 = sinf(2.0f * M_PI * (x * 1.5f + tsec * 0.3f));
      float combined = (wave1 + wave2 * 0.6f + wave3 * 0.4f) / 2.0f;
      
      // Map to character height
      int height = (int)((combined + 1.0f) * 1.5f); // 0-3
      if(height == row){
        lcd.setCursor(col, row);
        char bars[] = {'|', 'I', ':', '.'};
        lcd.print(bars[random(0, sizeof(bars)/sizeof(bars[0]))]);
      }
    }
  }
}

// Kaleidoscope pattern
void lcdKaleidoscope(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  const uint32_t UPDATE_INTERVAL = 100;
  
  if(now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;
  
  float tsec = now / 1000.0f;
  lcd.clear();
  
  // Add console text overlay (25% chance)
  drawConsoleText(now, 0); // idleStartTime not available
drawElementGroups(now); // Add element groups
  
  // Create symmetric kaleidoscope pattern
  for(int row = 0; row < 4; row++){
    for(int col = 0; col < 20; col++){
      // Mirror coordinates for symmetry
      int mirrorCol = (col < 10) ? col : (19 - col);
      int mirrorRow = (row < 2) ? row : (3 - row);
      
      float x = (float)mirrorCol / 10.0f;
      float y = (float)mirrorRow / 2.0f;
      float dist = sqrtf(x*x + y*y);
      float angle = atan2f(y, x);
      
      // Create pattern
      float pattern = sinf(dist * 5.0f - tsec * 2.0f) * sinf(angle * 3.0f + tsec);
      
      if(pattern > 0.3f){
        lcd.setCursor(col, row);
        char syms[] = {'*', '+', 'x', 'X', 'o', 'O', '#', '%'};
        int idx = (int)((pattern + 1.0f) * 4.0f) % (sizeof(syms)/sizeof(syms[0]));
        lcd.print(syms[idx]);
      }
    }
  }
}

// Pulsing geometric patterns
void lcdPulsingPatterns(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  const uint32_t UPDATE_INTERVAL = 80;
  
  if(now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;
  
  float tsec = now / 1000.0f;
  lcd.clear();
  
  // Add console text overlay (25% chance)
  drawConsoleText(now, 0); // idleStartTime not available
drawElementGroups(now); // Add element groups
  
  // Pulsing circles/patterns
  float pulse = 0.5f * (1.0f + sinf(tsec * 2.0f));
  
  for(int row = 0; row < 4; row++){
    for(int col = 0; col < 20; col++){
      float x = (float)col / 20.0f;
      float y = (float)row / 4.0f;
      float dist = sqrtf((x-0.5f)*(x-0.5f) + (y-0.5f)*(y-0.5f));
      
      // Concentric circles
      float circle = fabsf(dist - pulse * 0.4f);
      if(circle < 0.1f){
        lcd.setCursor(col, row);
        char patterns[] = {'O', 'o', '*', '+', '.'};
        lcd.print(patterns[random(0, sizeof(patterns)/sizeof(patterns[0]))]);
      }
      
      // Grid pattern
      if((int)(col + row + tsec * 5) % 3 == 0){
        lcd.setCursor(col, row);
        lcd.print('#');
      }
    }
  }
}

// Beat grid visualization
void lcdBeatGrid(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  static uint32_t lastBeat = 0;
  const uint32_t UPDATE_INTERVAL = 50;
  const uint32_t BEAT_INTERVAL = 600; // Simulated beat every 600ms
  
  if(now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;
  
  // Simulate beats
  bool beat = (now - lastBeat > BEAT_INTERVAL);
  if(beat){
    lastBeat = now;
  }
  
  lcd.clear();
  
  // Add console text overlay (25% chance)
  drawConsoleText(now, 0); // idleStartTime not available
drawElementGroups(now); // Add element groups
  
  // Grid of cells that light up on beat
  for(int row = 0; row < 4; row++){
    for(int col = 0; col < 20; col++){
      // Create grid pattern
      int cellRow = row;
      int cellCol = col / 2; // 2 cols per cell
      
      // Randomize which cells light up
      uint32_t seed = cellRow * 10 + cellCol;
      bool shouldLight = (random(0, 100) < 30);
      
      if(beat && shouldLight){
        // Bright on beat
        lcd.setCursor(col, row);
        char bright[] = {'#', '@', '*', 'O'};
        lcd.print(bright[random(0, sizeof(bright)/sizeof(bright[0]))]);
      } else if(shouldLight && (now - lastBeat) < 200){
        // Fade after beat
        lcd.setCursor(col, row);
        lcd.print(':');
      }
    }
  }
}

// Clear animation state
void lcdClearAnimation(){
  for(int i=0; i<10; i++){
    sparklePositions[i] = 0;
    sparkleSpawnTimes[i] = 0;
  }
  glitchFrame = 0;
  glitchPosition = 0;
  glitchActive = false;
}

// Pyramid animation - face-on and 3D views with fading edges
void lcdPyramidAnimation(uint32_t now, uint32_t idleStartTime){
  static uint32_t animStart = 0;
  static int viewMode = 0; // 0 = face-on, 1 = 3D isometric
  static float edgeBrightness[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // Brightness for each edge
  static float edgePhase[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // Phase for each edge fade
  const uint32_t VIEW_SWITCH_INTERVAL = 4000; // Switch views every 4 seconds (at least 3 seconds per view)
  const float FADE_SPEED = 0.02f; // Speed of edge fading
  
  // Initialize or switch views
  if(animStart == 0 || (now - animStart) > VIEW_SWITCH_INTERVAL){
    animStart = now;
    viewMode = (viewMode + 1) % 2; // Alternate between views
    // Randomize edge phases for variety
    for(int i = 0; i < 6; i++){
      edgePhase[i] = random(0, 628) / 100.0f; // Random phase 0-2π
    }
  }
  
  uint32_t elapsed = now - animStart;
  
  // Update edge brightness with sine waves (different phases for each edge)
  for(int i = 0; i < 6; i++){
    edgePhase[i] += FADE_SPEED;
    if(edgePhase[i] > 6.28f) edgePhase[i] -= 6.28f;
    edgeBrightness[i] = 0.3f + 0.7f * (sinf(edgePhase[i]) * 0.5f + 0.5f); // 0.3 to 1.0
  }
  
  lcd.clear();
  
  if(viewMode == 0){
    // Face-on view: Triangle pyramid (4 rows high)
    // Row 0:        ^
    // Row 1:       / \
    // Row 2:      /   \
    // Row 3:     /_____\
    
    const char* pyramidLines[] = {
      "        ^        ",  // Row 0
      "       / \\       ",  // Row 1
      "      /   \\      ",  // Row 2
      "     /_____\\     "   // Row 3
    };
    
    // Draw pyramid with fading edges
    for(int row = 0; row < 4; row++){
      const char* line = pyramidLines[row];
      for(int col = 0; col < 20; col++){
        char c = line[col];
        if(c == ' '){
          lcd.setCursor(col, row);
          lcd.print(' ');
        } else {
          // Determine which edge this character belongs to
          int edgeIdx = 0;
          if(row == 0 && c == '^') edgeIdx = 0; // Top point
          else if(c == '/') edgeIdx = 1; // Left edge
          else if(c == '\\') edgeIdx = 2; // Right edge
          else if(c == '_') edgeIdx = 3; // Base edge
          else if(c == '-') edgeIdx = 3; // Base edge
          
          // Use brightness to determine character
          float brightness = edgeBrightness[edgeIdx];
          char displayChar = ' ';
          
          if(brightness > 0.8f) displayChar = c; // Full brightness
          else if(brightness > 0.6f) displayChar = (c == '/' || c == '\\') ? '|' : (c == '_' ? '-' : c);
          else if(brightness > 0.4f) displayChar = (c == '/' || c == '\\') ? ':' : (c == '_' ? '.' : '.');
          else if(brightness > 0.2f) displayChar = '.';
          // else displayChar = ' ' (already set)
          
          if(displayChar != ' '){
            lcd.setCursor(col, row);
            lcd.print(displayChar);
          }
        }
      }
    }
  } else {
    // 3D isometric view: Pyramid from an angle (fits in 4 rows)
    // Row 0:        /\
    // Row 1:       /  \
    // Row 2:      /____\
    // Row 3:     /|    |\
    
    const char* pyramid3DLines[] = {
      "        /\\        ",  // Row 0 - top
      "       /  \\       ",  // Row 1 - sides
      "      /____\\      ",  // Row 2 - base front
      "     /|    |\\     "   // Row 3 - base back edges
    };
    
    // Draw 3D pyramid (using 4 rows)
    for(int row = 0; row < 4; row++){
      const char* line = pyramid3DLines[row];
      for(int col = 0; col < 20; col++){
        char c = line[col];
        if(c == ' '){
          lcd.setCursor(col, row);
          lcd.print(' ');
        } else {
          // Determine which edge this character belongs to
          int edgeIdx = 0;
          if(row == 0 && c == '/') edgeIdx = 4; // Left front edge (top)
          else if(row == 0 && c == '\\') edgeIdx = 5; // Right front edge (top)
          else if(row == 1 && c == '/' && col < 10) edgeIdx = 4; // Left front edge
          else if(row == 1 && c == '\\' && col >= 10) edgeIdx = 5; // Right front edge
          else if(row == 2 && c == '_') edgeIdx = 3; // Base front edge
          else if(row == 3 && c == '|') edgeIdx = 1; // Left back edge
          else if(row == 3 && c == '/') edgeIdx = 4; // Left front edge (base)
          else if(row == 3 && c == '\\') edgeIdx = 5; // Right front edge (base)
          else if(c == '/') edgeIdx = 4; // Default left front
          else if(c == '\\') edgeIdx = 5; // Default right front
          
          // Use brightness to determine character
          float brightness = edgeBrightness[edgeIdx];
          char displayChar = ' ';
          
          if(brightness > 0.8f) displayChar = c; // Full brightness
          else if(brightness > 0.6f) {
            if(c == '/' || c == '\\') displayChar = '|';
            else if(c == '_') displayChar = '-';
            else displayChar = c;
          }
          else if(brightness > 0.4f) {
            if(c == '/' || c == '\\' || c == '|') displayChar = ':';
            else if(c == '_' || c == '-') displayChar = '.';
            else displayChar = '.';
          }
          else if(brightness > 0.2f) displayChar = '.';
          // else displayChar = ' ' (already set)
          
          if(displayChar != ' '){
            lcd.setCursor(col, row);
            lcd.print(displayChar);
          }
        }
      }
    }
  }
}

// Welcome animation when exiting idle mode
// Fades current animation to black, then shows PYRRISMA text, then fades in Multi Spectral Console
static bool s_welcomeBeginNext = false;

void lcdWelcomeAnimationBeginSession(void) {
  s_welcomeBeginNext = true;
}

void lcdWelcomeAnimation(uint32_t now, uint32_t lastButtonPressTime) {
  static uint32_t animStart = 0;
  static bool animActive = false;
  static int animPhase = 0; // 0=fade to black, 1=PYRRISMA, 2=PYRRISMA fade out, 99=finished (hold until long gap)
  static int consoleTextIndex = 0;
  static uint32_t lastCallTime = 0;
  const uint32_t FADE_DURATION_MS = 100; // Fast fade to black
  const uint32_t PYRRISMA_DURATION_MS = 3000; // 3 seconds minimum (as requested)
  const uint32_t PYRRISMA_FADE_OUT_MS = 750; // 0.75 seconds fade away
  const uint32_t CONSOLE_FADE_DURATION_MS = 500; // 0.5 seconds fade in
  
  // Console text variations with leetspeak
  const char* consoleTexts[] = {
    "Multi Spectral Console",
    "MULTI SPECTRAL CONSOLE",
    "multi spectral console",
    "MULT1 SP3CTR4L C0NS0L3",
    "mult1 sp3ctr4l c0ns0l3",
    "MuLtI SpEcTrAl CoNsOlE",
    "MULT1 SP3CTR4L CONSOLE",
    "MULTI SP3CTR4L C0NS0L3",
    "MULT1 SPECTRAL C0NS0L3"
  };
  const int NUM_CONSOLE_TEXTS = sizeof(consoleTexts) / sizeof(consoleTexts[0]);
  
  // New session: explicit begin, first call, or idle long enough for a fresh welcome (do NOT use !animActive — that retriggers after every completion)
  if (s_welcomeBeginNext || lastCallTime == 0 || (now - lastCallTime > 6000)) {
    s_welcomeBeginNext = false;
    animStart = now;
    animActive = true;
    animPhase = 0;
    consoleTextIndex = random(0, NUM_CONSOLE_TEXTS);
    lcd.backlight();
  }
  lastCallTime = now;

  if (animPhase == 99) {
    return;
  }
  
  uint32_t elapsed = now - animStart;
  
  if(animPhase == 0) {
    // Phase 1: Fade current animation to black
    if(elapsed < FADE_DURATION_MS) {
      // Fill screen with progressively darker characters
      float fadeProgress = (float)elapsed / FADE_DURATION_MS;
      // Use characters that get darker: full blocks -> half blocks -> spaces (no dots)
      char darkChars[] = {'\xFF', '\xDB', '\xB2', '\xB1', ' '};
      int charIndex = (int)(fadeProgress * (sizeof(darkChars)/sizeof(darkChars[0]) - 1));
      if(charIndex >= sizeof(darkChars)/sizeof(darkChars[0])) charIndex = sizeof(darkChars)/sizeof(darkChars[0]) - 1;
      
      lcd.clear();
      for(int row = 0; row < 4; row++) {
        for(int col = 0; col < 20; col++) {
          lcd.setCursor(col, row);
          lcd.print(darkChars[charIndex]);
        }
      }
    } else {
      // Fade complete, move to PYRRISMA phase
      animPhase = 1;
      animStart = now; // Reset timer for PYRRISMA phase
    }
  } else if(animPhase == 1) {
    // Phase 2: Show PYRRISMA text with leetspeak swaps, Multi Spectral Console starts at 0.3s
    uint32_t phaseElapsed = now - animStart;
    const uint32_t CONSOLE_START_MS = 300; // Multi Spectral Console starts 0.3s into PYRRISMA
    const uint32_t LEETSPEAK_DELAY_MS = 2000; // Wait 2s after both texts visible before leetspeak
    
    if(phaseElapsed < PYRRISMA_DURATION_MS) {
      lcd.clear();
      const char* baseText = "=- PYRRISMA -=";
      int textLen = strlen(baseText);
      int startPos = (20 - textLen) / 2; // Center it
      
      // Calculate when both texts are fully visible
      // Multi Spectral Console fades in over 500ms starting at 0.3s, so fully visible at 0.8s
      uint32_t bothVisibleTime = CONSOLE_START_MS + CONSOLE_FADE_DURATION_MS; // 0.8s
      bool bothVisible = (phaseElapsed >= bothVisibleTime);
      uint32_t timeSinceBothVisible = bothVisible ? (phaseElapsed - bothVisibleTime) : 0;
      
      // PYRRISMA leetspeak: when alone (before Multi Spectral Console), use normal behavior
      // When both visible together, wait 2s before leetspeak
      bool pyrrismaAlone = (phaseElapsed < CONSOLE_START_MS);
      bool pyrrismaAllowLeetspeak;
      float swapProgress;
      
      if(pyrrismaAlone) {
        // PYRRISMA alone: normal leetspeak behavior (increasing over time)
        pyrrismaAllowLeetspeak = true;
        swapProgress = (float)phaseElapsed / CONSOLE_START_MS; // Progress over first 1s
      } else {
        // Both visible: wait 2s after both are fully visible
        pyrrismaAllowLeetspeak = bothVisible && (timeSinceBothVisible >= LEETSPEAK_DELAY_MS);
        if(pyrrismaAllowLeetspeak) {
          // Calculate progress for remaining time
          uint32_t remainingTime = PYRRISMA_DURATION_MS - phaseElapsed;
          uint32_t availableTime = PYRRISMA_DURATION_MS - bothVisibleTime - LEETSPEAK_DELAY_MS;
          if(availableTime > 0 && remainingTime > 0) {
            swapProgress = 1.0f - ((float)remainingTime / availableTime);
          } else {
            swapProgress = 1.0f;
          }
        } else {
          swapProgress = 0.0f;
        }
      }
      if(swapProgress < 0.0f) swapProgress = 0.0f;
      if(swapProgress > 1.0f) swapProgress = 1.0f;
      int swapChance = pyrrismaAllowLeetspeak ? (int)(swapProgress * 40) : 0; // Up to 40% chance
      
      // Use time-based randomization for consistent but changing swaps
      uint32_t swapSeed = (phaseElapsed / 100); // Change every 100ms
      
      // Show PYRRISMA on line 1
      lcd.setCursor(startPos, 1);
      for(int i = 0; i < textLen; i++) {
        char c = baseText[i];
        
        // Randomly swap characters to leetspeak (only after delay)
        randomSeed(swapSeed + i);
        if(pyrrismaAllowLeetspeak && random(0, 100) < swapChance) {
          // Leetspeak character swaps
          if(c == 'A' || c == 'a') c = '4';
          else if(c == 'E' || c == 'e') c = '3';
          else if(c == 'I' || c == 'i') c = '1';
          else if(c == 'O' || c == 'o') c = '0';
          else if(c == 'S' || c == 's') c = '5';
          else if(c == 'R' || c == 'r') c = '2';
          else if(c == 'P' || c == 'p') c = (random(0, 2) == 0) ? '9' : c;
          else if(c == 'Y' || c == 'y') c = (random(0, 2) == 0) ? '7' : c;
        }
        lcd.print(c);
      }
      
      // Show Multi Spectral Console on line 3 (with gap from PYRRISMA on line 1)
      if(phaseElapsed >= CONSOLE_START_MS) {
        const char* consoleText = consoleTexts[consoleTextIndex];
        int consoleTextLen = strlen(consoleText);
        int consoleStartPos = (20 - consoleTextLen) / 2; // Center it
        
        // Fade in effect: show text with increasing visibility
        float consoleFadeProgress = (float)(phaseElapsed - CONSOLE_START_MS) / 500.0f; // Fade in over 500ms
        if(consoleFadeProgress > 1.0f) consoleFadeProgress = 1.0f;
        
        // Multi Spectral Console leetspeak: can start earlier when alone (during fade in)
        // When both visible together, wait 2s after both are fully visible
        bool consoleAlone = !bothVisible; // Still fading in, so "alone"
        bool consoleAllowLeetspeak;
        float consoleSwapProgress;
        
        if(consoleAlone) {
          // During fade in: allow leetspeak immediately (user said "feel free to change multi spectral console text earlier")
          consoleAllowLeetspeak = true;
          consoleSwapProgress = consoleFadeProgress; // Progress with fade
        } else {
          // Both visible: wait 2s after both are fully visible
          consoleAllowLeetspeak = (timeSinceBothVisible >= LEETSPEAK_DELAY_MS);
          if(consoleAllowLeetspeak) {
            // Calculate progress for remaining time
            uint32_t remainingTime = PYRRISMA_DURATION_MS - phaseElapsed;
            uint32_t availableTime = PYRRISMA_DURATION_MS - bothVisibleTime - LEETSPEAK_DELAY_MS;
            if(availableTime > 0 && remainingTime > 0) {
              consoleSwapProgress = 1.0f - ((float)remainingTime / availableTime);
            } else {
              consoleSwapProgress = 1.0f;
            }
          } else {
            consoleSwapProgress = 0.0f;
          }
        }
        if(consoleSwapProgress < 0.0f) consoleSwapProgress = 0.0f;
        if(consoleSwapProgress > 1.0f) consoleSwapProgress = 1.0f;
        
        uint32_t consoleSwapSeed = ((phaseElapsed - CONSOLE_START_MS) / 50); // Change every 50ms
        int consoleSwapChance = consoleAllowLeetspeak ? (int)(consoleSwapProgress * 50) : 0; // Up to 50% chance
        
        lcd.setCursor(consoleStartPos, 3); // Line 3 (gap from PYRRISMA on line 1)
        for(int i = 0; i < consoleTextLen; i++) {
          char c = consoleText[i];
          
          // Leetspeak swaps
          randomSeed(consoleSwapSeed + i);
          if(consoleAllowLeetspeak && random(0, 100) < consoleSwapChance) {
            // Leetspeak character swaps
            if(c == 'A' || c == 'a') c = '4';
            else if(c == 'E' || c == 'e') c = '3';
            else if(c == 'I' || c == 'i') c = '1';
            else if(c == 'O' || c == 'o') c = '0';
            else if(c == 'S' || c == 's') c = '5';
            else if(c == 'T' || c == 't') c = '7';
            else if(c == 'L' || c == 'l') c = '1';
            else if(c == 'R' || c == 'r') c = (random(0, 2) == 0) ? '2' : c;
          }
          
          // Show character (fade effect: show dots/spaces early, then characters)
          // Only apply fade effect during the fade-in period, ensure full brightness after
          if(consoleFadeProgress >= 1.0f) {
            // Fade complete: always show full brightness character
            lcd.print(c);
          } else if(consoleFadeProgress < 0.3f && random(0, 100) < 70) {
            // Early fade: show dots/spaces randomly (only during first 30% of fade)
            lcd.print('.');
          } else {
            // During fade-in (30%-100%): show actual character at increasing brightness
            lcd.print(c);
          }
        }
      }
    } else {
      // PYRRISMA display complete, move to fade out
      animPhase = 2;
      animStart = now; // Reset timer for fade out phase
    }
  } else if(animPhase == 2) {
    // Phase 3: PYRRISMA and Multi Spectral Console fade away together
    uint32_t phaseElapsed = now - animStart;
    if(phaseElapsed < PYRRISMA_FADE_OUT_MS) {
      lcd.clear();
      const char* baseText = "=- PYRRISMA -=";
      int textLen = strlen(baseText);
      int startPos = (20 - textLen) / 2; // Center it
      
      // Fade out effect: show progressively dimmer characters
      float fadeProgress = (float)phaseElapsed / PYRRISMA_FADE_OUT_MS;
      
      // Use characters that get darker: full blocks -> half blocks -> spaces (no dots)
      char fadeChars[] = {'\xFF', '\xDB', '\xB2', '\xB1', ' '};
      int charIndex = (int)(fadeProgress * (sizeof(fadeChars)/sizeof(fadeChars[0]) - 1));
      if(charIndex >= sizeof(fadeChars)/sizeof(fadeChars[0])) charIndex = sizeof(fadeChars)/sizeof(fadeChars[0]) - 1;
      
      // Show PYRRISMA with fade effect on line 1
      lcd.setCursor(startPos, 1);
      for(int i = 0; i < textLen; i++) {
        // Randomly show fade characters or actual text based on fade progress
        if(random(0, 100) < (fadeProgress * 100)) {
          lcd.print(fadeChars[charIndex]);
        } else {
          char c = baseText[i];
          // Still apply some leetspeak swaps during fade
          randomSeed((phaseElapsed / 50) + i);
          if(random(0, 100) < 30) {
            if(c == 'A' || c == 'a') c = '4';
            else if(c == 'E' || c == 'e') c = '3';
            else if(c == 'I' || c == 'i') c = '1';
            else if(c == 'O' || c == 'o') c = '0';
            else if(c == 'S' || c == 's') c = '5';
            else if(c == 'R' || c == 'r') c = '2';
          }
          lcd.print(c);
        }
      }
      
      // Show Multi Spectral Console fading away on line 3 (gap from PYRRISMA)
      const char* consoleText = consoleTexts[consoleTextIndex];
      int consoleTextLen = strlen(consoleText);
      int consoleStartPos = (20 - consoleTextLen) / 2; // Center it
      
      lcd.setCursor(consoleStartPos, 3);
      for(int i = 0; i < consoleTextLen; i++) {
        // Randomly show fade characters or actual text based on fade progress
        if(random(0, 100) < (fadeProgress * 100)) {
          lcd.print(fadeChars[charIndex]);
        } else {
          char c = consoleText[i];
          // Still apply some leetspeak swaps during fade
          randomSeed((phaseElapsed / 50) + i + 1000);
          if(random(0, 100) < 30) {
            if(c == 'A' || c == 'a') c = '4';
            else if(c == 'E' || c == 'e') c = '3';
            else if(c == 'I' || c == 'i') c = '1';
            else if(c == 'O' || c == 'o') c = '0';
            else if(c == 'S' || c == 's') c = '5';
            else if(c == 'T' || c == 't') c = '7';
            else if(c == 'L' || c == 'l') c = '1';
            else if(c == 'R' || c == 'r') c = (random(0, 2) == 0) ? '2' : c;
          }
          lcd.print(c);
        }
      }
    } else {
      // Fade out complete — hold in 99 so we don't reset to phase 0 on the next call (was infinite PYRRISMA + LCD/backlight weirdness)
      animPhase = 99;
      animActive = false;
    }
  }
}

// ===================== Egyptian and Soviet Themed Animations =====================

// Egyptian #1: Hieroglyphic Scroll - horizontal scrolling hieroglyphs with fade effects
void lcdHieroglyphicScroll(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  static int scrollPos = 0;
  const uint32_t UPDATE_INTERVAL = 150; // Scroll speed
  const int SCROLL_WIDTH = 20;
  
  // Hieroglyphic symbols (simplified ASCII representation)
  const char hieroglyphs[] = {'^', '|', '-', '=', '*', '#', 'O', '[', ']', '(', ')', '<', '>', '/', '\\', 'V', 'A', 'X'};
  const int numGlyphs = 18;
  
  // Create a long string of hieroglyphs (regenerate periodically)
  static char glyphString[60];
  static uint32_t lastRegen = 0;
  if(now - lastRegen > 10000 || lastRegen == 0){ // Regenerate every 10 seconds
    randomSeed(now);
    for(int i = 0; i < 60; i++){
      glyphString[i] = hieroglyphs[random(0, numGlyphs)];
    }
    lastRegen = now;
  }
  
  if(now - lastUpdate > UPDATE_INTERVAL){
    lastUpdate = now;
    scrollPos++;
    if(scrollPos >= 60) scrollPos = 0;
  }
  
  lcd.clear();
  
  // Display scrolling hieroglyphs on each row with slight offset
  for(int row = 0; row < 4; row++){
    for(int col = 0; col < 20; col++){
      int glyphIdx = (scrollPos + col + row * 3) % 60;
      char glyph = glyphString[glyphIdx];
      
      // Fade effect at edges
      float fadeLeft = (col < 5) ? (col / 5.0f) : 1.0f;
      float fadeRight = (col > 14) ? ((20 - col) / 5.0f) : 1.0f;
      float fade = (fadeLeft < fadeRight) ? fadeLeft : fadeRight;
      
      if(random(0, 100) < (fade * 100)){
        lcd.setCursor(col, row);
        lcd.print(glyph);
      }
    }
  }
}

// Soviet #1: Cyrillic Text Scroll - scrolling Cyrillic-style text
void lcdCyrillicTextScroll(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  static int scrollPos = 0;
  const uint32_t UPDATE_INTERVAL = 100; // Scroll speed
  
  // Cyrillic-style text (using ASCII characters that look similar)
  const char* cyrillicText = "  ===== ПИРРИСМА ===== МУЛЬТИ СПЕКТРАЛЬНЫЙ КОНСОЛЬ  ";
  int textLen = strlen(cyrillicText);
  
  if(now - lastUpdate > UPDATE_INTERVAL){
    lastUpdate = now;
    scrollPos++;
    if(scrollPos >= textLen) scrollPos = 0;
  }
  
  lcd.clear();
  
  // Display scrolling text on middle rows
  for(int row = 1; row < 3; row++){
    for(int col = 0; col < 20; col++){
      int charIdx = (scrollPos + col) % textLen;
      lcd.setCursor(col, row);
      lcd.print(cyrillicText[charIdx]);
    }
  }
  
  // Add decorative borders on top and bottom
  for(int col = 0; col < 20; col++){
    lcd.setCursor(col, 0);
    lcd.print('-');
    lcd.setCursor(col, 3);
    lcd.print('-');
  }
}

// Soviet #3: Factory/Industrial Text - text appears with mechanical/industrial aesthetic
void lcdFactoryIndustrialText(uint32_t now, uint32_t idleStartTime){
  static int charIndex = 0;
  const uint32_t CHAR_INTERVAL = 200; // Time between characters appearing
  const uint32_t RESET_INTERVAL = 6000; // Reset after 6 seconds
  
  const char* industrialText = "ПИРРИСМА";
  const char* subtitle = "МУЛЬТИ СПЕКТРАЛЬНЫЙ";
  int textLen = strlen(industrialText);
  int subtitleLen = strlen(subtitle);
  
  uint32_t elapsed = now - idleStartTime;
  uint32_t cycleTime = elapsed % RESET_INTERVAL;
  
  // Reset cycle start when new cycle begins
  if(cycleTime < 100){
    charIndex = 0;
  }
  
  // Type out main text
  if(cycleTime < (textLen * CHAR_INTERVAL + 1000)){
    charIndex = (cycleTime / CHAR_INTERVAL);
    if(charIndex > textLen) charIndex = textLen;
  } else {
    // Show subtitle
    charIndex = textLen;
  }
  
  lcd.clear();
  
  // Main text with typewriter effect
  int startPos = (20 - textLen) / 2;
  lcd.setCursor(startPos, 1);
  for(int i = 0; i < charIndex && i < textLen; i++){
    lcd.print(industrialText[i]);
  }
  
  // Add blinking cursor
  if((now / 300) % 2 == 0 && charIndex < textLen){
    lcd.print('_');
  }
  
  // Industrial decorations
  lcd.setCursor(0, 0);
  lcd.print("[====]");
  lcd.setCursor(14, 0);
  lcd.print("[====]");
  
  lcd.setCursor(0, 3);
  lcd.print("[====]");
  lcd.setCursor(14, 3);
  lcd.print("[====]");
  
  // Show subtitle after main text
  if(charIndex >= textLen && cycleTime > (textLen * CHAR_INTERVAL + 1000)){
    int subStartPos = (20 - subtitleLen) / 2;
    if(subStartPos < 0) subStartPos = 0;
    lcd.setCursor(subStartPos, 2);
    lcd.print(subtitle);
  }
  
  if(cycleTime > RESET_INTERVAL - 100){
    charIndex = 0;
  }
}

// Soviet #6: Soviet Era Slogan Style - text appears in bold, blocky style, each word sequentially
void lcdSovietSloganStyle(uint32_t now, uint32_t idleStartTime){
  const uint32_t WORD_INTERVAL = 800; // Time each word is displayed
  
  const char* words[] = {"ПИРРИСМА", "МУЛЬТИ", "СПЕКТРАЛЬНЫЙ", "КОНСОЛЬ"};
  const int numWords = 4;
  
  uint32_t elapsed = now - idleStartTime;
  uint32_t cycleTime = elapsed % ((numWords * WORD_INTERVAL) + 1000);
  
  // Determine which word to show
  int currentWord = (cycleTime / WORD_INTERVAL) % numWords;
  float wordProgress = (float)(cycleTime % WORD_INTERVAL) / WORD_INTERVAL;
  
  lcd.clear();
  
  // Bold blocky style - show word with emphasis
  const char* word = words[currentWord];
  int wordLen = strlen(word);
  int startPos = (20 - wordLen) / 2;
  
  // Center the word
  lcd.setCursor(startPos, 1);
  
  // Add blocky borders around word
  if(wordProgress > 0.1f){ // Word appears after brief pause
    for(int i = 0; i < wordLen; i++){
      // Make it bold by potentially doubling characters or using block chars
      lcd.print(word[i]);
    }
  }
  
  // Add decorative brackets
  if(wordProgress > 0.2f){
    lcd.setCursor(startPos - 2, 1);
    lcd.print("[");
    lcd.setCursor(startPos + wordLen + 1, 1);
    lcd.print("]");
  }
  
  // Add top and bottom emphasis lines
  if(wordProgress > 0.3f){
    for(int col = startPos - 2; col < startPos + wordLen + 2 && col < 20; col++){
      if(col >= 0){
        lcd.setCursor(col, 0);
        lcd.print("=");
        lcd.setCursor(col, 2);
        lcd.print("=");
      }
    }
  }
  
  // Show next word hint at bottom
  if(wordProgress > 0.7f && currentWord < numWords - 1){
    int nextWordLen = strlen(words[currentWord + 1]);
    int nextStartPos = (20 - nextWordLen) / 2;
    lcd.setCursor(nextStartPos, 3);
    // Fade in next word
    float fade = (wordProgress - 0.7f) / 0.3f;
    if(random(0, 100) < (fade * 50)){
      lcd.print(words[currentWord + 1]);
    }
  }
}

// Soviet #7: Matrix-Style Cyrillic - Cyrillic characters falling like rain
void lcdMatrixCyrillic(uint32_t now, uint32_t idleStartTime){
  static uint32_t lastUpdate = 0;
  static float dropPositions[20]; // One drop per column
  static float dropSpeeds[20];
  static char dropChars[20];
  const uint32_t UPDATE_INTERVAL = 50;
  
  // Cyrillic-like characters (using ASCII that looks similar)
  const char cyrillicChars[] = {'A', 'B', 'E', 'K', 'M', 'H', 'O', 'P', 'C', 'T', 'Y', 'X', 
                                 '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 
                                 '=', '+', '-', '*', '#', '%', '&'};
  const int numChars = 29;
  
  // Initialize drops
  static bool initialized = false;
  if(!initialized){
    for(int i = 0; i < 20; i++){
      dropPositions[i] = random(-10, 0) / 10.0f; // Start above screen
      dropSpeeds[i] = 0.1f + (random(0, 50) / 100.0f); // Random speed
      dropChars[i] = cyrillicChars[random(0, numChars)];
    }
    initialized = true;
  }
  
  if(now - lastUpdate > UPDATE_INTERVAL){
    lastUpdate = now;
    
    // Update drop positions
    for(int i = 0; i < 20; i++){
      dropPositions[i] += dropSpeeds[i];
      
      // Reset drop when it goes off screen
      if(dropPositions[i] > 4.5f){
        dropPositions[i] = random(-10, 0) / 10.0f;
        dropSpeeds[i] = 0.1f + (random(0, 50) / 100.0f);
        dropChars[i] = cyrillicChars[random(0, numChars)];
      }
    }
  }
  
  lcd.clear();
  
  // Draw falling characters
  for(int col = 0; col < 20; col++){
    float pos = dropPositions[col];
    
    // Draw trail of characters
    for(int trail = 0; trail < 4; trail++){
      float trailPos = pos - trail;
      int row = (int)trailPos;
      
      if(row >= 0 && row < 4){
        // Fade effect - characters get dimmer as trail goes up
        float brightness = 1.0f - (trail * 0.25f);
        if(brightness > 0.1f && random(0, 100) < (brightness * 100)){
          lcd.setCursor(col, row);
          
          // Use different characters for trail effect
          if(trail == 0){
            lcd.print(dropChars[col]); // Bright head
          } else {
            // Dimmer trail characters
            char trailChars[] = {'|', ':', '.', ' '};
            int trailCharIdx = (trail < 4) ? trail : 3;
            if(brightness > 0.3f){
              lcd.print(trailChars[trailCharIdx]);
            }
          }
        }
      }
    }
  }
  
  // Occasionally show "ПИРРИСМА" text briefly
  if((now / 2000) % 10 == 0 && (now % 2000) < 500){
    lcd.setCursor(6, 1);
    lcd.print("ПИРРИСМА");
  }
}

// ===================== Retro rhythm game LCD =====================

// Horizontal marquee: cycle = title + gap spaces, ~msPerCol ms per column shift.
static void lcdRetroTitleScrollSegment(char *out, size_t outSz, const char *title, uint32_t wallMs, unsigned visibleCols,
                                       unsigned msPerCol) {
  if (!out || outSz < visibleCols + 1 || visibleCols == 0 || msPerCol == 0)
    return;
  const char *t = title ? title : "";
  size_t L = strlen(t);
  if (L == 0) {
    memset(out, ' ', visibleCols);
    out[visibleCols] = '\0';
    return;
  }
  if (L <= visibleCols) {
    for (size_t i = 0; i < visibleCols; i++)
      out[i] = (i < L) ? (char)t[i] : ' ';
    out[visibleCols] = '\0';
    return;
  }
  const unsigned gap = 3;
  unsigned cycle = (unsigned)L + gap;
  unsigned shift = (unsigned)((wallMs / (uint32_t)msPerCol) % cycle);
  for (unsigned i = 0; i < visibleCols; i++) {
    unsigned idx = (shift + i) % cycle;
    out[i] = (idx < L) ? (char)t[idx] : ' ';
  }
  out[visibleCols] = '\0';
}

// ~14s per full marquee cycle for long titles (slow enough to read comfortably).
// Short titles dwell statically per col with a generous timer so any partial-col
// re-render between frames doesn't smear the text.
static unsigned lcdRetroTitleScrollMsPerCol(const char *title, unsigned visibleCols) {
  size_t L = title ? strlen(title) : 0;
  if (L <= visibleCols)
    return 600u;
  const unsigned gap = 3;
  unsigned cycle = (unsigned)L + gap;
  const unsigned targetCycleMs = 14000u;
  unsigned mpc = targetCycleMs / cycle;
  if (mpc < 220u)
    mpc = 220u;
  if (mpc > 700u)
    mpc = 700u;
  return mpc;
}

uint32_t lcdRetroTitleScrollDurationMs(const char *songTitle, unsigned visibleCols) {
  size_t L = songTitle ? strlen(songTitle) : 0;
  if (visibleCols == 0)
    visibleCols = 20;
  if (L == 0)
    return 700u;
  if (L <= visibleCols)
    return 1100u;
  const unsigned gap = 3;
  unsigned cycle = (unsigned)L + gap;
  unsigned mpc = lcdRetroTitleScrollMsPerCol(songTitle, visibleCols);
  return (uint32_t)cycle * mpc + 500u;
}

static uint32_t lcdRetroFlashHash(uint32_t t, unsigned idx, unsigned salt) {
  uint32_t x = t * 1664525u + idx * 1013904223u + salt * 374761393u;
  x ^= x >> 16;
  x *= 2246822519u;
  x ^= x >> 13;
  return x;
}

// Enter rhythm: light glitch intro, then stable plain text (readability).
static const uint32_t kRetroFlashFadeInMs = 480;
static const uint32_t kRetroFlashGlitchHoldMs = 700;
static const uint32_t kRetroFlashFadeGlitchMs = 380;
static const uint32_t kRetroFlashPlainHoldMs = 1600; // >= 1.5 s clean label

uint32_t lcdRetroFlashDurationMs(void) {
  return kRetroFlashFadeInMs + kRetroFlashGlitchHoldMs + kRetroFlashFadeGlitchMs + kRetroFlashPlainHoldMs;
}

void lcdRetroFlashScreen(uint32_t now, uint32_t flashStartMs) {
  uint32_t t = now - flashStartMs;
  const uint32_t tGlitchEnd = kRetroFlashFadeInMs + kRetroFlashGlitchHoldMs + kRetroFlashFadeGlitchMs;

  if (t >= tGlitchEnd) {
    static const char kPlain0[] = "    RHYTHM MODE    ";
    static const char kPlain1[] = "38+39 3s=menu 4s=out";
    lcd.setCursor(0, 0);
    lcd.print(kPlain0);
    lcd.setCursor(0, 1);
    lcd.print(kPlain1);
    lcd.setCursor(0, 2);
    lcd.print("                    ");
    lcd.setCursor(0, 3);
    lcd.print("                    ");
    return;
  }

  float v;
  if (t < kRetroFlashFadeInMs)
    v = (float)t / (float)kRetroFlashFadeInMs;
  else if (t < kRetroFlashFadeInMs + kRetroFlashGlitchHoldMs)
    v = 1.f;
  else
    v = 1.f - (float)(t - kRetroFlashFadeInMs - kRetroFlashGlitchHoldMs) / (float)kRetroFlashFadeGlitchMs;

  static const char kGlitch[] = ".:;`'";
  const int nG = (int)sizeof(kGlitch) - 1;

  static const char line0[] = "Entering Rhythm game";
  static const char line1[] = "       mode.       ";

  for (int row = 0; row < 4; row++) {
    lcd.setCursor(0, row);
    if (row <= 1) {
      const char *line = (row == 0) ? line0 : line1;
      for (int col = 0; col < 20; col++) {
        char ch = line[col];
        uint32_t R = lcdRetroFlashHash(t, (unsigned)(row * 32 + col), 0u);
        uint32_t thresh = (uint32_t)(v * 260.f);
        if (thresh > 240u)
          thresh = 240u;
        char out;
        if (ch == ' ') {
          out = ' ';
        } else if ((R & 511u) < thresh) {
          out = ch;
          if (v > 0.78f && (R >> 19) % 52u == 0u)
            out = kGlitch[(int)((R >> 11) % (unsigned)nG)];
        } else {
          out = kGlitch[(int)((R >> 12) % (unsigned)nG)];
        }
        lcd.print(out);
      }
    } else {
      for (int col = 0; col < 20; col++) {
        uint32_t R = lcdRetroFlashHash(t, (unsigned)(row * 64 + col), 1u);
        char c = ' ';
        if (v > 0.4f && (R & 1023u) < (uint32_t)(v * 72.f))
          c = '.';
        lcd.print(c);
      }
    }
  }
}

void lcdRetroMenu(int selectedIdx, const RhythmSongRow *rows, int numRows, uint32_t wallMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (numRows <= 0 || !rows) {
    lcd.print("NO .MP3 IN /RHYTHM");
    lcd.setCursor(0, 1);
    lcd.print("ADD FILES & RESCAN");
    lcd.setCursor(0, 2);
    lcd.print("38+39 1.5S=ENTER ");
    lcd.setCursor(0, 3);
    lcd.print("38+39 3s/4s EXIT   ");
    return;
  }
  lcd.print("16/46 SC R=PLAY     ");
  for (int r = 0; r < 3; r++) {
    int idx = selectedIdx - 1 + r;
    lcd.setCursor(0, 1 + r);
    if (idx < 0 || idx >= numRows) {
      lcd.print("                    ");
      continue;
    }
    lcd.print(idx == selectedIdx ? '>' : ' ');
    char titleSeg[16];
    const unsigned menuTitleCols = 14;
    uint32_t scrollMs = (idx == selectedIdx && strlen(rows[idx].title) > menuTitleCols) ? wallMs : 0;
    unsigned mpc = lcdRetroTitleScrollMsPerCol(rows[idx].title, menuTitleCols);
    lcdRetroTitleScrollSegment(titleSeg, sizeof(titleSeg), rows[idx].title, scrollMs, menuTitleCols, mpc);
    char buf[20];
    snprintf(buf, sizeof(buf), "%s D%u", titleSeg, (unsigned)rows[idx].difficulty);
    buf[19] = 0;
    lcd.print(buf);
  }
}

// Beat lane (20 cols): hearts travel right → left through the entire row, passing
// THROUGH the bracket interior (cols 9, 10, 11).  Only the two rails at cols 8 and 12
// are reserved for the bracket frame; every other column is part of the heart runway.
//
// Each entry in the supplied beat array generates exactly one heart.  At display time
// we walk the array and place each beat at:
//   col = kHitColC + (beatMs - songRelMs) * kCpp / scrollPeriodMs
// The caller supplies a precomputed beat list (chart-time grid) — this is the only
// source of "future" hearts since real-time bass detection only sees the past.  The
// scroll period is chart-derived (constant during a song) so motion is smooth.
static const int  kHitColL      = 8;   // left rail
static const int  kHitColC      = 10;  // strike point — where a heart sits when its onset hits
static const int  kHitColR      = 12;  // right rail
static const char kBeatChar     = '\x01'; // CGRAM slot 1 = heart (defined at play-start)

// 5×8 CGRAM glyphs (bit4 = left pixel, bit0 = right). Shared by play lane + meltdown.
// Heart: filled ▼ with a single pixel cut from the top centre — all 8 rows used for max ink.
static const uint8_t kLcdHeartGlyph[8] = {
    0x1B, // ██░██  flat top, centre pixel out
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x0E, // ░███░
    0x04, // ░░█░░  tip fills bottom row
};
// Hit burst: solid 5×7 block flash — unmistakable, full contrast vs the ▼ heart.
static const uint8_t kLcdBurstGlyph[8] = {
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x1F, // █████
    0x00,
};

static void lcdWriteCgramGlyph(uint8_t slot, const uint8_t *src) {
  uint8_t buf[8];
  memcpy(buf, src, 8);
  lcd.createChar(slot, buf);
}

// Heart shape in CGRAM slot 1 — written when entering RG_PLAYING.
// Explode/burst shape in CGRAM slot 2 — shown when a heart is hit inside the bracket.
// Each beat is placed at: col = kHitColC + (beatMs - songRelMs) * kCpp / scrollPeriodMs
//   beat in the future → col > 10 (right side) → scrolls left as songRelMs advances
//   beat at strike time → col == 10 (reticle centre)
static void lcdRetroDefineBeatChar(void) {
  lcdWriteCgramGlyph(1, kLcdHeartGlyph);
  lcdWriteCgramGlyph(2, kLcdBurstGlyph);
}
static const char kExplodeChar  = '\x02'; // CGRAM slot 2 = burst
// Hit lifecycle on the lane: burst at frozen col, then blink-fade, then gone.
static const uint32_t kHitBurstMs = 250u;
static const uint32_t kHitFadeMs  = 500u;  // blink-fade after burst
static const uint32_t kHitGoneMs  = kHitBurstMs + kHitFadeMs;
// HIT label on the time row uses the burst window only.
static const uint32_t kHitFlashMs = kHitBurstMs;

static void lcdRetroFillBeatStars(char *row, uint32_t scrollPeriodMs, uint32_t songRelMs,
                                  const uint32_t *beats, int nBeats,
                                  uint32_t wallMs,
                                  const LcdConsumedEntry *consumed, int nConsumed) {
  const int W    = 20;
  const int kCpp = 9;

  for (int i = 0; i < W; i++) row[i] = ' ';
  row[W] = '\0';

  if (!beats || nBeats <= 0) return;
  uint32_t period = (scrollPeriodMs > 0u) ? scrollPeriodMs : 1u;
  uint32_t matchFuzz = period / 4u;
  if (matchFuzz < 40u)
    matchFuzz = 40u;

  for (int i = 0; i < nBeats; i++) {
    // Fuzzy match — grid snap can shift beatMs slightly between frames.
    uint32_t hitWall = 0;
    int8_t   hitCol  = -1;
    for (int j = 0; j < nConsumed; j++) {
      uint32_t d = (consumed[j].beatMs > beats[i]) ? (consumed[j].beatMs - beats[i]) : (beats[i] - consumed[j].beatMs);
      if (d <= matchFuzz) {
        hitWall = consumed[j].hitWallMs;
        hitCol  = consumed[j].hitCol;
        break;
      }
    }

    int col;
    if (hitWall != 0) {
      uint32_t age = wallMs - hitWall;
      // Freeze at the column where the heart was when hit (no leftward drift).
      col = (int)hitCol;
      if (col < 0 || col >= W)
        continue;
      if (col == kHitColL || col == kHitColR)
        continue;
      if (age < kHitBurstMs) {
        row[col] = kExplodeChar;
      } else if (age < kHitGoneMs) {
        // Hold solid burst through fade window — no blink (was reading dim/odd on LCD).
        row[col] = kExplodeChar;
      }
      // After fade: stay blank — never redraw the scrolling heart.
      continue;
    }

    int64_t dt   = (int64_t)beats[i] - (int64_t)songRelMs;
    int64_t dCol = (dt * (int64_t)kCpp) / (int64_t)period;
    col          = kHitColC + (int)dCol;

    if (col < 0 || col >= W) continue;
    if (col == kHitColL || col == kHitColR) continue;

    if (row[col] == ' ') row[col] = kBeatChar;
  }
}

// Bracket frame for row 2: vertical rails at cols 8 and 12.  No centre indicator —
// the heart passes through cols 9/10/11 on its way across the row.
static void lcdRetroOverlayHitRails(char *row) {
  row[kHitColL] = '|';
  row[kHitColR] = '|';
}

// Bracket frame for row 3: chevron rails at cols 8 and 12.  Same behaviour as above.
static void lcdRetroOverlayHitWindow(char *row) {
  row[kHitColL] = '>';
  row[kHitColR] = '<';
}

static void lcdRetroPad20(char *out, const char *src) {
  for (int i = 0; i < 20; i++)
    out[i] = (src && src[i]) ? src[i] : ' ';
  out[20] = '\0';
}

// snprintf time line is often <20 chars; pad so lcd.print clears cols 14–19 (e.g. get-ready "111111" leftovers).
// HIT / MISS label flashes for kHitFlashMs; score % stays in its fixed slot.
static void lcdRetroFormatTimeLine(char *row21, uint32_t elapsedMs, uint32_t durationMs, bool paused, int livePct,
                                    uint32_t wallMs, uint32_t lastHitWallMs, uint32_t lastMissWallMs) {
  uint32_t e = elapsedMs / 1000;
  uint32_t d = durationMs / 1000;
  if (d == 0)
    d = 1;
  if (paused) {
    snprintf(row21, 21, "%u:%02u / %u:%02u ||", (unsigned)(e / 60), (unsigned)(e % 60), (unsigned)(d / 60),
             (unsigned)(d % 60));
  } else if (livePct >= 0) {
    bool flashHit  = (lastHitWallMs != 0) && ((wallMs - lastHitWallMs) < kHitFlashMs);
    bool flashMiss = !flashHit && (lastMissWallMs != 0) && ((wallMs - lastMissWallMs) < kHitFlashMs);
    const char *label = flashHit ? "HIT" : (flashMiss ? "MISS" : "   ");
    snprintf(row21, 21, "%u:%02u/%u:%02u %s %3d%%", (unsigned)(e / 60), (unsigned)(e % 60), (unsigned)(d / 60),
             (unsigned)(d % 60), label, livePct);
  } else {
    snprintf(row21, 21, "%u:%02u / %u:%02u   ", (unsigned)(e / 60), (unsigned)(e % 60), (unsigned)(d / 60),
             (unsigned)(d % 60));
  }
  int n = (int)strlen(row21);
  if (n < 0)
    n = 0;
  for (int i = n; i < 20; i++)
    row21[i] = ' ';
  row21[20] = '\0';
}

void lcdRetroResumeCountdown(uint32_t now, uint32_t startMs, uint32_t countEachMs, const char *title, uint32_t elapsedMs,
                             uint32_t durationMs) {
  uint32_t t = now - startMs;
  char row0[21];
  lcdRetroPad20(row0, title);
  char row1[21];
  lcdRetroFormatTimeLine(row1, elapsedMs, durationMs, false, -1, 0u, 0u, 0u);

  char countCh = '3';
  if (t >= countEachMs * 2u)
    countCh = '1';
  else if (t >= countEachMs)
    countCh = '2';

  char fill[21];
  for (int i = 0; i < 20; i++)
    fill[i] = countCh;
  fill[20] = '\0';

  lcd.setCursor(0, 0);
  lcd.print(row0);
  lcd.setCursor(0, 1);
  lcd.print(row1);
  lcd.setCursor(0, 2);
  lcd.print(fill);
  lcd.setCursor(0, 3);
  lcd.print(fill);
}

static uint32_t s_playLastElapsedMs = 0xffffffffu;
static char s_playLastRow0[21];
static char s_playLastRow1[21];
static char s_playLastRow2[21];
static char s_playLastRow3[21];
static bool s_playRowsInit = false;

// ---- 5s hold meltdown easter-egg -------------------------------------------

static char     s_meltdownSnap[4][21];
static bool     s_meltdownSnapReady = false;
static uint32_t s_meltdownRng       = 1;
static int      s_rainHeadY[20];
static int      s_rainSpeed[20];
static uint32_t s_meltdownLastRainMs = 0;
static char     s_meltdownLastDraw[4][21];
static bool     s_meltdownCgramReady  = false;

static const char kBeatCharLocal    = '\x01';
static const char kExplodeCharLocal = '\x02';

static uint32_t meltdownRand(void) {
  s_meltdownRng = s_meltdownRng * 1664525u + 1013904223u;
  return s_meltdownRng;
}

static char meltdownMatrixChar(void) {
  static const char kPool[] = "0123456789#$%&@|:";
  return kPool[meltdownRand() % (sizeof(kPool) - 1u)];
}

static void meltdownDefineRainChar(void) {
  uint8_t rain[8] = { 0x04, 0x04, 0x0E, 0x0E, 0x1F, 0x00, 0x00, 0x00 };
  lcdWriteCgramGlyph(1, kLcdHeartGlyph);
  lcdWriteCgramGlyph(2, kLcdBurstGlyph);
  lcdWriteCgramGlyph(3, rain);
}

static void meltdownPrintRows(char row[4][21]) {
  for (int r = 0; r < 4; r++) {
    if (memcmp(row[r], s_meltdownLastDraw[r], 20) == 0)
      continue;
    lcd.setCursor(0, r);
    for (int c = 0; c < 20; c++)
      lcd.write((uint8_t)row[r][c]);
    memcpy(s_meltdownLastDraw[r], row[r], 21);
  }
}

void lcdRetroMeltdownBegin(void) {
  meltdownDefineRainChar();
  if (s_playRowsInit) {
    memcpy(s_meltdownSnap[0], s_playLastRow0, 21);
    memcpy(s_meltdownSnap[1], s_playLastRow1, 21);
    memcpy(s_meltdownSnap[2], s_playLastRow2, 21);
    memcpy(s_meltdownSnap[3], s_playLastRow3, 21);
    s_meltdownSnapReady = true;
  } else {
    for (int r = 0; r < 4; r++) {
      memset(s_meltdownSnap[r], ' ', 20);
      s_meltdownSnap[r][20] = '\0';
    }
    s_meltdownSnapReady = true;
  }
  s_meltdownRng = millis() | 1u;
  for (int c = 0; c < 20; c++) {
    s_rainHeadY[c] = -(int)(meltdownRand() % 5);
    s_rainSpeed[c] = 1 + (int)(meltdownRand() % 2);
  }
  s_meltdownLastRainMs = 0;
  s_meltdownCgramReady = true;
  for (int r = 0; r < 4; r++) {
    memset(s_meltdownLastDraw[r], 0xff, 20);
    s_meltdownLastDraw[r][20] = '\0';
  }
  memset(s_playLastRow0, ' ', 20);
  memset(s_playLastRow1, ' ', 20);
  memset(s_playLastRow2, ' ', 20);
  memset(s_playLastRow3, ' ', 20);
  s_playLastRow0[20] = s_playLastRow1[20] = s_playLastRow2[20] = s_playLastRow3[20] = '\0';
}

bool lcdRetroHoldMeltdown(uint32_t now, uint32_t startMs) {
  const uint32_t kGlitchMs = 2200u;
  const uint32_t kChaosMs  = 4200u;
  const uint32_t kDisintMs = 5800u;
  const uint32_t kRainMs   = 10500u;
  const char     kRainChar = '\x03';

  uint32_t t = now - startMs;
  char row[4][21];

  if (t < kGlitchMs) {
    float chaos = (float)t / (float)kGlitchMs;
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 20; c++) {
        char base = s_meltdownSnapReady ? s_meltdownSnap[r][c] : ' ';
        uint32_t roll = meltdownRand() % 1000u;
        if (roll < (uint32_t)(chaos * 850.f))
          row[r][c] = meltdownMatrixChar();
        else if (roll < (uint32_t)(chaos * 950.f)) {
          int nc = (c + 1 + (int)(meltdownRand() % 3)) % 20;
          row[r][c] = s_meltdownSnapReady ? s_meltdownSnap[r][nc] : base;
        } else
          row[r][c] = base;
      }
      row[r][20] = '\0';
    }
  } else if (t < kChaosMs) {
    float fill = (float)(t - kGlitchMs) / (float)(kChaosMs - kGlitchMs);
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 20; c++) {
        uint32_t roll = meltdownRand() % 1000u;
        if (roll < (uint32_t)(200.f + fill * 750.f)) {
          uint32_t pick = meltdownRand() % 100u;
          if (pick < 35u)
            row[r][c] = kBeatCharLocal;
          else if (pick < 55u)
            row[r][c] = kExplodeCharLocal;
          else
            row[r][c] = meltdownMatrixChar();
        } else if (s_meltdownSnapReady)
          row[r][c] = s_meltdownSnap[r][c];
        else
          row[r][c] = ' ';
      }
      row[r][20] = '\0';
    }
  } else if (t < kDisintMs) {
    float die = (float)(t - kChaosMs) / (float)(kDisintMs - kChaosMs);
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 20; c++) {
        uint32_t roll = meltdownRand() % 1000u;
        if (roll < (uint32_t)(die * 920.f))
          row[r][c] = ' ';
        else if (roll < (uint32_t)(die * 980.f))
          row[r][c] = (meltdownRand() & 1u) ? kExplodeCharLocal : meltdownMatrixChar();
        else if (roll < 500u)
          row[r][c] = kBeatCharLocal;
        else
          row[r][c] = meltdownMatrixChar();
      }
      row[r][20] = '\0';
    }
  } else if (t < kRainMs) {
    if (s_meltdownLastRainMs == 0 || (now - s_meltdownLastRainMs) >= 55u) {
      s_meltdownLastRainMs = now;
      for (int c = 0; c < 20; c++) {
        s_rainHeadY[c] += s_rainSpeed[c];
        if (s_rainHeadY[c] > 7)
          s_rainHeadY[c] = -(int)(meltdownRand() % 4);
      }
    }
    float wash = (float)(t - kDisintMs) / (float)(kRainMs - kDisintMs);
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 20; c++)
        row[r][c] = ' ';
      row[r][20] = '\0';
    }
    for (int c = 0; c < 20; c++) {
      int head = s_rainHeadY[c];
      for (int trail = 0; trail < 3; trail++) {
        int y = head - trail;
        if (y < 0 || y >= 4)
          continue;
        if (trail == 0)
          row[y][c] = kRainChar;
        else if (wash < 0.55f)
          row[y][c] = meltdownMatrixChar();
        else
          row[y][c] = ' ';
      }
    }
    if (wash > 0.35f) {
      for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 20; c++) {
          if (meltdownRand() % 1000u < (uint32_t)(wash * 700.f))
            row[r][c] = ' ';
          else if (row[r][c] == ' ' && meltdownRand() % 100u < 8u)
            row[r][c] = meltdownMatrixChar();
        }
      }
    }
  } else {
    for (int r = 0; r < 4; r++) {
      memset(row[r], ' ', 20);
      row[r][20] = '\0';
    }
  }

  meltdownPrintRows(row);
  return true;
}

// Snap-back delay after releasing during meltdown: scales with how far the glitch progressed (max 1 s).
uint32_t lcdRetroMeltdownRecoverDurationMs(uint32_t meltdownHeldMs) {
  const uint32_t kMaxRecover = 1000u;
  const uint32_t kMinRecover = 80u;
  const uint32_t kFullGlitchMs = 5800u; // disintegrate phase ≈ peak chaos
  if (meltdownHeldMs >= kFullGlitchMs)
    return kMaxRecover;
  return kMinRecover + (meltdownHeldMs * (kMaxRecover - kMinRecover)) / kFullGlitchMs;
}

static char lcdRecoverGlitchChar(uint32_t seed) {
  static const char kPool[] = "0123456789#$%&@|:";
  return kPool[seed % (sizeof(kPool) - 1u)];
}

static void lcdRetroCorruptRowGlitch(char *row, float glitch, uint32_t wallMs, int rowIdx) {
  if (glitch <= 0.005f)
    return;
  for (int c = 0; c < 20; c++) {
    uint32_t seed = wallMs * 131u + (uint32_t)rowIdx * 17u + (uint32_t)c * 7u;
    uint32_t roll = seed % 1000u;
    if (roll < (uint32_t)(glitch * 900.f))
      row[c] = lcdRecoverGlitchChar(seed);
    else if (roll < (uint32_t)(glitch * 980.f))
      row[c] = ' ';
  }
}

void lcdRetroPlayingInvalidate(void) {
  lcdRetroDefineBeatChar();
  s_playRowsInit = false;
  s_meltdownSnapReady = false;
  s_meltdownCgramReady = false;
  memset(s_playLastRow0, ' ', 20);
  memset(s_playLastRow1, ' ', 20);
  memset(s_playLastRow2, ' ', 20);
  memset(s_playLastRow3, ' ', 20);
  s_playLastRow0[20] = s_playLastRow1[20] = s_playLastRow2[20] = s_playLastRow3[20] = '\0';
}

void lcdRetroPlaying(const char *title, uint32_t elapsedMs, uint32_t durationMs, uint32_t beatPeriodMs, uint32_t wallMs,
                     uint32_t songRelMs, const uint32_t *beats, int nBeats,
                     uint32_t lastHitWallMs, uint32_t lastMissWallMs,
                     const LcdConsumedEntry *consumed, int nConsumed,
                     bool paused, int liveScorePct, float recoverGlitch) {
  if (!s_playRowsInit || elapsedMs < s_playLastElapsedMs) {
    lcdRetroDefineBeatChar();
    // Full clear so the LCD matches the all-spaces cache. Without this, direct
    // writes from get-ready (countdown 1/2/3) leave stale chars in positions
    // that the delta-writer skips because fresh == cached == ' '.
    lcd.clear();
    memset(s_playLastRow0, ' ', 20);
    memset(s_playLastRow1, ' ', 20);
    memset(s_playLastRow2, ' ', 20);
    memset(s_playLastRow3, ' ', 20);
    s_playLastRow0[20] = s_playLastRow1[20] = s_playLastRow2[20] = s_playLastRow3[20] = '\0';
    s_playRowsInit = true;
  }
  s_playLastElapsedMs = elapsedMs;

  char row0[21];
  const unsigned playTitleCols = 20;
  size_t tlen = title ? strlen(title) : 0;
  if (tlen <= playTitleCols) {
    lcdRetroPad20(row0, title);
  } else {
    unsigned mpc = lcdRetroTitleScrollMsPerCol(title, playTitleCols);
    lcdRetroTitleScrollSegment(row0, sizeof(row0), title, elapsedMs, playTitleCols, mpc);
  }

  char row1[21];
  lcdRetroFormatTimeLine(row1, elapsedMs, durationMs, paused, paused ? -1 : liveScorePct, wallMs, lastHitWallMs,
                         lastMissWallMs);

  char row2[21];
  char row3[21];
  if (paused) {
    lcdRetroPad20(row2, "== PAUSED 16+46 3s =");
    lcdRetroPad20(row3, "  HOLD 3s TO RESUME  ");
  } else {
    lcdRetroFillBeatStars(row2, beatPeriodMs, songRelMs, beats, nBeats, wallMs, consumed, nConsumed);
    lcdRetroOverlayHitRails(row2);
    lcdRetroFillBeatStars(row3, beatPeriodMs, songRelMs, beats, nBeats, wallMs, consumed, nConsumed);
    lcdRetroOverlayHitWindow(row3);
  }

  if (recoverGlitch > 0.005f) {
    lcdRetroCorruptRowGlitch(row0, recoverGlitch, wallMs, 0);
    lcdRetroCorruptRowGlitch(row1, recoverGlitch, wallMs, 1);
    lcdRetroCorruptRowGlitch(row2, recoverGlitch, wallMs, 2);
    lcdRetroCorruptRowGlitch(row3, recoverGlitch, wallMs, 3);
  }

  // Per-cell delta update — only writes the columns that actually changed.  Avoids
  // the visible flicker / "dim heart" effect of re-printing the entire 20-char row
  // every frame at 12.5 fps.  Stable cells (rails, time digits, etc.) never blink.
  auto deltaWriteRow = [](int rowIdx, const char *fresh, char *last) {
    int c = 0;
    while (c < 20) {
      if (fresh[c] == last[c]) { c++; continue; }
      int runStart = c;
      while (c < 20 && fresh[c] != last[c]) {
        last[c] = fresh[c];
        c++;
      }
      lcd.setCursor(runStart, rowIdx);
      for (int i = runStart; i < c; i++)
        lcd.write((uint8_t)fresh[i]);
    }
    last[20] = '\0';
  };
  // Beat rows: full-line write with lcd.write so CGRAM slots 1/2 render reliably.
  auto printBeatRow = [](int rowIdx, const char *fresh, char *last) {
    if (memcmp(fresh, last, 20) == 0)
      return;
    lcd.setCursor(0, rowIdx);
    for (int i = 0; i < 20; i++)
      lcd.write((uint8_t)fresh[i]);
    memcpy(last, fresh, 21);
  };
  deltaWriteRow(0, row0, s_playLastRow0);
  deltaWriteRow(1, row1, s_playLastRow1);
  printBeatRow(2, row2, s_playLastRow2);
  printBeatRow(3, row3, s_playLastRow3);
}

void lcdRetroStillTherePrompt(uint32_t now, uint32_t promptStartMs) {
  uint32_t elapsed = now - promptStartMs;
  unsigned remSec = 1;
  if (elapsed < 10000) {
    uint32_t remMs = 10000 - elapsed;
    remSec = (unsigned)((remMs + 999) / 1000);
    if (remSec < 1)
      remSec = 1;
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("STILL THERE?        ");
  lcd.setCursor(0, 1);
  lcd.print("TAP ANY BUTTON      ");
  lcd.setCursor(0, 2);
  lcd.print("                    ");
  lcd.setCursor(0, 3);
  lcd.printf("AUTO-IDLE IN %lus   ", (unsigned long)remSec);
}

void lcdRetroGetReady(uint32_t now, uint32_t getReadyStartMs, const char *songTitle, uint32_t titleScrollMs,
                      uint32_t countEachMs) {
  uint32_t t = now - getReadyStartMs;
  uint32_t scrollElapsed = t;
  char fill[21];
  for (int i = 0; i < 20; i++)
    fill[i] = '.';
  fill[20] = '\0';

  // Avoid lcd.clear() every frame: only full redraw when countdown phase changes; row 0 scrolls in place.
  static uint32_t s_grSession = 0xffffffffu;
  static uint8_t s_lastPhase = 0xff;
  static char s_lastRow0[21];
  if (getReadyStartMs != s_grSession) {
    s_grSession = getReadyStartMs;
    s_lastPhase = 0xff;
    s_lastRow0[0] = '\0';
  }

  uint8_t phase;
  if (t < titleScrollMs)
    phase = 0;
  else if (t < titleScrollMs + countEachMs)
    phase = 3;
  else if (t < titleScrollMs + 2u * countEachMs)
    phase = 2;
  else
    phase = 1;

  char row0[21];
  unsigned grMpc = lcdRetroTitleScrollMsPerCol(songTitle, 20u);
  lcdRetroTitleScrollSegment(row0, sizeof(row0), songTitle, scrollElapsed, 20u, grMpc);

  char countCh = '.';
  if (t >= titleScrollMs + 2u * countEachMs)
    countCh = '1';
  else if (t >= titleScrollMs + countEachMs)
    countCh = '2';
  else if (t >= titleScrollMs)
    countCh = '3';

  bool phaseChanged = (phase != s_lastPhase);
  bool row0Changed = (strncmp(row0, s_lastRow0, 20) != 0);

  if (!phaseChanged && !row0Changed)
    return;

  if (phaseChanged) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(row0);
    memcpy(s_lastRow0, row0, sizeof(s_lastRow0));
    s_lastPhase = phase;

    if (t < titleScrollMs) {
      lcd.setCursor(0, 1);
      lcd.print(fill);
      lcd.setCursor(0, 2);
      lcd.print(fill);
      lcd.setCursor(0, 3);
      lcd.print(fill);
    } else {
      for (int i = 0; i < 20; i++)
        fill[i] = countCh;
      lcd.setCursor(0, 1);
      lcd.print(fill);
      lcd.setCursor(0, 2);
      lcd.print(fill);
      lcd.setCursor(0, 3);
      lcd.print(fill);
    }
    return;
  }

  lcd.setCursor(0, 0);
  lcd.print(row0);
  memcpy(s_lastRow0, row0, sizeof(s_lastRow0));
}

void lcdRetroResultsScore(const char *title, char grade, int mainPct, int bonusPct, int totalPct, uint32_t now) {
  lcd.clear();
  char t0[21];
  lcdRetroTitleScrollSegment(t0, sizeof(t0), title, now, 20, lcdRetroTitleScrollMsPerCol(title, 20u));
  lcd.setCursor(0, 0);
  lcd.print(t0);
  lcd.setCursor(0, 1);
  lcd.printf("M%3d%% B%3d%% T%3d%%", mainPct, bonusPct, totalPct);

  lcd.setCursor(0, 2);
  if (grade == 'F' || grade == 'D')
    lcd.print(":< :/ :-0");
  else if (grade == 'C')
    lcd.print(":| :/ :-?");
  else
    lcd.print(":3 :> !_! :O");

  lcd.setCursor(0, 3);
  lcd.print("FRNT_LEFT=MENU  FRNT_RGHT=NEXT TRK");
}

void lcdRetroResultsPrompt(uint32_t wallMs, const char *nextSongTitle) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PLAY NEXT TRACK?");
  char t1[21];
  lcdRetroTitleScrollSegment(t1, sizeof(t1), nextSongTitle, wallMs, 20,
                             lcdRetroTitleScrollMsPerCol(nextSongTitle, 20u));
  lcd.setCursor(0, 1);
  lcd.print(t1);
  lcd.setCursor(0, 2);
  lcd.print("46=PLAY NEXT TRACK  ");
  lcd.setCursor(0, 3);
  lcd.print("16=BACK TO MENU     ");
}


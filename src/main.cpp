// ESP32-S3 (LILYGO ETH-Lite/Elite) + PCA9685 RGB buttons + LCD + I2S Synth (UDA1334A)
//
// Buttons: LEFT bottom→top = 38,12,5,7 ; FRONT = 16(left),46(right) ; RIGHT bottom→top = 39,2,15,8
// LEDs: same wiring map as we probed earlier (MAP[] below).
// LCD: hd44780_I2Cexp (4x20 ok)
// I2S DAC: UDA1334A  BCLK=26, LRCLK=25, DIN=23
//
// New: 8 side buttons = synth keys (Left column → Left voice, Right column → Right voice)
// Front buttons nudge offsets depending on which side is active:
//  - if any LEFT buttons down: 16 = up, 46 = down (left voice).
//  - if any RIGHT buttons down: 46 = up, 16 = down (right voice).
// Hold 16+46 together → cycle scale.
// Idle LED/LCD mode advances automatically every 2 min when unused; hold GPIO 38+39 ≥1.5s → rhythm game.
// LCD shows scale, note names & offsets live.
//
// LED logic kept: solid while held, 1s tails, centers mirror mixes, idle modes (BREATHE, FLAME_WARM, FLAME_COOL, FLAME_RGB).
// Front press rainbow: 5s sweep with random start hue when side quiet.

#include <Arduino.h>
#include <Wire.h>
#include <esp_random.h>
#include "config.h"
#include "scales.h"
#include "leds.h"
#include "buttons.h"
#include "display.h"
#include "audio.h"
#include "debug.h"
#include "mqtt_link.h"
#include "rhythm_game.h"

// All config definitions are now in config.cpp

// All scale functions are now in scales.cpp

// All LED state variables and functions are now in leds.cpp (exported via leds.h)
// All button functions are now in buttons.cpp (exported via buttons.h)  
// All display functions are now in display.cpp (exported via display.h)

// Removed duplicate function definitions - they're now in the module .cpp files

// All function definitions have been moved to their respective module files:
// - LED functions: leds.cpp
// - Button functions: buttons.cpp  
// - Display functions: display.cpp

// All audio functions are now in audio.cpp
// All debug functions are now in debug.cpp

// ===================== Setup =====================
void setup(){
  Serial.begin(115200);
  delay(3000); // Long delay
  
  Serial.println("\n\n\n=== ESP32-S3 Synth Controller ===");
  Serial.println("Serial initialized successfully!");
  delay(500);
  
  // GPIO testing - uncomment to enable
  // identifyBreakoutPins();
  // testButtonGPIOs();
  // return;

  // Normal operation starts here
  Serial.printf("I2S Pins: BCLK=%d, LRCLK=%d, DATA=%d\n", I2S_BCLK, I2S_LRCK, I2S_DATA);
  
  Wire.begin(I2C_SDA,I2C_SCL,100000);
  Wire.setTimeOut(20);
  Serial.printf("[I2C] Initialized: SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);

  // LCD splash + I2C scan
  Serial.println("[LCD] Initializing...");
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Booting.....");
  delay(200);
  lcd.clear(); lcd.setCursor(0,0); lcd.print("LCD OK");
  Serial.println("[LCD] Initialized OK");
  
  String devs="";
  for(uint8_t a=1;a<127;a++){ Wire.beginTransmission(a); if(Wire.endTransmission()==0){ char b[6]; snprintf(b,sizeof(b),"%02X",a); if(devs.length()) devs+=","; devs+="0x"; devs+=b; } }
  lcd.setCursor(0,1); lcd.print("I2C: "); lcd.print(devs.length()?devs:"none");
  Serial.printf("[I2C] Devices found: %s\n", devs.length()?devs.c_str():"none");

  Serial.println("[MQTT] Starting WiFi/MQTT (solenoid commands)...");
  mqttLinkSetup();

  // PCA init
  Serial.println("[PCA] Initializing PCA9685 drivers...");
  pcaA.begin(); pcaB.begin(); pcaA.setPWMFreq(PCA_FREQ); pcaB.setPWMFreq(PCA_FREQ);
  Serial.printf("[PCA] Driver A (0x%02X): OK, Driver B (0x%02X): OK, Freq=%dHz\n", PCA_A_ADDR, PCA_B_ADDR, PCA_FREQ);
  for(int i=0;i<16;i++){ lastPWM_A[i]=0xFFFF; lastPWM_B[i]=0xFFFF; }
  for(int i=0;i<10;i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
  buttonsRestoreInputPullups();
  Serial.println("[BTN] All button GPIOs configured as INPUT_PULLUP");

  // idle profiles
  randomSeed(esp_random());
  Serial.println("=== Initializing idle RGB profiles ===");
  for(int i=0;i<10;i++){
    idleBaseHue[i]=random(0,360);
    idleRateDegPerSec[i]=8 + (random(0,200)/10.0f);
    rgbBaseHue[i]=random(0,360);
    float base=8 + (random(0,140)/10.0f);
    if (i==IDX_38||i==1||i==2||i==3) base+=2;
    if (i==IDX_11||i==7||i==8||i==9) base-=1;
    rgbHueRateDegPerSec[i]=base;
    rainbowStartHue[i]=random(0,360);
    setLED_RGB(i,0,0,0);
    
    // DEBUG: Print first few LEDs
    if (i < 3) {
      Serial.printf("  LED[%d]: idleBaseHue=%.1f, idleRate=%.2f deg/s\n", 
                    i, idleBaseHue[i], idleRateDegPerSec[i]);
    }
  }
  Serial.printf("  idleMode: %d (change to 0 for RGB idle mode)\n", idleMode);

  buildScaleHz();
  
  Serial.println("Initializing I2S...");
  audioInit();
  Serial.println("I2S init complete (check messages above for errors)");
  
  // Boot-up LED animation: green/turquoise pulsing (overlaps with audio)
  Serial.println("[BOOT] Starting LED boot animation...");
  
  // Play wakeup sequence: power-up sound, then static overlaps and fades out
  if(i2s_initialized){
    lcd.setCursor(0,2); lcd.print("Testing audio...");
    Serial.println("\n[I2S] ===== PLAYING WAKEUP SEQUENCE =====");
    Serial.printf("[I2S] 1. Power-up sound (organ)\n");
    Serial.printf("[I2S] 2. Static overlaps and fades out\n");
    Serial.printf("[I2S] If you don't hear sound, check:\n");
    Serial.printf("[I2S]   1. DAC connections (BCLK, LRCLK, DIN)\n");
    Serial.printf("[I2S]   2. DAC power supply\n");
    Serial.printf("[I2S]   3. Amplifier/speaker connections\n");
    Serial.printf("[I2S]   4. Serial monitor for I2S errors\n");
    
    // Start boot animation and audio together (animation overlaps with audio)
    uint32_t animStartTime = millis();
    const uint32_t PRE_AUDIO_ANIM_MS = 500; // 0.5 seconds before audio starts
    
    // Animation parameters
    const float TAU = 6.28318530718f;
    const float PULSE_RATE = 1.5f;
    const float HUE_START = 150.0f;
    const float HUE_END = 180.0f;
    const float SATURATION = 1.0f;
    const uint8_t START_BUTTONS[] = {0, 6}; // 38 and 11
    const uint8_t OTHER_BUTTONS[] = {1, 2, 3, 4, 5, 7, 8, 9};
    
    // Run animation for PRE_AUDIO_ANIM_MS before audio starts
    while(millis() - animStartTime < PRE_AUDIO_ANIM_MS){
      uint32_t elapsed = millis() - animStartTime;
      float t = elapsed / 1000.0f;
      
      // Pulsing brightness
      float pulse = 0.3f + 0.7f * (0.5f + 0.5f * sinf(TAU * PULSE_RATE * t));
      
      // Phase hue between green and turquoise
      float huePhase = sinf(TAU * 0.3f * t);
      float hue = HUE_START + (HUE_END - HUE_START) * (0.5f + 0.5f * huePhase);
      
      // Start buttons (38 and 11) fade in before audio
      float startButtonBrightness = pulse * (elapsed / (float)PRE_AUDIO_ANIM_MS);
      for(int i = 0; i < 2; i++){
        uint8_t idx = START_BUTTONS[i];
        uint8_t r, g, b;
        hsv2rgb(hue, SATURATION, startButtonBrightness, r, g, b);
        setLED_RGB(idx, r, g, b);
      }
      
      // Other buttons off for now
      for(int i = 0; i < 8; i++){
        setLED_RGB(OTHER_BUTTONS[i], 0, 0, 0);
      }
      
      delay(10);
    }
    
    // Start audio - it will update LEDs during playback (overlapping)
    playWakeupSequence(animStartTime);
    Serial.println("[I2S] ===== WAKEUP SEQUENCE COMPLETE =====\n");
    
    delay(100); // Brief pause after sounds
    
    // Play falling shepard's tone (2 seconds) - DISABLED
    // Serial.println("[I2S] ===== PLAYING FALLING SHEPARD'S TONE =====");
    // playFallingShepardTone(2000);
    // Serial.println("[I2S] ===== SHEPARD'S TONE COMPLETE =====\n");
  } else {
    Serial.println("[I2S] ERROR: Skipping test tone - I2S not initialized!");
    Serial.printf("[I2S] Check serial output above for I2S initialization errors\n");
    lcd.setCursor(0,2); lcd.print("I2S init failed!");
    playBootAnimation(); // Play animation even if audio failed
  }
  
  Serial.println("[BOOT] Boot animation complete");
  
  // Show PYRRISMA and Multi Spectral Console animation after debug lines
  Serial.println("[BOOT] Showing PYRRISMA welcome animation...");
  uint32_t bootWelcomeStart = millis();
  const uint32_t BOOT_WELCOME_DURATION_MS = 4000; // 100ms fade + 3000ms PYRRISMA + 750ms fade out + 150ms buffer
  
  while(millis() - bootWelcomeStart < BOOT_WELCOME_DURATION_MS) {
    lcdWelcomeAnimation(millis(), bootWelcomeStart);
    delay(10); // Small delay to allow LCD updates
  }
  Serial.println("[BOOT] Welcome animation complete");
  
  // Wait 1 second after everything, then set lastPressMs so idle mode can start
  delay(1000);
  lastPressMs=millis();
  lastFlameStepMs=millis();

  lcdPrintStatus(SCALES[scaleIndex].name, 0,0, 0,0);
}

// ===================== Loop =====================
int8_t leftDegOff=0, rightDegOff=0;
uint32_t lastCenterScaleHold=0; bool centerHoldLatched=false;

void loop(){
  // SD scan + SPI can block; keep out of setup() so the task WDT (~5s) does not reset during long network + boot.
  static bool s_rhythmInited = false;
  if (!s_rhythmInited) {
    s_rhythmInited = true;
    rhythmGameSetup();
    buttonsRestoreInputPullups();
  }

  mqttLinkLoop();

  uint32_t now=millis();
  static uint32_t loopCounter = 0;
  static uint32_t modeDisplayStart=0; // When mode name display started (shared across sections)
  static uint32_t waveDisplayStart = 0; // Waveform preview after green hold (LCD ASCII art)
  static uint32_t lastLCD = 0;
  loopCounter++;

  // SD SPI leaves MISO/MOSI/CS as outputs; GPIO 9 (etc.) reads stuck LOW until re-muxed (Lite: skip 11/12 for W5500).
  // Do NOT SPI.end() while MP3 reads from SD — every-loop refresh here caused Card Failed / instant "MP3 finished".
  if (!rhythmGameOwnsAudioOutput())
    buttonsRefreshSdSharedPins();
  
  // scan buttons
  bool anyDown=false;
  for(int i=0;i<10;i++){
    bool ed=false, eu=false;
    bool lvl=readLevelDebounced(BTN_PINS[i], ed, eu);
    edgeDownArr[i]=ed; edgeUpArr[i]=eu; down[i]=lvl;
    if (lvl) anyDown=true;
    if (ed) {
      lastPressMs=now;
      Serial.printf("[BTN] GPIO %d (idx %d) PRESSED\n", BTN_PINS[i], i);
      if (i <= 3 || (i >= 6 && i <= 9))
        mqttSolenoidPulseOnSideColumnPress(i);
    }
    if (eu) {
      releaseTs[i]=now;
      Serial.printf("[BTN] GPIO %d (idx %d) RELEASED\n", BTN_PINS[i], i);
    }
  }

  rhythmGameLoop(now, down, edgeDownArr);
  for (int i = 0; i < 10; i++) {
    if (edgeDownArr[i])
      rhythmGameOnButtonEdge(now);
  }
  
  // Update debug watch variables - buttons
  for(int i=0; i<10; i++) dbgWatch.buttonsDown[i] = down[i];
  dbgWatch.anyButtonDown = anyDown;
  dbgWatch.lastPressTime = lastPressMs;
  dbgWatch.uptimeMs = now;
  dbgWatch.loopCount = loopCounter;

  float wantL = 0, wantR = 0;
  bool fronts = down[IDX_FRONT_L] && down[IDX_FRONT_R];

  if (!rhythmGameSuppressNormalUi()) {
  // ---- SCALES: hold both front (16+46) for 1.5 seconds to switch scale
  
  // Initialize bothHoldStart when both buttons are first pressed
  if (fronts && bothHoldStart==0){
    bothHoldStart=now;
    lastCenterScaleHold=now;
  }
  
  // Reset timers when buttons are released
  if (!fronts){
    bothHoldStart=0;
    lastCenterScaleHold=0;
    centerHoldLatched=false;
    holdLatch=false;
  }
  
  // Scale switching: happens at 1.5 seconds (1500ms)
  if (fronts && !centerHoldLatched && !holdLatch){
    uint32_t holdTime = now - lastCenterScaleHold;
    if (holdTime >= 1500){ // 1.5 second hold
      scaleIndex = (scaleIndex+1) % NUM_SCALES;
      buildScaleHz();
      centerHoldLatched=true;
      Serial.printf("[SCALE] Changed to: %s\n", SCALES[scaleIndex].name);
    }
  }

  // WAVEFORM: hold both GREEN side buttons (GPIO 5 + 9) ~1.5s to cycle SINE/TRI/SOFT/RICH
  static uint32_t bothGreenHoldStart = 0;
  static bool waveHoldLatched = false;
  bool greens = down[IDX_LEFT[2]] && down[IDX_RIGHT[2]];
  if (greens && bothGreenHoldStart == 0)
    bothGreenHoldStart = now;
  if (!greens) {
    bothGreenHoldStart = 0;
    waveHoldLatched = false;
  }
  if (greens && !waveHoldLatched && bothGreenHoldStart != 0 && (now - bothGreenHoldStart) >= 1500) {
    audioCycleWaveShape();
    waveHoldLatched = true;
    Serial.printf("[WAVE] %s\n", audioWaveShapeName(audioGetWaveShape()));
    waveDisplayStart = now;
    lcdPrintWaveShapePreview(audioGetWaveShape());
    lastLCD = now;
  }

  // ---- Which side is active?
  bool leftSideActive=false, rightSideActive=false;
  for(int k=0;k<4;k++){ if (down[IDX_LEFT[k]]) leftSideActive=true; }
  for(int k=0;k<4;k++){ if (down[IDX_RIGHT[k]]) rightSideActive=true; }

  // ---- Front pitch shift logic:
  // 16 (IDX_FRONT_L) = shift DOWN for both sides
  // 46 (IDX_FRONT_R) = shift UP for both sides
  // Continuous shifting while button is held (no clicking)
  static uint32_t lastPitchShiftTime = 0;
  const uint32_t PITCH_SHIFT_INTERVAL_MS = 122; // Shift every 122ms (23% faster than 150ms)
  
  if (leftSideActive){
    if (down[IDX_FRONT_L] && (now - lastPitchShiftTime >= PITCH_SHIFT_INTERVAL_MS)) { 
      // 16 held: shift left side DOWN continuously
      if (leftDegOff > -24) {
        leftDegOff--;
        lastPitchShiftTime = now;
        Serial.printf("[PITCH] Left offset: %+d (shifting down)\n", leftDegOff);
      }
    }
    if (down[IDX_FRONT_R] && (now - lastPitchShiftTime >= PITCH_SHIFT_INTERVAL_MS)) { 
      // 46 held: shift left side UP continuously
      if (leftDegOff <  24) {
        leftDegOff++;
        lastPitchShiftTime = now;
        Serial.printf("[PITCH] Left offset: %+d (shifting up)\n", leftDegOff);
      }
    }
  }
  if (rightSideActive){
    if (down[IDX_FRONT_L] && (now - lastPitchShiftTime >= PITCH_SHIFT_INTERVAL_MS)) { 
      // 16 held: shift right side DOWN continuously
      if (rightDegOff > -24) {
        rightDegOff--;
        lastPitchShiftTime = now;
        Serial.printf("[PITCH] Right offset: %+d (shifting down)\n", rightDegOff);
      }
    }
    if (down[IDX_FRONT_R] && (now - lastPitchShiftTime >= PITCH_SHIFT_INTERVAL_MS)) { 
      // 46 held: shift right side UP continuously
      if (rightDegOff <  24) {
        rightDegOff++;
        lastPitchShiftTime = now;
        Serial.printf("[PITCH] Right offset: %+d (shifting up)\n", rightDegOff);
      }
    }
  }
  
  // ---- Compute target freqs from side buttons
  // left column: take highest (topmost) pressed as priority; else lowest tail later
  for(int s=3;s>=0;s--){ int i=IDX_LEFT[s];
    if (down[i]){ int baseIdx = degreeIndexForSlot(s) + leftDegOff; wantL = scaleHzAtIdx(baseIdx); break; }
  }
  for(int s=3;s>=0;s--){ int i=IDX_RIGHT[s];
    if (down[i]){ int baseIdx = degreeIndexForSlot(s) + rightDegOff; wantR = scaleHzAtIdx(baseIdx); break; }
  }

  } // !rhythmGameSuppressNormalUi
  
  // Generate rising/falling tone when pitch shifting (after computing base frequencies)
  // The pitch naturally rises/falls as leftDegOff/rightDegOff change continuously
  // No need for separate tone - the continuous shifting creates the effect
  
  if (rhythmGameShouldSilenceSynth()) {
    wantL = 0;
    wantR = 0;
  }

  // Update debug watch variables - audio
  dbgWatch.targetFreqL = wantL;
  dbgWatch.targetFreqR = wantR;
  dbgWatch.currentFreqL = curFreqL;
  dbgWatch.currentFreqR = curFreqR;
  dbgWatch.ampLeft = ampL;
  dbgWatch.ampRight = ampR;
  dbgWatch.currentScaleIndex = scaleIndex;
  dbgWatch.leftOffset = leftDegOff;
  dbgWatch.rightOffset = rightDegOff;
  
  // DEBUG: Print frequency changes
  static float lastWantL=0, lastWantR=0;
  static uint32_t lastFreqDebug=0;
  if ((wantL != lastWantL || wantR != lastWantR) && (now - lastFreqDebug > 100)){
    if (wantL > 0 || wantR > 0){
      Serial.printf("[AUDIO] L=%.1fHz (%s) R=%.1fHz (%s)\n", 
                    wantL, noteNameFromHz(wantL), wantR, noteNameFromHz(wantR));
    }
    lastWantL=wantL; lastWantR=wantR;
    lastFreqDebug=now;
  }

  // Idle mode (used by LCD + LED when not in rhythm UI)
  bool idle = (!anyDown) && (now - lastPressMs > IDLE_AFTER_MS);

  // Auto-advance idle LED/LCD mode every 2 minutes while unused (no rhythm UI).
  static uint32_t nextIdleAutoAdvanceMs = 0;
  if (!idle || rhythmGameIsActive()) {
    nextIdleAutoAdvanceMs = 0;
  } else if (nextIdleAutoAdvanceMs == 0) {
    nextIdleAutoAdvanceMs = now + 120000u;
  } else if ((int32_t)(now - nextIdleAutoAdvanceMs) >= 0) {
    idleMode = (idleMode + 1) % 12;
    modeDisplayStart = now;
    nextIdleAutoAdvanceMs = now + 120000u;
    static const char *const kIdleModeNames[] = {"RGB Breathe", "Warm Flame", "Cool Flame", "RGB+Flame", "Solid Colors",
                                                   "Rainbow Wave", "Aurora", "Starlight", "Gradient Flow", "Matrix Rain",
                                                   "Lightning Strike", "Plasma Swirl"};
    Serial.printf("[IDLE] Auto-advance (2 min idle): %d (%s)\n", idleMode, kIdleModeNames[idleMode]);
  }

  // ========= LED rendering (same behavior as before) =========
  // centers mix their sides (incl. tails) in non-idle; rainbow when held & side quiet
  float mixLr,mixLg,mixLb,sumL, mixRr,mixRg,mixRb,sumR;
  mixGroupColors(now, IDX_LEFT,4, mixLr,mixLg,mixLb,sumL);
  mixGroupColors(now, IDX_RIGHT,4, mixRr,mixRg,mixRb,sumR);
  
  // ---- LCD update with animations
  static uint8_t lcdAnimationMode = 1; // 0=status, 1=sparkle+glitch animation
  static uint32_t lastAnimationSwitch = 0;
  static uint8_t displayedMode=255; // Which mode is being displayed
  // modeDisplayStart is declared at function scope above
  
  // Check if we should show mode name (1 second after mode change, matching cycle time)
  bool showModeName = false;
  if(modeDisplayStart > 0 && (now - modeDisplayStart) < 1000){
    showModeName = true;
    displayedMode = idleMode;
  } else if(modeDisplayStart > 0 && (now - modeDisplayStart) >= 1000){
    modeDisplayStart = 0; // Clear after 1 second
  }
  
  // Always show animation when idle (no switching needed - just one cool animation)
  // But skip normal status if welcome animation is playing
  static bool welcomeAnimTriggered=false;
  static bool lastIdleStateForWelcome=false;
  static uint32_t idleStartTime = 0;
  
  if(!idle){
    // Check if we just exited idle (transition from idle to active)
    if(lastIdleStateForWelcome && !idle && !welcomeAnimTriggered){
      // Check if idle duration was >= 25 seconds
      if(idleStartTime > 0){
        uint32_t idleDuration = now - idleStartTime;
        if(idleDuration >= 25000){ // 25 seconds
          lcdWelcomeAnimationBeginSession();
          welcomeAnimTriggered = true;
          Serial.printf("[LCD] Triggering welcome animation (idle was %d ms)\n", idleDuration);
        } else {
          Serial.printf("[LCD] Skipping PYRRISMA (idle was only %d ms, need 25000)\n", idleDuration);
        }
        idleStartTime = 0; // Reset
      }
    }
    lastIdleStateForWelcome = idle;
    
    // Show welcome animation if triggered, otherwise show normal status
    if(welcomeAnimTriggered){
      static uint32_t welcomeStart = 0;
      if(welcomeStart == 0) welcomeStart = now;
      lcdWelcomeAnimation(now, lastPressMs);
      // Must exceed 100 + PYRRISMA_DURATION_MS (3000) + PYRRISMA_FADE_OUT_MS (750) in display.cpp (~3850ms)
      if(now - welcomeStart > 4200){
        welcomeAnimTriggered = false;
        welcomeStart = 0;
        lcd.backlight();
      }
    } else {
      lcdAnimationMode = 0;
      lcdClearAnimation(); // Reset animation when becoming active
    }
  } else {
    // Track when idle started
    if(!lastIdleStateForWelcome){
      idleStartTime = now;
    }
    lastIdleStateForWelcome = idle;
    lcdAnimationMode = 1; // Show sparkle+glitch animation when idle
  }
  
  // Check if we should show scale selection (when holding 16+46 for 1.5s+)
  static uint32_t scaleDisplayStart = 0;
  bool showScaleSelection = false;
  if(fronts){
    uint32_t holdTime = now - lastCenterScaleHold;
    if(holdTime >= 1500){
      // Start showing scale selection when hold reaches 1.5s
      if(scaleDisplayStart == 0) scaleDisplayStart = now;
      // Show for 3 seconds total (while holding or after release)
      showScaleSelection = true;
    }
  } else {
    // Continue showing for 3 seconds after release
    if(scaleDisplayStart > 0){
      if((now - scaleDisplayStart) < 3000){
        showScaleSelection = true;
      } else {
        scaleDisplayStart = 0; // Reset after 3 seconds
      }
    }
  }

  bool showWaveSelection = false;
  if (waveDisplayStart > 0) {
    if ((now - waveDisplayStart) < 3000)
      showWaveSelection = true;
    else
      waveDisplayStart = 0;
  }
  
  // Rhythm UI throttles inside rhythmGameDrawLcd; do not run LCD branch every loop (was starving MP3 + causing flicker).
  uint32_t lcdPollMs = idle ? 100 : 500;
  if (rhythmGameIsActive())
    lcdPollMs = 50;
  if (now - lastLCD > lcdPollMs) {
    if (rhythmGameDrawLcd(now)) {
      lastLCD = now;
    } else if (!rhythmGameIsActive() && showScaleSelection){
      // Show scale selection display
      lcdPrintScaleSelection(scaleIndex, now);
    } else if(!rhythmGameIsActive() && showWaveSelection){
      lcdPrintWaveShapePreview(audioGetWaveShape());
    } else if(!rhythmGameIsActive() && showModeName && idle){
      // Show mode name for 1 second after mode change (works when idle or cycling)
      const char* modeNames[] = {"RGB Breathe", "Warm Flame", "Cool Flame", "RGB+Flame", "Solid Colors", 
                                  "Rainbow Wave", "Aurora", "Starlight", "Gradient Flow", "Matrix Rain",
                                  "Lightning Strike", "Plasma Swirl"};
      lcd.clear();
      lcd.setCursor(0, 1);
      lcd.print("Idle Mode:");
      lcd.setCursor(0, 2);
      lcd.print(modeNames[displayedMode]);
      // Show mode number on bottom
      char modeStr[20];
      snprintf(modeStr, sizeof(modeStr), "Mode %d/11", displayedMode);
      lcd.setCursor(0, 3);
      lcd.print(modeStr);
    } else if(!rhythmGameIsActive() && idle && lcdAnimationMode > 0){
      // Randomly select LCD animation (includes glitch text + new animations)
      static uint32_t lastAnimationSwitch = 0;
      static uint8_t currentLCDAnim = 0;
      const uint32_t ANIMATION_SWITCH_INTERVAL = 4000; // Switch every 4 seconds
      
      // Switch animation randomly
      if(now - lastAnimationSwitch > ANIMATION_SWITCH_INTERVAL){
        // Randomly pick: 0=glitch text, 4=particle explosion, 5=wave, 6=kaleidoscope, 8=pulsing, 9=beat grid, 10=pyramid
        // 11=hieroglyphic scroll, 12=cyrillic scroll, 13=factory industrial, 14=soviet slogan, 15=matrix cyrillic
        uint8_t anims[] = {0, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        currentLCDAnim = anims[random(0, sizeof(anims)/sizeof(anims[0]))];
        lastAnimationSwitch = now;
      }
      
      // Call selected animation (pass idleStartTime for console text sequencing)
      uint32_t currentIdleStartTime = (idleStartTime > 0) ? idleStartTime : now;
      switch(currentLCDAnim){
        case 0:
          lcdGlitchTextAnimation(now, currentIdleStartTime); // Glitch text (includes sparkles)
          break;
        case 4:
          lcdParticleExplosion(now, currentIdleStartTime);
          break;
        case 5:
          lcdWaveVisualization(now, currentIdleStartTime);
          break;
        case 6:
          lcdKaleidoscope(now, currentIdleStartTime);
          break;
        case 8:
          lcdPulsingPatterns(now, currentIdleStartTime);
          break;
        case 9:
          lcdBeatGrid(now, currentIdleStartTime);
          break;
        case 10:
          lcdPyramidAnimation(now, currentIdleStartTime);
          break;
        case 11:
          lcdHieroglyphicScroll(now, currentIdleStartTime);
          break;
        case 12:
          lcdCyrillicTextScroll(now, currentIdleStartTime);
          break;
        case 13:
          lcdFactoryIndustrialText(now, currentIdleStartTime);
          break;
        case 14:
          lcdSovietSloganStyle(now, currentIdleStartTime);
          break;
        case 15:
          lcdMatrixCyrillic(now, currentIdleStartTime);
          break;
        default:
          lcdGlitchTextAnimation(now, currentIdleStartTime);
          break;
      }
    } else if (!rhythmGameIsActive()) {
      // Show welcome animation if triggered, otherwise show normal status
      if(welcomeAnimTriggered){
        lcdWelcomeAnimation(now, lastPressMs);
      } else {
        lcdPrintStatus(SCALES[scaleIndex].name, wantL, wantR, leftDegOff, rightDegOff);
      }
    }
    lastLCD=now;
  }
  
  // Update debug watch variables - idle state
  dbgWatch.isIdle = idle;
  dbgWatch.idleModeValue = idleMode;
  dbgWatch.timeSinceLastPress = now - lastPressMs;
  dbgWatch.i2sOk = i2s_initialized;
  dbgWatch.i2sErrorCount = i2s_consec_errors;
  dbgWatch.led0_r = lastR[0];
  dbgWatch.led0_g = lastG[0];
  dbgWatch.led0_b = lastB[0];
  
  // DEBUG: Print idle state changes
  static bool lastIdleStateForDebug=false;
  if (idle != lastIdleStateForDebug){
    Serial.printf("[IDLE] State changed: %s (mode=%d)\n", idle?"IDLE":"ACTIVE", idleMode);
    lastIdleStateForDebug=idle;
  }

  if (rhythmGameIsActive()) {
    uint8_t beatFrL = 0, beatFgL = 0, beatFbL = 0, beatFrR = 0, beatFgR = 0, beatFbR = 0;
    uint8_t musicSideL[4][3], musicSideR[4][3];
    uint8_t resR = 0, resG = 0, resB = 0;
    uint8_t resFrontR = 0, resFrontG = 0, resFrontB = 0;
    const bool beatFrontLeds = rhythmGameOwnsAudioOutput();
    const bool resultsAmbient = rhythmGameResultsAmbientLedsActive();
    if (beatFrontLeds) {
      rhythmGameGetFrontPlayingLeds(now, beatFrL, beatFgL, beatFbL, beatFrR, beatFgR, beatFbR);
      rhythmGameGetSideColumnMusicLeds(now, musicSideL, musicSideR);
    }
    if (resultsAmbient) {
      rhythmGameGetResultsMoodRgb(now, resR, resG, resB);
      rhythmGameGetResultsFrontRgb(resFrontR, resFrontG, resFrontB);
    }
    // Same press colors + release tail as normal play (was all-off, which hid feedback during rhythm).
    for (int i = 0; i < 10; i++) {
      uint8_t r = 0, g = 0, b = 0;
      if (down[i]) {
        uint8_t pr, pg, pb;
        getPressColorForGPIO(BTN_PINS[i], pr, pg, pb);
        r = (uint8_t)lroundf(pr * PRESS_V);
        g = (uint8_t)lroundf(pg * PRESS_V);
        b = (uint8_t)lroundf(pb * PRESS_V);
        lastR[i] = r;
        lastG[i] = g;
        lastB[i] = b;
      } else {
        uint32_t dt = now - releaseTs[i];
        if (dt < PRESS_FADE_MS) {
          float w = 1.0f - (dt / (float)PRESS_FADE_MS);
          r = (uint8_t)lroundf(lastR[i] * w);
          g = (uint8_t)lroundf(lastG[i] * w);
          b = (uint8_t)lroundf(lastB[i] * w);
        } else if (resultsAmbient) {
          if (i == IDX_FRONT_L || i == IDX_FRONT_R) {
            r = resFrontR;
            g = resFrontG;
            b = resFrontB;
          } else {
            r = resR;
            g = resG;
            b = resB;
          }
        }
      }
      if (beatFrontLeds && i == IDX_FRONT_L && !down[i]) {
        r = beatFrL;
        g = beatFgL;
        b = beatFbL;
      } else if (beatFrontLeds && i == IDX_FRONT_R && !down[i]) {
        r = beatFrR;
        g = beatFgR;
        b = beatFbR;
      } else if (beatFrontLeds && !down[i]) {
        for (int k = 0; k < 4; k++) {
          if (i == IDX_LEFT[k]) {
            r = musicSideL[k][0];
            g = musicSideL[k][1];
            b = musicSideL[k][2];
            break;
          }
          if (i == IDX_RIGHT[k]) {
            r = musicSideR[k][0];
            g = musicSideR[k][1];
            b = musicSideR[k][2];
            break;
          }
        }
      }
      setLED_RGB(i, r, g, b);
    }
  } else {
  // flame stepping for idle flame modes (also when cycling modes)
  // Only show idle modes if truly idle (no buttons pressed) OR cycling modes (but not if buttons pressed)
  // If buttons are pressed, prefer showing RGBY colors on pressed buttons
  bool showingIdleModes = (idle && !anyDown);
  if (showingIdleModes && (idleMode==1 || idleMode==2 || idleMode==3) && (now - lastFlameStepMs >= FLAME_UPDATE_MS)){
    lastFlameStepMs = now; flameStep(flameL); flameStep(flameR);
  }

  // IDLE render (also render when cycling through modes)
  // Only show idle modes if no buttons are pressed (prefer RGBY colors on pressed buttons)
  if (showingIdleModes){
    if (idleMode==1 || idleMode==2){
      bool cool=(idleMode==2);
      // fronts subtle
      uint8_t rr,gg,bb; if(!cool){ warmManualRGB(0.25f,rr,gg,bb);} else { hsv2rgb(250.0f,1.0f,0.22f,rr,gg,bb);}
      setLED_RGB(IDX_FRONT_L,rr,gg,bb); setLED_RGB(IDX_FRONT_R,rr,gg,bb);
      // left col
      for(int k=0;k<4;k++){ int layer= flipLeft ? (3-k):k; uint8_t r,g,b; flameToRGB(flameL[layer], cool,r,g,b);
        if (IDX_LEFT[k]==IDX_38) applyVFloor(r,g,b,0.60f); setLED_RGB(IDX_LEFT[k], r,g,b); }
      // right col
      for(int k=0;k<4;k++){ int layer= flipRight ? (3-k):k; uint8_t r,g,b; flameToRGB(flameR[layer], cool,r,g,b);
        if (IDX_RIGHT[k]==IDX_11) applyVFloor(r,g,b,0.60f); setLED_RGB(IDX_RIGHT[k], r,g,b); }
    } else if (idleMode==3){
      float tsec=now/1000.0f;
      for(int idx : {IDX_FRONT_L, IDX_FRONT_R}){
        float hue=rgbBaseHue[idx]+rgbHueRateDegPerSec[idx]*tsec; uint8_t r,g,b; hsv2rgb(hue,1.0f,0.22f,r,g,b); setLED_RGB(idx,r,g,b);
      }
      for(int k=0;k<4;k++){
        int layer= flipLeft ? (3-k):k; int led=IDX_LEFT[k];
        float hue=rgbBaseHue[led]+rgbHueRateDegPerSec[led]*tsec; float v=0.18f+0.80f*powf(flameL[layer],1.25f);
        uint8_t r,g,b; hsv2rgb(hue,1.0f,v,r,g,b); if (led==IDX_38) applyVFloor(r,g,b,0.50f); setLED_RGB(led,r,g,b);
      }
      for(int k=0;k<4;k++){
        int layer= flipRight ? (3-k):k; int led=IDX_RIGHT[k];
        float hue=rgbBaseHue[led]+rgbHueRateDegPerSec[led]*tsec; float v=0.18f+0.80f*powf(flameR[layer],1.25f);
        uint8_t r,g,b; hsv2rgb(hue,1.0f,v,r,g,b); if (led==IDX_11) applyVFloor(r,g,b,0.50f); setLED_RGB(led,r,g,b);
      }
    } else if (idleMode==0){
      // RGB IDLE MODE (mode 0) - this is the breathe mode
      float tsec=now/1000.0f;
      float lfo=0.5f*(1.0f+sinf(2*M_PI*IDLE_LFO_HZ*tsec));
      float idleV=IDLE_V_MIN + (IDLE_V_MAX-IDLE_V_MIN)*lfo;
      
      for(int i=0;i<10;i++){ 
        uint8_t r,g,b; 
        float hue=idleBaseHue[i]+idleRateDegPerSec[i]*tsec; 
        // Normalize hue to 0-360
        while(hue < 0) hue += 360.0f;
        while(hue >= 360.0f) hue -= 360.0f;
        hsv2rgb(hue,IDLE_SAT,idleV,r,g,b); 
        setLED_RGB(i,r,g,b); 
      }
    } else if (idleMode==4){
      // SOLID COLORS MODE (mode 4) - solid RGB colors like when buttons are pressed
      // Left column: yellow, blue, green, red (top to bottom)
      // Right column: yellow, blue, green, red (top to bottom)
      // Front buttons: dim white
      for(int k=0;k<4;k++){
        int leftIdx = IDX_LEFT[k];
        int rightIdx = IDX_RIGHT[k];
        uint8_t rL,gL,bL, rR,gR,bR;
        getPressColorForGPIO(BTN_PINS[leftIdx], rL, gL, bL);
        getPressColorForGPIO(BTN_PINS[rightIdx], rR, gR, bR);
        // Dim them a bit for idle
        setLED_RGB(leftIdx, (uint8_t)(rL*0.6f), (uint8_t)(gL*0.6f), (uint8_t)(bL*0.6f));
        setLED_RGB(rightIdx, (uint8_t)(rR*0.6f), (uint8_t)(gR*0.6f), (uint8_t)(bR*0.6f));
      }
      // Front buttons: dim white
      setLED_RGB(IDX_FRONT_L, 100, 100, 100);
      setLED_RGB(IDX_FRONT_R, 100, 100, 100);
    } else if (idleMode==5){
      // RAINBOW WAVE MODE (mode 5) - wave flows bottom-to-top on each side
      float tsec=now/1000.0f;
      float waveSpeed = 0.8f; // waves per second
      float waveLength = 2.0f; // spacing between waves
      
      // Left column - wave from bottom to top
      for(int k=0;k<4;k++){
        int led=IDX_LEFT[k];
        float pos = (float)k / 3.0f; // 0.0 (bottom) to 1.0 (top)
        float wave = sinf(2*M_PI*(waveSpeed*tsec - pos*waveLength));
        float v = 0.15f + 0.25f * (wave * 0.5f + 0.5f); // 0.15 to 0.40
        float hue = fmodf(180.0f + pos*120.0f + tsec*30.0f, 360.0f); // Blue-green gradient
        uint8_t r,g,b; hsv2rgb(hue, 1.0f, v, r, g, b);
        setLED_RGB(led, r, g, b);
      }
      
      // Right column - wave from bottom to top (offset for visual interest)
      for(int k=0;k<4;k++){
        int led=IDX_RIGHT[k];
        float pos = (float)k / 3.0f;
        float wave = sinf(2*M_PI*(waveSpeed*tsec - pos*waveLength + 0.5f)); // 180° phase offset
        float v = 0.15f + 0.25f * (wave * 0.5f + 0.5f);
        float hue = fmodf(300.0f + pos*60.0f + tsec*30.0f, 360.0f); // Magenta-red gradient
        uint8_t r,g,b; hsv2rgb(hue, 1.0f, v, r, g, b);
        setLED_RGB(led, r, g, b);
      }
      
      // Front buttons - subtle pulsing
      float frontWave = sinf(2*M_PI*waveSpeed*tsec);
      float frontV = 0.20f + 0.15f * (frontWave * 0.5f + 0.5f);
      uint8_t r,g,b; hsv2rgb(fmodf(tsec*20.0f, 360.0f), 0.8f, frontV, r, g, b);
      setLED_RGB(IDX_FRONT_L, r, g, b);
      setLED_RGB(IDX_FRONT_R, r, g, b);
      
    } else if (idleMode==6){
      // AURORA MODE (mode 6) - soft flowing gradients like northern lights
      float tsec=now/1000.0f;
      float auroraSpeed = 0.3f; // slow drift
      
      // Left column - aurora effect
      for(int k=0;k<4;k++){
        int led=IDX_LEFT[k];
        float pos = (float)k / 3.0f;
        // Multiple sine waves for aurora-like movement
        float wave1 = sinf(2*M_PI*(auroraSpeed*tsec*0.7f + pos*1.5f));
        float wave2 = sinf(2*M_PI*(auroraSpeed*tsec*1.3f + pos*2.0f + 0.5f));
        float wave3 = sinf(2*M_PI*(auroraSpeed*tsec*0.5f - pos*1.2f));
        float combined = (wave1 + wave2*0.6f + wave3*0.4f) / 2.0f;
        float v = 0.12f + 0.20f * (combined * 0.5f + 0.5f); // Soft brightness
        // Aurora colors: green-cyan-blue range
        float hue = fmodf(150.0f + combined*40.0f + pos*30.0f, 360.0f);
        uint8_t r,g,b; hsv2rgb(hue, 0.7f + combined*0.3f, v, r, g, b);
        setLED_RGB(led, r, g, b);
      }
      
      // Right column - similar but offset
      for(int k=0;k<4;k++){
        int led=IDX_RIGHT[k];
        float pos = (float)k / 3.0f;
        float wave1 = sinf(2*M_PI*(auroraSpeed*tsec*0.8f + pos*1.3f + 1.0f));
        float wave2 = sinf(2*M_PI*(auroraSpeed*tsec*1.1f + pos*1.8f + 0.3f));
        float wave3 = sinf(2*M_PI*(auroraSpeed*tsec*0.6f - pos*1.0f + 0.7f));
        float combined = (wave1 + wave2*0.6f + wave3*0.4f) / 2.0f;
        float v = 0.12f + 0.20f * (combined * 0.5f + 0.5f);
        float hue = fmodf(180.0f + combined*50.0f + pos*25.0f, 360.0f); // Cyan-purple range
        uint8_t r,g,b; hsv2rgb(hue, 0.7f + combined*0.3f, v, r, g, b);
        setLED_RGB(led, r, g, b);
      }
      
      // Front buttons - gentle aurora glow
      float frontWave = sinf(2*M_PI*auroraSpeed*tsec*0.5f);
      float frontV = 0.15f + 0.10f * (frontWave * 0.5f + 0.5f);
      uint8_t r,g,b; hsv2rgb(fmodf(160.0f + frontWave*30.0f, 360.0f), 0.6f, frontV, r, g, b);
      setLED_RGB(IDX_FRONT_L, r, g, b);
      setLED_RGB(IDX_FRONT_R, r, g, b);
      
    } else if (idleMode==7){
      // STARLIGHT MODE (mode 7) - random twinkling stars
      static uint32_t lastTwinkle[10] = {0};
      static float twinkleBrightness[10] = {0};
      static uint32_t twinkleDuration[10] = {0};
      
      float tsec=now/1000.0f;
      
      // Update twinkles
      for(int i=0;i<10;i++){
        if(twinkleDuration[i] == 0 || (now - lastTwinkle[i] > twinkleDuration[i])){
          // Start new twinkle
          if(random(0,100) < 15){ // 15% chance per check
            twinkleBrightness[i] = 0.0f;
            twinkleDuration[i] = 500 + random(0,1500); // 0.5-2 seconds
            lastTwinkle[i] = now;
          }
        }
        
        // Update brightness (fade in then out)
        if(twinkleDuration[i] > 0){
          float elapsed = (float)(now - lastTwinkle[i]);
          float progress = elapsed / (float)twinkleDuration[i];
          if(progress < 0.3f){
            // Fade in
            twinkleBrightness[i] = progress / 0.3f;
          } else if(progress < 1.0f){
            // Fade out
            twinkleBrightness[i] = 1.0f - (progress - 0.3f) / 0.7f;
          } else {
            twinkleBrightness[i] = 0.0f;
            twinkleDuration[i] = 0;
          }
        }
      }
      
      // Render starlight
      for(int i=0;i<10;i++){
        float baseV = 0.08f; // Very dim base
        float twinkleV = baseV + twinkleBrightness[i] * 0.35f; // Bright twinkle
        // Random hue per LED, but consistent
        float hue = idleBaseHue[i] + tsec * 5.0f; // Slow color drift
        while(hue >= 360.0f) hue -= 360.0f;
        uint8_t r,g,b; hsv2rgb(hue, 0.9f, twinkleV, r, g, b);
        setLED_RGB(i, r, g, b);
      }
    } else if (idleMode==8){
      // GRADIENT FLOW MODE (mode 8) - colors flow from left to right across the pyramid
      float tsec=now/1000.0f;
      float flowSpeed = 0.5f; // cycles per second
      float flowPhase = fmodf(tsec * flowSpeed, 1.0f); // 0.0 to 1.0
      
      // Left column - gradient flows bottom to top
      for(int k=0;k<4;k++){
        int led=IDX_LEFT[k];
        float pos = (float)k / 3.0f; // 0.0 (bottom) to 1.0 (top)
        // Gradient position: flows from left side (0.0) to right side (1.0)
        float gradientPos = flowPhase + (1.0f - pos) * 0.3f; // Bottom gets gradient first
        gradientPos = fmodf(gradientPos, 1.0f);
        // Create smooth gradient using sine wave
        float gradient = sinf(gradientPos * M_PI);
        float v = 0.10f + 0.30f * gradient * gradient; // Smooth brightness curve
        // Color shifts across spectrum as it flows
        float hue = fmodf(240.0f + gradientPos * 120.0f + tsec * 10.0f, 360.0f); // Blue to magenta flow
        uint8_t r,g,b; hsv2rgb(hue, 0.9f, v, r, g, b);
        setLED_RGB(led, r, g, b);
      }
      
      // Right column - gradient flows top to bottom (opposite direction)
      for(int k=0;k<4;k++){
        int led=IDX_RIGHT[k];
        float pos = (float)k / 3.0f; // 0.0 (bottom) to 1.0 (top)
        // Reverse flow direction for visual interest
        float gradientPos = (1.0f - flowPhase) + pos * 0.3f;
        gradientPos = fmodf(gradientPos, 1.0f);
        float gradient = sinf(gradientPos * M_PI);
        float v = 0.10f + 0.30f * gradient * gradient;
        float hue = fmodf(60.0f + gradientPos * 120.0f + tsec * 10.0f, 360.0f); // Yellow to cyan flow
        uint8_t r,g,b; hsv2rgb(hue, 0.9f, v, r, g, b);
        setLED_RGB(led, r, g, b);
      }
      
      // Front buttons - blend of both sides
      float frontGradient = sinf(flowPhase * M_PI);
      float frontV = 0.15f + 0.20f * frontGradient * frontGradient;
      float frontHue = fmodf(180.0f + flowPhase * 180.0f + tsec * 15.0f, 360.0f);
      uint8_t r,g,b; hsv2rgb(frontHue, 0.8f, frontV, r, g, b);
      setLED_RGB(IDX_FRONT_L, r, g, b);
      setLED_RGB(IDX_FRONT_R, r, g, b);
      
    } else if (idleMode==9){
      // MATRIX RAIN MODE (mode 9) - falling streaks of color like Matrix code
      static float dropPos[10] = {0}; // Position of each drop (0.0 = top, 1.0+ = off screen)
      static float dropSpeed[10] = {0}; // Speed of each drop
      static float dropHue[10] = {0}; // Color of each drop
      static uint32_t lastDropUpdate = 0;
      
      float tsec=now/1000.0f;
      uint32_t deltaMs = (lastDropUpdate > 0) ? (now - lastDropUpdate) : 16; // Default 16ms if first run
      if(deltaMs > 100) deltaMs = 100; // Cap delta to prevent large jumps on mode switch
      lastDropUpdate = now;
      
      // Update drops
      for(int i=0;i<10;i++){
        // Initialize drop if needed
        if(dropPos[i] <= -0.5f || dropPos[i] > 1.5f){
          if(random(0,100) < 15){ // 15% chance to spawn new drop (increased from 8%)
            dropPos[i] = -0.3f - random(0,50)/1000.0f; // Start slightly above
            dropSpeed[i] = 2.0f + random(0,300)/100.0f; // Much faster: 2.0-5.0 units/second (was 0.3-0.5)
            dropHue[i] = random(100,160); // Green-cyan range
          } else {
            dropPos[i] = -1.0f; // Keep off screen
          }
        }
        
        // Update drop position
        if(dropPos[i] > -1.0f){
          dropPos[i] += dropSpeed[i] * (deltaMs / 1000.0f);
        }
      }
      
      // Render matrix rain
      // Map LEDs to vertical positions (bottom to top)
      int ledPositions[10]; // Maps LED index to vertical position
      float ledY[10]; // Y position (0.0 = bottom, 1.0 = top)
      
      // Left column (bottom to top)
      for(int k=0;k<4;k++){
        ledPositions[k] = IDX_LEFT[k];
        ledY[k] = (float)k / 3.0f; // 0.0 to 1.0
      }
      // Right column (bottom to top)
      for(int k=0;k<4;k++){
        ledPositions[k+4] = IDX_RIGHT[k];
        ledY[k+4] = (float)k / 3.0f;
      }
      // Front buttons (middle height)
      ledPositions[8] = IDX_FRONT_L;
      ledY[8] = 0.5f;
      ledPositions[9] = IDX_FRONT_R;
      ledY[9] = 0.5f;
      
      // Render each LED
      for(int i=0;i<10;i++){
        float brightness = 0.0f;
        float hue = 120.0f; // Default green
        
        // Check if any drop is near this LED
        for(int d=0;d<10;d++){
          if(dropPos[d] > -0.5f && dropPos[d] < 1.5f){
            float ledPos = 1.0f - ledY[i]; // Invert Y (top=0, bottom=1)
            float dist = fabsf(dropPos[d] - ledPos);
            if(dist < 0.2f){
              // Drop is near this LED
              float intensity = 1.0f - (dist / 0.2f); // Fade with distance
              if(intensity > brightness){
                brightness = intensity;
                hue = dropHue[d];
              }
            }
            // Trail effect - dimmer behind the drop
            if(dropPos[d] > ledPos && dist < 0.5f){
              float trailIntensity = (0.5f - dist) / 0.5f * 0.5f; // Brighter trail (was 0.3f)
              if(trailIntensity > brightness){
                brightness = trailIntensity;
                hue = dropHue[d] - 20.0f; // Slightly different hue for trail
                if(hue < 0) hue += 360.0f;
              }
            }
          }
        }
        
        // Base dim glow - increased brightness
        float baseV = 0.08f; // Increased from 0.05f
        float totalV = baseV + brightness * 0.70f; // Increased from 0.40f (max now 0.78 instead of 0.45)
        uint8_t r,g,b; hsv2rgb(hue, 0.9f, totalV, r, g, b);
        setLED_RGB(ledPositions[i], r, g, b);
      }
    } else if (idleMode==10){
      // LIGHTNING STRIKE MODE (mode 10) - realistic lightning arcs traveling down each side
      // Left side: 7(top) → 5 → 12 → 38(bottom)
      // Right side: 8(top) → 9 → 2 → 11(bottom)
      static uint32_t lastStrike = 0;
      static uint32_t strikeDuration = 0;
      static int strikeSide = -1; // 0=left, 1=right, -1=none
      static int strikePosition = -1; // Which LED in the chain (0-3)
      static float strikeIntensity = 0.0f;
      static float strikeHue = 200.0f; // Blue-cyan
      
      float tsec=now/1000.0f;
      uint32_t timeSinceStrike = now - lastStrike;
      
      // Lightning chains: top to bottom
      // Left: GPIO 7(3) → 5(2) → 12(1) → 38(0)
      // Right: GPIO 8(9) → 9(8) → 2(7) → 11(6)
      const int LEFT_CHAIN[4] = {IDX_LEFT[3], IDX_LEFT[2], IDX_LEFT[1], IDX_LEFT[0]};  // 3,2,1,0 = GPIOs 7,5,12,38
      const int RIGHT_CHAIN[4] = {IDX_RIGHT[3], IDX_RIGHT[2], IDX_RIGHT[1], IDX_RIGHT[0]}; // 9,8,7,6 = GPIOs 8,9,2,11
      const uint32_t STRIKE_TOTAL_DURATION = 300; // Total strike duration in ms
      const uint32_t PER_LED_DURATION = 60; // Time each LED is bright
      
      // Trigger new strike
      if(strikeIntensity <= 0.0f && strikePosition < 0){
        // Wait for next strike (random interval 0.8-4 seconds)
        if(timeSinceStrike > (800 + random(0, 3200))){
          strikeSide = random(0, 2); // 0=left, 1=right
          strikePosition = 0; // Start at top
          strikeDuration = STRIKE_TOTAL_DURATION;
          strikeIntensity = 1.0f;
          strikeHue = 200.0f + random(0, 40); // Blue-cyan range (200-240)
          lastStrike = now;
        }
      }
      
      // Update strike position and intensity
      if(strikeIntensity > 0.0f && strikePosition >= 0){
        uint32_t strikeAge = now - lastStrike;
        
        // Calculate which LED should be bright based on time
        int currentLED = strikeAge / PER_LED_DURATION;
        if(currentLED > 3) currentLED = 3;
        
        // Update strike position
        strikePosition = currentLED;
        
        // Calculate intensity - bright flash then fade
        if(strikeAge > strikeDuration){
          strikeIntensity = 0.0f;
          strikePosition = -1;
          strikeSide = -1;
        } else {
          float progress = (float)strikeAge / (float)strikeDuration;
          if(progress < 0.15f){
            strikeIntensity = 1.0f; // Bright initial flash
          } else {
            strikeIntensity = 1.0f - ((progress - 0.15f) / 0.85f); // Fade out
          }
        }
      }
      
      // Render lightning
      for(int i=0;i<10;i++){
        float v = 0.03f; // Base very dim
        float hue = idleBaseHue[i] + tsec * 1.0f;
        while(hue >= 360.0f) hue -= 360.0f;
        
        if(strikeIntensity > 0.0f && strikeSide >= 0){
          // Check if this LED is in the strike chain
          bool inChain = false;
          int chainPos = -1;
          
          if(strikeSide == 0){
            // Left side chain
            for(int p=0; p<4; p++){
              if(i == LEFT_CHAIN[p]){
                inChain = true;
                chainPos = p;
                break;
              }
            }
          } else {
            // Right side chain
            for(int p=0; p<4; p++){
              if(i == RIGHT_CHAIN[p]){
                inChain = true;
                chainPos = p;
                break;
              }
            }
          }
          
          if(inChain){
            // This LED is in the strike chain
            if(chainPos <= strikePosition){
              // Lightning has reached or passed this LED
              float ledIntensity = strikeIntensity;
              
              // Current LED is brightest
              if(chainPos == strikePosition){
                ledIntensity = strikeIntensity * 1.0f;
              } else {
                // Past LEDs fade quickly
                int age = strikePosition - chainPos;
                ledIntensity = strikeIntensity * (1.0f - age * 0.4f);
                if(ledIntensity < 0.1f) ledIntensity = 0.1f;
              }
              
              // Illuminate the whole side as arc progresses
              if(chainPos <= strikePosition){
                v = 0.15f + ledIntensity * 0.85f; // Bright flash
                hue = strikeHue;
              }
            }
            
          }
          
          // Illuminate the whole side as the arc travels (side glow effect)
          if(strikeSide == 0){
            // Left side - illuminate all left LEDs
            for(int k=0; k<4; k++){
              if(i == IDX_LEFT[k]){
                float sideGlow = strikeIntensity * 0.4f; // Side-wide illumination
                if(sideGlow > v){
                  v = sideGlow;
                  hue = strikeHue + 10.0f; // Slightly different hue for glow
                  while(hue >= 360.0f) hue -= 360.0f;
                }
                break;
              }
            }
          } else if(strikeSide == 1){
            // Right side - illuminate all right LEDs
            for(int k=0; k<4; k++){
              if(i == IDX_RIGHT[k]){
                float sideGlow = strikeIntensity * 0.4f; // Side-wide illumination
                if(sideGlow > v){
                  v = sideGlow;
                  hue = strikeHue + 10.0f; // Slightly different hue for glow
                  while(hue >= 360.0f) hue -= 360.0f;
                }
                break;
              }
            }
          }
        }
        
        uint8_t r,g,b; hsv2rgb(hue, 1.0f, v, r, g, b);
        setLED_RGB(i, r, g, b);
      }
    } else if (idleMode==11){
      // PLASMA SWIRL MODE (mode 11) - swirling colors with Perlin-like noise
      float tsec=now/1000.0f;
      float plasmaSpeed = 0.5f;
      float plasmaScale = 2.0f;
      
      // Simple plasma using sine waves (Perlin-like effect)
      for(int i=0;i<10;i++){
        // Map LED position to 2D space
        float x, y;
        if(i < 4){
          // Left column
          x = 0.0f;
          y = (float)i / 3.0f;
        } else if(i < 8){
          // Right column
          x = 1.0f;
          y = (float)(i-4) / 3.0f;
        } else {
          // Front buttons
          x = 0.5f;
          y = 0.5f;
        }
        
        // Create swirling plasma effect
        float plasma1 = sinf((x + tsec * plasmaSpeed) * plasmaScale);
        float plasma2 = sinf((y + tsec * plasmaSpeed * 0.7f) * plasmaScale);
        float plasma3 = sinf((x + y + tsec * plasmaSpeed * 1.3f) * plasmaScale * 0.5f);
        float plasma4 = sinf((sqrtf(x*x + y*y) + tsec * plasmaSpeed * 0.5f) * plasmaScale);
        
        float combined = (plasma1 + plasma2 + plasma3 + plasma4) / 4.0f;
        
        // Map to hue and brightness
        float hue = fmodf(180.0f + combined * 180.0f + tsec * 20.0f, 360.0f);
        float v = 0.15f + 0.35f * (combined * 0.5f + 0.5f);
        
        uint8_t r,g,b; hsv2rgb(hue, 1.0f, v, r, g, b);
        setLED_RGB(i, r, g, b);
      }
    }
  } else {
    float tsec=now/1000.0f;
    for (int i=0;i<10;i++){
      uint8_t r=0,g=0,b=0;
      if (i==IDX_FRONT_L){
        if (down[i]){
          if (sumL>0){ r=(uint8_t)lroundf(mixLr); g=(uint8_t)lroundf(mixLg); b=(uint8_t)lroundf(mixLb); }
          else { float hue=fmodf(rainbowStartHue[i]+tsec*(RAINBOW_HZ*360.0f),360.0f); hsv2rgb(hue,1.0f,PRESS_V,r,g,b); }
        } else {
          if (sumL>0){ r=(uint8_t)lroundf(mixLr); g=(uint8_t)lroundf(mixLg); b=(uint8_t)lroundf(mixLb); }
          else { float hue=idleBaseHue[i]+idleRateDegPerSec[i]*tsec; hsv2rgb(hue,IDLE_SAT,IDLE_V_MIN,r,g,b); }
        }
        setLED_RGB(i,r,g,b); continue;
      }
      if (i==IDX_FRONT_R){
        if (down[i]){
          if (sumR>0){ r=(uint8_t)lroundf(mixRr); g=(uint8_t)lroundf(mixRg); b=(uint8_t)lroundf(mixRb); }
          else { float hue=fmodf(rainbowStartHue[i]+tsec*(RAINBOW_HZ*360.0f),360.0f); hsv2rgb(hue,1.0f,PRESS_V,r,g,b); }
        } else {
          if (sumR>0){ r=(uint8_t)lroundf(mixRr); g=(uint8_t)lroundf(mixRg); b=(uint8_t)lroundf(mixRb); }
          else { float hue=idleBaseHue[i]+idleRateDegPerSec[i]*tsec; hsv2rgb(hue,IDLE_SAT,IDLE_V_MIN,r,g,b); }
        }
        setLED_RGB(i,r,g,b); continue;
      }

      if (down[i]){
        uint8_t pr,pg,pb; getPressColorForGPIO(BTN_PINS[i],pr,pg,pb);
        r=(uint8_t)lroundf(pr*PRESS_V); g=(uint8_t)lroundf(pg*PRESS_V); b=(uint8_t)lroundf(pb*PRESS_V);
        lastR[i]=r; lastG[i]=g; lastB[i]=b;
      } else {
        uint32_t dt = now - releaseTs[i];
        if (dt<PRESS_FADE_MS){ float w=1.0f-(dt/(float)PRESS_FADE_MS);
          r=(uint8_t)lroundf(lastR[i]*w); g=(uint8_t)lroundf(lastG[i]*w); b=(uint8_t)lroundf(lastB[i]*w);
        } else { float hue=idleBaseHue[i]+idleRateDegPerSec[i]*tsec; hsv2rgb(hue,IDLE_SAT,IDLE_V_MIN,r,g,b); }
      }
      setLED_RGB(i,r,g,b);
    }
  }

  // randomize rainbow start hue on front press
  if (edgeDownArr[IDX_FRONT_L]) rainbowStartHue[IDX_FRONT_L]=random(0,360);
  if (edgeDownArr[IDX_FRONT_R]) rainbowStartHue[IDX_FRONT_R]=random(0,360);

  } // !rhythmGameIsActive() (LED strip)

  // ========= Audio render =========
  if (rhythmGameOwnsAudioOutput())
    rhythmGameAudioPump();
  else
    audioRender(wantL, wantR);

  // MP3 decode is heavy; avoid fixed 1ms sleep every loop while it owns I2S.
  if (rhythmGameOwnsAudioOutput())
    delay(0);
  else
    delay(1);
  
  // DEBUG: Periodic status update every 5 seconds
  static uint32_t lastStatusUpdate=0;
  if (now - lastStatusUpdate > 5000){
    lastStatusUpdate = now;
    uint32_t timeSinceLastGood = (lastGoodI2S > 0) ? (now - lastGoodI2S) : 999999;
    Serial.printf("[STATUS] Uptime: %lus, Idle: %s, Mode: %d, Scale: %s\n",
                  now/1000, idle?"YES":"NO", idleMode, SCALES[scaleIndex].name);
    Serial.printf("[STATUS] I2S: %s, Errors: %d, LastGood: %lums ago, Buttons: %d\n",
                  i2s_initialized?"OK":"FAIL", i2s_consec_errors, timeSinceLastGood, anyDown?1:0);
    
    // Print detailed I2S diagnostics every 30 seconds
    static uint32_t lastI2SDiag=0;
    if(now - lastI2SDiag > 30000){
      lastI2SDiag = now;
      audioPrintDiagnostics();
    }
  }
}



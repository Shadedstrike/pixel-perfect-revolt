#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include "scales.h"
#include "audio.h"

extern hd44780_I2Cexp lcd;

void lcdPrintStatus(const char* scaleName, float fL, float fR, int8_t offL, int8_t offR);
void lcdPrintScaleSelection(uint8_t currentScaleIndex, uint32_t now);
// Full-screen 4x20: title + 3-line ASCII waveform (one cycle across width).
void lcdPrintWaveShapePreview(AudioWaveShape shape);

// LCD Animation functions
void lcdSparkleAnimation(uint32_t now);
void lcdGlitchTextAnimation(uint32_t now, uint32_t idleStartTime);
void lcdParticleExplosion(uint32_t now, uint32_t idleStartTime);
void lcdWaveVisualization(uint32_t now, uint32_t idleStartTime);
void lcdKaleidoscope(uint32_t now, uint32_t idleStartTime);
void lcdPulsingPatterns(uint32_t now, uint32_t idleStartTime);
void lcdBeatGrid(uint32_t now, uint32_t idleStartTime);
void lcdPyramidAnimation(uint32_t now, uint32_t idleStartTime);
void lcdClearAnimation();
void lcdWelcomeAnimation(uint32_t now, uint32_t lastButtonPressTime);
// Call when starting a welcome (idle exit); ensures a new session even if last run ended in phase 99 with a short gap.
void lcdWelcomeAnimationBeginSession(void);
// Egyptian and Soviet themed animations
void lcdHieroglyphicScroll(uint32_t now, uint32_t idleStartTime);
void lcdCyrillicTextScroll(uint32_t now, uint32_t idleStartTime);
void lcdFactoryIndustrialText(uint32_t now, uint32_t idleStartTime);
void lcdSovietSloganStyle(uint32_t now, uint32_t idleStartTime);
void lcdMatrixCyrillic(uint32_t now, uint32_t idleStartTime);

// Retro rhythm-game UI (20x4 HD44780)
struct RhythmSongRow {
  const char *title;
  uint8_t difficulty;
};
void lcdRetroFlashScreen(uint32_t now, uint32_t flashStartMs);
// Sum of intro + plain-text phases; rhythm_game waits this long before RG_MENU.
uint32_t lcdRetroFlashDurationMs(void);
void lcdRetroMenu(int selectedIdx, const RhythmSongRow *rows, int numRows, uint32_t wallMs);
void lcdRetroPlaying(const char *title, uint32_t elapsedMs, uint32_t durationMs, uint16_t approxBpm, uint32_t wallMs,
                     uint32_t msToNextBeat, uint32_t lastTapWallMs);
void lcdRetroStillTherePrompt(uint32_t now, uint32_t promptStartMs);
void lcdRetroGetReady(uint32_t now, uint32_t getReadyStartMs, const char *songTitle, uint32_t readyMs,
                      uint32_t countEachMs);
void lcdRetroResultsScore(const char *title, char grade, int mainPct, int bonusPct, int totalPct, uint32_t now);
void lcdRetroResultsPrompt(uint32_t wallMs, const char *nextSongTitle);

#endif // DISPLAY_H


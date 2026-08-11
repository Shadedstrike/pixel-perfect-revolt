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
void lcdRetroPlayingInvalidate(void);
// Drop the cached play-lane CGRAM (slots 1-5) so it reloads on the next frame.
void lcdRetroPlayLaneGlyphsInvalidate(void);
// Called between LCD row writes — point this at the MP3 pump so a full-panel
// refresh cannot block audio decode for 10-20ms in one stretch.
void lcdSetInterRowCallback(void (*cb)(void));
void lcdRetroMeltdownBegin(void);
// 5s hold easter-egg: glitch → matrix hearts → disintegrate → rain wash.
bool lcdRetroHoldMeltdown(uint32_t now, uint32_t startMs);
// Release during meltdown: 80 ms – 1000 ms snap-back scaled by meltdown depth.
uint32_t lcdRetroMeltdownRecoverDurationMs(uint32_t meltdownHeldMs);
struct LcdConsumedEntry { uint32_t beatMs; uint32_t hitWallMs; int8_t hitCol; };
void lcdRetroPlaying(const char *title, uint32_t elapsedMs, uint32_t durationMs, uint32_t beatPeriodMs, uint32_t wallMs,
                     uint32_t songRelMs, const uint32_t *beats, int nBeats,
                     uint32_t lastHitWallMs, uint32_t lastMissWallMs,
                     const LcdConsumedEntry *consumed, int nConsumed,
                     bool paused, int liveScorePct, float recoverGlitch = 0.f);
// True while HIT/MISS label strobe or strike-cue X flash is active — faster LCD refresh during play.
bool lcdRetroJudgementFlashActive(uint32_t wallMs, uint32_t lastHitWallMs, uint32_t lastMissWallMs);
bool lcdRetroPlayingNeedsFastLcd(uint32_t wallMs, uint32_t songRelMs, uint32_t beatPeriodMs,
                                 uint32_t lastHitWallMs, uint32_t lastMissWallMs,
                                 const uint32_t *beats, int nBeats,
                                 const LcdConsumedEntry *consumed, int nConsumed);
void lcdRetroResumeCountdown(uint32_t now, uint32_t startMs, uint32_t countEachMs, const char *title, uint32_t elapsedMs,
                             uint32_t durationMs);
void lcdRetroStillTherePrompt(uint32_t now, uint32_t promptStartMs);
// Ms for one full title marquee (long titles) or a short read pause; use before 3-2-1 countdown.
uint32_t lcdRetroTitleScrollDurationMs(const char *songTitle, unsigned visibleCols);
void lcdRetroGetReady(uint32_t now, uint32_t getReadyStartMs, const char *songTitle, uint32_t titleScrollMs,
                      uint32_t countEachMs);
// Held-to-enter countdown, shown in RG_NORMAL while all four top keys are down.
// Owns the full 20x4 panel: sprite borders on rows 0/3, "PRESS n MORE SEC" on
// row 1, lightly glitched "R Y T H E M  M 0 D E" on row 2.
void lcdRetroEnterCountdown(uint32_t now, uint32_t heldMs, uint32_t totalMs);
// Generic "hold to do X" panel — sprite borders, "PRESS n MORE SEC", and `title`
// centred and lightly glitched on row 2. Shared by rhythm entry, Simon entry, and
// the wave / scale hold gestures.
void lcdHoldCountdown(uint32_t now, uint32_t heldMs, uint32_t totalMs, const char *title);

// Simon ("Follow the Leader") panels.
void lcdSimonBanner(uint32_t now, const char *line1, const char *line2);
void lcdSimonStatus(uint32_t now, int round, int step, int total, bool playerTurn, int poolSize);
// Forget Simon's CGRAM (slots 0-3) — call if another screen overwrites them.
void lcdSimonInvalidate(void);
void lcdSimonGameOver(uint32_t now, int round);
void lcdSimonWin(uint32_t now, int round);
// Idle "how to play" prompt, one gesture per `which` (0-3). Rotates through the
// idle animation set so people discover the hidden modes without signage.
void lcdIdleGesturePrompt(uint32_t now, uint8_t which);
// Forget the CGRAM slots this screen loaded (call if another screen overwrites 0-3).
void lcdRetroEnterCountdownInvalidate(void);

void lcdRetroResultsScore(const char *title, char grade, int mainPct, int bonusPct, int totalPct, uint32_t now);
void lcdRetroResultsPrompt(uint32_t wallMs, const char *nextSongTitle);

#endif // DISPLAY_H


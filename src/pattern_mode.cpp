#include "pattern_mode.h"
#include "config.h"
#include "display.h"
#include "leds.h"
#include "rhythm_game.h"
#include "simon_game.h"
#include <string.h>

enum PatternPhase : uint8_t {
  PAT_OFF = 0,
  PAT_INTRO,
  PAT_SHOW_ON,
  PAT_SHOW_OFF,
  PAT_INPUT,
  PAT_ROUND_OK,
  PAT_ROUND_FAIL,
};

static const uint32_t PAT_ENTER_HOLD_MS = 5000;
static const uint32_t PAT_EXIT_HOLD_MS = 5000;
static const uint32_t PAT_INTRO_MS = 900;
static const uint32_t PAT_ROUND_OK_MS = 1200;
static const uint32_t PAT_ROUND_FAIL_MS = 2200;
static const int kMaxPatternLen = 24;

static PatternPhase s_phase = PAT_OFF;
static uint8_t s_pattern[kMaxPatternLen];
static int s_patternLen = 0;
static int s_showIdx = 0;
static int s_inputIdx = 0;
static int s_level = 1;
static int s_roundsCompleted = 0;
static int s_lastCorrectCount = 0;
static uint32_t s_phaseStartMs = 0;
static uint32_t s_flashOnMs = 750;
static uint32_t s_flashOffMs = 280;
static uint32_t s_yellowEnterHoldStart = 0;
static uint32_t s_yellowExitHoldStart = 0;
static bool s_yellowExitArmed = false;
static uint32_t s_lastInteractMs = 0;
static uint32_t s_lastLcdMs = 0;
static int s_showLedIdx = -1;

static void patternTouchActivity(uint32_t now) { s_lastInteractMs = now; }

static bool yellowPairHold(const bool *down) { return down[IDX_38] && down[IDX_11]; }

static bool allButtonsDown(const bool *down) {
  for (int i = 0; i < 10; i++) {
    if (!down[i])
      return false;
  }
  return true;
}

static int sideButtonToPatternIdx(int btnIdx) {
  for (int k = 0; k < 4; k++) {
    if (btnIdx == IDX_LEFT[k])
      return k;
    if (btnIdx == IDX_RIGHT[k])
      return 4 + k;
  }
  return -1;
}

static int patternIdxToButtonIdx(int patIdx) {
  if (patIdx < 0 || patIdx > 7)
    return -1;
  if (patIdx < 4)
    return IDX_LEFT[patIdx];
  return IDX_RIGHT[patIdx - 4];
}

static void patternUpdateFlashTiming() {
  uint32_t onMs = 750u;
  if (s_level > 1)
    onMs = 750u - (uint32_t)(s_level - 1) * 45u;
  if (onMs < 260u)
    onMs = 260u;
  s_flashOnMs = onMs;
  s_flashOffMs = 220u + (onMs / 5u);
}

static void patternGenerate(int len) {
  if (len < 1)
    len = 1;
  if (len > kMaxPatternLen)
    len = kMaxPatternLen;
  s_patternLen = len;
  int prev = -1;
  for (int i = 0; i < len; i++) {
    int pick = random(0, 8);
    if (len > 2 && pick == prev)
      pick = (pick + 1 + random(0, 7)) % 8;
    if (i > 0 && len > 3 && pick == s_pattern[i - 1] && (i < 2 || pick == s_pattern[i - 2]))
      pick = (pick + 2 + random(0, 5)) % 8;
    s_pattern[i] = (uint8_t)pick;
    prev = pick;
  }
}

static void patternBeginRound(uint32_t now) {
  patternUpdateFlashTiming();
  patternGenerate(s_level);
  s_showIdx = 0;
  s_inputIdx = 0;
  s_showLedIdx = -1;
  s_phase = PAT_SHOW_ON;
  s_phaseStartMs = now;
  patternTouchActivity(now);
}

static void patternEnter(uint32_t now) {
  s_level = 1;
  s_roundsCompleted = 0;
  s_lastCorrectCount = 0;
  patternTouchActivity(now);
  s_yellowEnterHoldStart = 0;
  s_yellowExitHoldStart = 0;
  s_yellowExitArmed = false;
  s_phase = PAT_INTRO;
  s_phaseStartMs = now;
  Serial.println("[PATTERN] Enter pattern mode (both yellow held >=5s)");
}

static void patternExit(const char *reason) {
  s_phase = PAT_OFF;
  s_showLedIdx = -1;
  s_yellowEnterHoldStart = 0;
  s_yellowExitHoldStart = 0;
  s_yellowExitArmed = false;
  Serial.printf("[PATTERN] %s\n", reason);
}

static void patternProcessExitHold(uint32_t now, const bool *down) {
  if (!yellowPairHold(down)) {
    s_yellowExitHoldStart = 0;
    s_yellowExitArmed = true;
    return;
  }
  if (!s_yellowExitArmed)
    return;
  if (s_yellowExitHoldStart == 0)
    s_yellowExitHoldStart = now;
  else if (now - s_yellowExitHoldStart >= PAT_EXIT_HOLD_MS)
    patternExit("Exit pattern mode (both yellow held >=5s)");
}

static bool patternAnyInput(const bool *down, const bool *edgeDown) {
  for (int i = 0; i < 10; i++) {
    if (down[i] || edgeDown[i])
      return true;
  }
  return false;
}

void patternModeLoop(uint32_t now, const bool *down, const bool *edgeDown) {
  if (simonGameIsActive()) {
    s_yellowEnterHoldStart = 0;
    return;
  }
  if (rhythmGameIsActive()) {
    if (s_phase != PAT_OFF)
      patternExit("Rhythm game took over");
    return;
  }

  if (s_phase == PAT_OFF) {
    if (allButtonsDown(down)) {
      s_yellowEnterHoldStart = 0;
      return;
    }
    if (yellowPairHold(down)) {
      if (s_yellowEnterHoldStart == 0)
        s_yellowEnterHoldStart = now;
      else if (now - s_yellowEnterHoldStart >= PAT_ENTER_HOLD_MS)
        patternEnter(now);
    } else {
      s_yellowEnterHoldStart = 0;
    }
    return;
  }

  patternProcessExitHold(now, down);
  if (s_phase == PAT_OFF)
    return;

  if (patternAnyInput(down, edgeDown))
    patternTouchActivity(now);

  // Idle applies only while waiting for player input — not during pattern playback.
  if (s_phase == PAT_INPUT && (now - s_lastInteractMs) >= IDLE_AFTER_MS) {
    patternExit("Idle timeout -> synth");
    return;
  }

  switch (s_phase) {
  case PAT_INTRO:
    if (now - s_phaseStartMs >= PAT_INTRO_MS)
      patternBeginRound(now);
    break;

  case PAT_SHOW_ON:
    if (s_showIdx < s_patternLen)
      s_showLedIdx = patternIdxToButtonIdx(s_pattern[s_showIdx]);
    if (now - s_phaseStartMs >= s_flashOnMs) {
      s_showLedIdx = -1;
      s_phase = PAT_SHOW_OFF;
      s_phaseStartMs = now;
    }
    break;

  case PAT_SHOW_OFF:
    if (now - s_phaseStartMs >= s_flashOffMs) {
      s_showIdx++;
      if (s_showIdx >= s_patternLen) {
        s_inputIdx = 0;
        s_phase = PAT_INPUT;
        s_phaseStartMs = now;
        patternTouchActivity(now);
      } else {
        s_phase = PAT_SHOW_ON;
        s_phaseStartMs = now;
      }
    }
    break;

  case PAT_INPUT:
    for (int i = 0; i < 10; i++) {
      if (!edgeDown[i])
        continue;
      int patIdx = sideButtonToPatternIdx(i);
      if (patIdx < 0)
        continue;
      patternTouchActivity(now);
      if (patIdx == (int)s_pattern[s_inputIdx]) {
        s_inputIdx++;
        s_lastCorrectCount = s_inputIdx;
        if (s_inputIdx >= s_patternLen) {
          s_roundsCompleted++;
          s_lastCorrectCount = s_patternLen;
          s_level++;
          s_phase = PAT_ROUND_OK;
          s_phaseStartMs = now;
        }
      } else {
        s_lastCorrectCount = s_inputIdx;
        s_phase = PAT_ROUND_FAIL;
        s_phaseStartMs = now;
      }
      break;
    }
    break;

  case PAT_ROUND_OK:
    patternTouchActivity(now);
    if (now - s_phaseStartMs >= PAT_ROUND_OK_MS)
      patternBeginRound(now);
    break;

  case PAT_ROUND_FAIL:
    if (now - s_phaseStartMs >= PAT_ROUND_FAIL_MS)
      patternExit("Round failed -> synth");
    break;

  default:
    break;
  }
}

bool patternModeIsActive() { return s_phase != PAT_OFF; }

bool patternModeSuppressNormalUi() { return patternModeIsActive(); }

bool patternModeShouldSilenceSynth() { return patternModeIsActive(); }

bool patternModeDrawLcd(uint32_t now) {
  if (!patternModeIsActive())
    return false;

  uint32_t throttle = 180;
  if (s_phase == PAT_SHOW_ON || s_phase == PAT_INPUT)
    throttle = 90;
  if (now - s_lastLcdMs < throttle)
    return false;
  s_lastLcdMs = now;

  const char *status = "Watch...";
  if (s_phase == PAT_INTRO)
    status = "Get ready";
  else if (s_phase == PAT_INPUT)
    status = "Your turn";
  else if (s_phase == PAT_ROUND_OK)
    status = "Nice!";
  else if (s_phase == PAT_ROUND_FAIL)
    status = "Missed";

  lcdPatternModeScreen(status, s_level, s_lastCorrectCount, s_patternLen, s_roundsCompleted);
  return true;
}

void patternModeApplyLeds(uint32_t now, const bool *down) {
  if (!patternModeIsActive())
    return;

  for (int i = 0; i < 10; i++) {
    uint8_t r = 0, g = 0, b = 0;
    if (s_phase == PAT_SHOW_ON && i == s_showLedIdx) {
      getPressColorForGPIO(BTN_PINS[i], r, g, b);
      r = (uint8_t)lroundf(r * 1.0f);
      g = (uint8_t)lroundf(g * 1.0f);
      b = (uint8_t)lroundf(b * 1.0f);
    } else if (down[i]) {
      getPressColorForGPIO(BTN_PINS[i], r, g, b);
      r = (uint8_t)lroundf(r * PRESS_V);
      g = (uint8_t)lroundf(g * PRESS_V);
      b = (uint8_t)lroundf(b * PRESS_V);
    } else if (s_phase == PAT_SHOW_OFF || s_phase == PAT_SHOW_ON) {
      hsv2rgb(42.f, 0.35f, 0.06f, r, g, b);
    } else {
      hsv2rgb(200.f, 0.25f, 0.05f, r, g, b);
    }
    setLED_RGB(i, r, g, b);
  }
}

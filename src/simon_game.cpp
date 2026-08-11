#include "simon_game.h"

#include "actuator_link.h"
#include "config.h"
#include "display.h"
#include "leds.h"
#include "scales.h"

#include <string.h>

// ---------------------------------------------------------------- tuning ----
static const uint32_t SIMON_ENTER_HOLD_MS = 5000;
static const uint32_t SIMON_INTRO_MS      = 1600;
static const uint32_t SIMON_GAP_MS        = 170;   // dark gap between steps
static const uint32_t SIMON_STEP_MAX_MS   = 430;   // lit time, round 1
static const uint32_t SIMON_STEP_MIN_MS   = 230;   // floor as rounds climb
static const uint32_t SIMON_GOOD_MS       = 550;
static const uint32_t SIMON_FAIL_MS       = 3500;
static const uint32_t SIMON_INPUT_TIMEOUT_MS = 6000;
static const uint32_t SIMON_QUIT_HOLD_MS  = 2000;  // yellow pair to bail out
static const int      SIMON_SEQ_MAX       = 32;

enum SimonPhase : uint8_t {
  SIMON_OFF = 0,
  SIMON_INTRO,
  SIMON_PLAYBACK,
  SIMON_INPUT,
  SIMON_GOOD,
  SIMON_FAIL,
};

static SimonPhase s_phase = SIMON_OFF;
static uint8_t  s_seq[SIMON_SEQ_MAX];
static int      s_seqLen = 0;
static int      s_round = 0;
static int      s_inputIdx = 0;
static int      s_stepIdx = 0;
static bool     s_stepLit = false;
static uint32_t s_phaseStartMs = 0;
static uint32_t s_stepStartMs = 0;
static uint32_t s_lastInputMs = 0;
static uint32_t s_enterHoldStart = 0;
static uint32_t s_enterHeldMs = 0;
static uint32_t s_quitHoldStart = 0;
static int      s_litKey = -1;   // button index currently lit, -1 = none
static int      s_wrongKey = -1;

// -------------------------------------------------------------- key pool ----
// Index into the pool -> button index. 0-3 left, 4-7 right, 8-9 fronts.
static int simonPoolKey(int n) {
  if (n < 4)
    return IDX_LEFT[n];
  if (n < 8)
    return IDX_RIGHT[n - 4];
  return (n == 8) ? IDX_FRONT_L : IDX_FRONT_R;
}

static int simonPoolSize(int round) {
  if (round <= 3)
    return 4;
  if (round <= 7)
    return 8;
  return 10;
}

// Is this button index part of the pool for the current round?
static bool simonKeyInPool(int btnIdx, int round) {
  const int n = simonPoolSize(round);
  for (int i = 0; i < n; i++) {
    if (simonPoolKey(i) == btnIdx)
      return true;
  }
  return false;
}

// Front keys carry no actuator colour — only the 8 side keys drive it.
static bool simonKeyDrivesActuator(int btnIdx) {
  for (int k = 0; k < 4; k++) {
    if (IDX_LEFT[k] == btnIdx || IDX_RIGHT[k] == btnIdx)
      return true;
  }
  return false;
}

// Tone for a step. Reuses the active scale so it stays musical, and splits sides
// across the stereo field the same way normal play does.
static float simonKeyHz(int btnIdx) {
  for (int k = 0; k < 4; k++) {
    if (IDX_LEFT[k] == btnIdx || IDX_RIGHT[k] == btnIdx)
      return scaleHzAtIdx(degreeIndexForSlot(k));
  }
  return scaleHzAtIdx(degreeIndexForSlot(3) + 2); // fronts sit above the top slot
}

static bool simonKeyIsRight(int btnIdx) {
  for (int k = 0; k < 4; k++) {
    if (IDX_RIGHT[k] == btnIdx)
      return true;
  }
  return btnIdx == IDX_FRONT_R;
}

static uint32_t simonStepMs(int round) {
  const uint32_t shrink = (uint32_t)round * 14u;
  if (SIMON_STEP_MAX_MS <= SIMON_STEP_MIN_MS + shrink)
    return SIMON_STEP_MIN_MS;
  return SIMON_STEP_MAX_MS - shrink;
}

// ------------------------------------------------------------- actuator ----
static void simonActuator(int btnIdx, bool on) {
  if (btnIdx >= 0 && simonKeyDrivesActuator(btnIdx))
    actuatorPublishForSideColumn(btnIdx, on);
}

static void simonAllActuatorsOff() {
  for (int k = 0; k < 4; k++) {
    actuatorPublishForSideColumn(IDX_LEFT[k], false);
    actuatorPublishForSideColumn(IDX_RIGHT[k], false);
  }
}

// ---------------------------------------------------------------- rounds ----
static void simonBeginRound(uint32_t now) {
  s_round++;
  if (s_seqLen < SIMON_SEQ_MAX) {
    const int pool = simonPoolSize(s_round);
    s_seq[s_seqLen++] = (uint8_t)simonPoolKey((int)random(pool));
  }
  s_stepIdx = 0;
  s_stepLit = false;
  s_inputIdx = 0;
  s_litKey = -1;
  s_phase = SIMON_PLAYBACK;
  s_phaseStartMs = now;
  s_stepStartMs = now;
  Serial.printf("[SIMON] round %d — %d steps, pool %d\n", s_round, s_seqLen, simonPoolSize(s_round));
}

static void simonStart(uint32_t now) {
  lcdSimonInvalidate(); // reclaim CGRAM 0-3 from the hold-countdown screen
  randomSeed((uint32_t)esp_random());
  s_seqLen = 0;
  s_round = 0;
  s_wrongKey = -1;
  s_phase = SIMON_INTRO;
  s_phaseStartMs = now;
  s_enterHoldStart = 0;
  s_enterHeldMs = 0;
  Serial.println("[SIMON] enter Follow the Leader");
}

static void simonStop(uint32_t now, const char *why) {
  (void)now;
  lcdRetroEnterCountdownInvalidate(); // hand CGRAM 0-3 back
  simonAllActuatorsOff();
  s_phase = SIMON_OFF;
  s_litKey = -1;
  s_enterHoldStart = 0;
  s_enterHeldMs = 0;
  s_quitHoldStart = 0;
  Serial.printf("[SIMON] exit — %s (reached round %d)\n", why, s_round);
}

// Enter gesture: both blues down, both yellows UP. Without the yellow-up test this
// also matches the rhythm-mode combo and the two countdowns fight.
static bool simonEnterHold(const bool *down) {
  return down[IDX_LEFT[1]] && down[IDX_RIGHT[1]] && !down[IDX_LEFT[0]] && !down[IDX_RIGHT[0]];
}

static bool simonQuitHold(const bool *down) { return down[IDX_LEFT[0]] && down[IDX_RIGHT[0]]; }

void simonGameInit() {
  s_phase = SIMON_OFF;
  s_seqLen = 0;
  s_round = 0;
}

void simonGameLoop(uint32_t now, const bool *down, const bool *edgeDown) {
  if (s_phase == SIMON_OFF) {
    if (simonEnterHold(down)) {
      if (s_enterHoldStart == 0)
        s_enterHoldStart = now;
      s_enterHeldMs = now - s_enterHoldStart;
      if (s_enterHeldMs >= SIMON_ENTER_HOLD_MS)
        simonStart(now);
    } else {
      s_enterHoldStart = 0;
      s_enterHeldMs = 0;
    }
    return;
  }

  // Bail out from anywhere on the yellow pair.
  if (simonQuitHold(down)) {
    if (s_quitHoldStart == 0)
      s_quitHoldStart = now;
    if (now - s_quitHoldStart >= SIMON_QUIT_HOLD_MS) {
      simonStop(now, "quit hold");
      return;
    }
  } else {
    s_quitHoldStart = 0;
  }

  switch (s_phase) {
    case SIMON_INTRO:
      if (now - s_phaseStartMs >= SIMON_INTRO_MS)
        simonBeginRound(now);
      break;

    case SIMON_PLAYBACK: {
      const uint32_t litMs = simonStepMs(s_round);
      if (!s_stepLit) {
        if (now - s_stepStartMs >= SIMON_GAP_MS) {
          s_litKey = (int)s_seq[s_stepIdx];
          s_stepLit = true;
          s_stepStartMs = now;
          simonActuator(s_litKey, true);
        }
      } else if (now - s_stepStartMs >= litMs) {
        simonActuator(s_litKey, false);
        s_litKey = -1;
        s_stepLit = false;
        s_stepStartMs = now;
        if (++s_stepIdx >= s_seqLen) {
          s_phase = SIMON_INPUT;
          s_phaseStartMs = now;
          s_lastInputMs = now;
          s_inputIdx = 0;
        }
      }
      break;
    }

    case SIMON_INPUT: {
      if (now - s_lastInputMs >= SIMON_INPUT_TIMEOUT_MS) {
        s_wrongKey = -1;
        s_phase = SIMON_FAIL;
        s_phaseStartMs = now;
        Serial.printf("[SIMON] timeout at step %d/%d\n", s_inputIdx + 1, s_seqLen);
        break;
      }
      for (int i = 0; i < 10; i++) {
        if (!edgeDown[i])
          continue;
        if (!simonKeyInPool(i, s_round))
          continue; // keys outside this round's pool are ignored, not a loss
        s_lastInputMs = now;
        if ((uint8_t)i == s_seq[s_inputIdx]) {
          if (++s_inputIdx >= s_seqLen) {
            s_phase = SIMON_GOOD;
            s_phaseStartMs = now;
          }
        } else {
          s_wrongKey = i;
          s_phase = SIMON_FAIL;
          s_phaseStartMs = now;
          Serial.printf("[SIMON] wrong key at step %d/%d\n", s_inputIdx + 1, s_seqLen);
        }
        break;
      }
      break;
    }

    case SIMON_GOOD:
      if (now - s_phaseStartMs >= SIMON_GOOD_MS)
        simonBeginRound(now);
      break;

    case SIMON_FAIL:
      if (now - s_phaseStartMs >= SIMON_FAIL_MS)
        simonStop(now, "game over");
      break;

    default:
      break;
  }
}

bool simonGameIsActive() { return s_phase != SIMON_OFF; }
// Deliberately does NOT include the enter countdown. Suppressing during the hold
// would kill the synth the instant both blues go down — and blue is a playable note,
// so the keys would go silent for 5 s before anything happened. The LCD dispatch
// handles the countdown panel separately, which is how rhythm mode does it too.
bool simonGameSuppressNormalUi() { return s_phase != SIMON_OFF; }
bool simonGameEnterCountdownActive() { return s_phase == SIMON_OFF && s_enterHoldStart != 0; }
uint32_t simonGameEnterHeldMs() { return s_enterHeldMs; }
uint32_t simonGameEnterTotalMs() { return SIMON_ENTER_HOLD_MS; }

// ------------------------------------------------------------------ audio ----
void simonGameAudioTargets(float &wantL, float &wantR) {
  wantL = 0.f;
  wantR = 0.f;
  if (s_phase == SIMON_PLAYBACK && s_litKey >= 0) {
    const float hz = simonKeyHz(s_litKey);
    if (simonKeyIsRight(s_litKey))
      wantR = hz;
    else
      wantL = hz;
  }
}

// ------------------------------------------------------------------- LEDs ----
static void simonKeyColor(int btnIdx, uint8_t &r, uint8_t &g, uint8_t &b) {
  // Reuse the panel's own colour identity so the sequence reads at a glance.
  getPressColorForGPIO(BTN_PINS[btnIdx], r, g, b);
  if (r == 0 && g == 0 && b == 0) { // fronts have no press colour — use white
    r = g = b = 200;
  }
}

void simonGameRenderLeds(uint32_t now) {
  uint8_t r = 0, g = 0, b = 0;

  if (s_phase == SIMON_FAIL) {
    // Hard red strobe on everything; the wrong key stays solid.
    const bool on = ((now - s_phaseStartMs) / 130u) % 2u == 0u;
    for (int i = 0; i < 10; i++) {
      const bool solid = (i == s_wrongKey);
      setLED_RGB(i, (solid || on) ? 255 : 0, 0, 0);
    }
    return;
  }

  if (s_phase == SIMON_GOOD) {
    const bool on = ((now - s_phaseStartMs) / 110u) % 2u == 0u;
    for (int i = 0; i < 10; i++)
      setLED_RGB(i, 0, on ? 220 : 0, 0);
    return;
  }

  for (int i = 0; i < 10; i++) {
    const bool inPool = simonKeyInPool(i, s_round > 0 ? s_round : 1);
    if (s_phase == SIMON_PLAYBACK && i == s_litKey) {
      simonKeyColor(i, r, g, b);
      setLED_RGB(i, r, g, b);
    } else if (s_phase == SIMON_INPUT && down[i] && inPool) {
      simonKeyColor(i, r, g, b);
      setLED_RGB(i, r, g, b);
    } else if (s_phase == SIMON_INPUT && inPool) {
      // Dim idle so the player can see which keys are live this round.
      simonKeyColor(i, r, g, b);
      setLED_RGB(i, (uint8_t)(r / 12), (uint8_t)(g / 12), (uint8_t)(b / 12));
    } else {
      setLED_RGB(i, 0, 0, 0);
    }
  }
}

// -------------------------------------------------------------------- LCD ----
bool simonGameDrawLcd(uint32_t now) {
  if (s_phase == SIMON_OFF)
    return false;

  switch (s_phase) {
    case SIMON_INTRO:
      lcdSimonBanner(now, "F O L L O W", "L E A D E R");
      return true;
    case SIMON_PLAYBACK:
      lcdSimonStatus(now, s_round, s_stepIdx + 1, s_seqLen, false, simonPoolSize(s_round));
      return true;
    case SIMON_INPUT:
      lcdSimonStatus(now, s_round, s_inputIdx + 1, s_seqLen, true, simonPoolSize(s_round));
      return true;
    case SIMON_GOOD:
      lcdSimonBanner(now, "N I C E", "R O U N D  U P");
      return true;
    case SIMON_FAIL:
      lcdSimonGameOver(now, s_round);
      return true;
    default:
      return false;
  }
}

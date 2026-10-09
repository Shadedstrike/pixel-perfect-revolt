#include "pi_link.h"
#include "rhythm_game.h"
#include "display.h"
#include "leds.h"
#include <string.h>
#include <stdlib.h>

static bool s_piGame = false;
static uint32_t s_lastPiGameCommandMs = 0;
static constexpr uint32_t PI_LINK_FAILSAFE_MS = 3000;
static char s_line[64];
static size_t s_lineLen = 0;
static bool s_discardLine = false;
static uint32_t s_lastBeatMs = 0;
static uint8_t s_score = 100;
static bool s_lcdDirty = true;
static bool s_lcdNeedsFullDraw = true;
static uint32_t s_lastLedRenderMs = 0;
static char s_judgement[8] = "";
static uint32_t s_judgementMs = 0;
static uint32_t s_holdStartMs = 0;
static bool s_meltdownActive = false;
static uint32_t s_meltdownStartMs = 0;
static constexpr uint32_t PI_LED_FRAME_MS = 25; // 40 Hz; bounds shared I2C traffic.
static constexpr uint32_t PI_JUDGEMENT_MS = 700;
static constexpr uint32_t PI_MELTDOWN_HOLD_MS = 5000;
static constexpr uint32_t PI_BUTTON_MAX_AGE_MS = 500;
static constexpr uint8_t PI_BUTTON_QUEUE_SIZE = 32;
struct PendingButton {
  uint8_t index;
  bool pressed;
  uint32_t queuedMs;
};
static PendingButton s_buttonQueue[PI_BUTTON_QUEUE_SIZE];
static uint8_t s_buttonHead = 0;
static uint8_t s_buttonCount = 0;

static void piLinkClearButtons() {
  s_buttonHead = 0;
  s_buttonCount = 0;
}

static void piLinkFlushButtons(uint32_t nowMs) {
  while (s_buttonCount > 0) {
    const PendingButton &button = s_buttonQueue[s_buttonHead];
    if ((uint32_t)(nowMs - button.queuedMs) > PI_BUTTON_MAX_AGE_MS) {
      s_buttonHead = (uint8_t)((s_buttonHead + 1u) % PI_BUTTON_QUEUE_SIZE);
      --s_buttonCount;
      continue;
    }

    char line[20];
    const int length = snprintf(line, sizeof(line), button.pressed ? "PPR1 BTN %u\n" : "PPR1 BTN_UP %u\n",
                                (unsigned)button.index);
    if (length <= 0 || length >= (int)sizeof(line)) {
      s_buttonHead = (uint8_t)((s_buttonHead + 1u) % PI_BUTTON_QUEUE_SIZE);
      --s_buttonCount;
      continue;
    }
    // Serial is configured non-blocking. Wait until the entire protocol record
    // fits so a debug backlog cannot truncate or corrupt a button line.
    if (Serial.availableForWrite() < length)
      return;
    if (Serial.write((const uint8_t *)line, (size_t)length) != (size_t)length)
      return;
    s_buttonHead = (uint8_t)((s_buttonHead + 1u) % PI_BUTTON_QUEUE_SIZE);
    --s_buttonCount;
  }
}

static void piLinkSetGameMode(bool enabled, uint32_t nowMs) {
  if (enabled) {
    s_lastPiGameCommandMs = nowMs;
    if (!s_piGame) {
      // The Pi owns song playback. Make sure the controller's internal MP3 mode
      // cannot play a second copy through the shared analog mixer.
      rhythmGameStopForExternalAudio();
      s_piGame = true;
      s_score = 100;
      s_lastBeatMs = 0;
      s_lcdDirty = true;
      s_lcdNeedsFullDraw = true;
      s_lastLedRenderMs = 0;
      s_judgement[0] = 0;
      s_holdStartMs = 0;
      s_meltdownActive = false;
      Serial.println("[PI] MODE PI_GAME OK (local synth muted)");
    }
  } else if (s_piGame) {
    s_piGame = false;
    piLinkClearButtons();
    s_lcdDirty = true;
    s_lcdNeedsFullDraw = true;
    s_holdStartMs = 0;
    s_meltdownActive = false;
    Serial.println("[PI] MODE NORMAL OK (local synth restored)");
  }
}

static void piLinkHandleLine(const char *line, uint32_t nowMs) {
  if (strcmp(line, "PPR1 MODE PI_GAME") == 0) {
    piLinkSetGameMode(true, nowMs);
  } else if (strcmp(line, "PPR1 MODE NORMAL") == 0) {
    piLinkSetGameMode(false, nowMs);
  } else if (strcmp(line, "PPR1 BEAT") == 0) {
    if (s_piGame)
      s_lastBeatMs = nowMs;
  } else if (strncmp(line, "PPR1 SCORE ", 11) == 0) {
    long score = strtol(line + 11, nullptr, 10);
    if (s_piGame && score >= 0 && score <= 100) {
      s_score = (uint8_t)score;
      s_lcdDirty = true;
    }
  } else if (strncmp(line, "PPR1 JUDGE ", 11) == 0) {
    const char *label = line + 11;
    if (s_piGame && (strcmp(label, "HIT") == 0 || strcmp(label, "MISS") == 0 ||
                     strcmp(label, "PERFECT") == 0)) {
      strncpy(s_judgement, label, sizeof(s_judgement) - 1u);
      s_judgement[sizeof(s_judgement) - 1u] = 0;
      s_judgementMs = nowMs;
      s_lcdDirty = true;
    }
  } else if (strncmp(line, "PPR1", 4) == 0) {
    Serial.printf("[PI] ERR unknown command: %s\n", line);
  }
}

void piLinkSetup() {
  s_piGame = false;
  s_lastPiGameCommandMs = 0;
  s_lineLen = 0;
  s_discardLine = false;
  piLinkClearButtons();
}

void piLinkLoop(uint32_t nowMs) {
  // Gameplay input owns the first opportunity to use USB TX capacity each loop.
  piLinkFlushButtons(nowMs);
  // Bound work per frame so a noisy host cannot starve audio/button handling.
  int budget = 96;
  while (budget-- > 0 && Serial.available() > 0) {
    int raw = Serial.read();
    if (raw < 0)
      break;
    char c = (char)raw;
    if (c == '\r')
      continue;
    if (c == '\n') {
      s_line[s_lineLen] = 0;
      if (!s_discardLine && s_lineLen > 0)
        piLinkHandleLine(s_line, nowMs);
      s_lineLen = 0;
      s_discardLine = false;
      continue;
    }
    if (s_discardLine)
      continue;
    if (s_lineLen + 1 < sizeof(s_line)) {
      s_line[s_lineLen++] = c;
    } else {
      // Discard an oversized/malformed line and wait for its newline.
      s_lineLen = 0;
      s_discardLine = true;
    }
  }

  if (s_piGame && (uint32_t)(nowMs - s_lastPiGameCommandMs) > PI_LINK_FAILSAFE_MS) {
    s_piGame = false;
    piLinkClearButtons();
    Serial.println("[PI] heartbeat timeout: local synth restored");
  }
}

bool piLinkSynthMuted() { return s_piGame; }

void piLinkButtonPressed(uint8_t index, uint32_t nowMs) {
  if (!s_piGame)
    return;
  if (s_buttonCount == PI_BUTTON_QUEUE_SIZE) {
    // Preserve the freshest input during an exceptional host-side stall.
    s_buttonHead = (uint8_t)((s_buttonHead + 1u) % PI_BUTTON_QUEUE_SIZE);
    --s_buttonCount;
  }
  const uint8_t tail = (uint8_t)((s_buttonHead + s_buttonCount) % PI_BUTTON_QUEUE_SIZE);
  s_buttonQueue[tail] = {index, true, nowMs};
  ++s_buttonCount;
}

void piLinkButtonReleased(uint8_t index, uint32_t nowMs) {
  if (!s_piGame)
    return;
  if (s_buttonCount == PI_BUTTON_QUEUE_SIZE) {
    s_buttonHead = (uint8_t)((s_buttonHead + 1u) % PI_BUTTON_QUEUE_SIZE);
    --s_buttonCount;
  }
  const uint8_t tail = (uint8_t)((s_buttonHead + s_buttonCount) % PI_BUTTON_QUEUE_SIZE);
  s_buttonQueue[tail] = {index, false, nowMs};
  ++s_buttonCount;
}

void piLinkUpdateButtons(uint32_t nowMs, const bool down[10]) {
  if (!s_piGame)
    return;
  bool anyDown = false;
  for (int i = 0; i < 10; ++i)
    anyDown = anyDown || down[i];
  if (!anyDown) {
    if (s_meltdownActive) {
      s_meltdownActive = false;
      s_lcdNeedsFullDraw = true;
      s_lcdDirty = true;
    }
    s_holdStartMs = 0;
    return;
  }
  if (s_holdStartMs == 0)
    s_holdStartMs = nowMs;
  else if (!s_meltdownActive && (uint32_t)(nowMs - s_holdStartMs) >= PI_MELTDOWN_HOLD_MS) {
    s_meltdownActive = true;
    s_meltdownStartMs = nowMs;
    lcdRetroMeltdownBegin();
  }
}

void piLinkDrawLcd(uint32_t nowMs) {
  if (!s_piGame)
    return;
  if (s_meltdownActive) {
    lcdRetroHoldMeltdown(nowMs, s_meltdownStartMs);
    return;
  }
  if (s_judgement[0] != 0 && (uint32_t)(nowMs - s_judgementMs) >= PI_JUDGEMENT_MS) {
    s_judgement[0] = 0;
    s_lcdDirty = true;
  }
  if (!s_lcdDirty)
    return;
  if (s_lcdNeedsFullDraw) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("====================");
    lcd.setCursor(1, 1); lcd.print("RHYTHM GAME MODE");
    s_lcdNeedsFullDraw = false;
  }
  char judgementLine[21];
  memset(judgementLine, ' ', 20);
  judgementLine[20] = 0;
  const char *judgement = s_judgement[0] ? s_judgement : "ACTIVE";
  size_t judgementLen = strlen(judgement);
  if (judgementLen > 20u)
    judgementLen = 20u;
  memcpy(judgementLine + (20u - judgementLen) / 2u, judgement, judgementLen);
  lcd.setCursor(0, 2); lcd.print(judgementLine);
  char scoreLine[21];
  snprintf(scoreLine, sizeof(scoreLine), "SCORE %3u%%          ", (unsigned)s_score);
  lcd.setCursor(0, 3); lcd.print(scoreLine);
  s_lcdDirty = false;
}

void piLinkRenderLeds(uint32_t nowMs) {
  if (!s_piGame)
    return;
  if (s_lastLedRenderMs != 0 && (uint32_t)(nowMs - s_lastLedRenderMs) < PI_LED_FRAME_MS)
    return;
  s_lastLedRenderMs = nowMs;
  const uint32_t age = nowMs - s_lastBeatMs;
  // A crisp white beat attack fading over 180 ms on all side keys.
  uint8_t flash = age < 180u ? (uint8_t)(255u - (age * 255u / 180u)) : 0u;
  for (int i = 0; i < 10; ++i) {
    if (i == IDX_FRONT_L || i == IDX_FRONT_R)
      continue;
    setLED_RGB(i, flash, (uint8_t)(flash / 3u), flash);
  }

  // Front LEDs are a persistent red -> amber -> green performance meter;
  // a beat briefly lifts them toward white without hiding the score colour.
  uint8_t r = s_score < 50 ? 255 : (uint8_t)((100 - s_score) * 255 / 50);
  uint8_t g = s_score > 50 ? 255 : (uint8_t)(s_score * 255 / 50);
  uint8_t b = 0;
  if (flash > 0) {
    r = max(r, flash); g = max(g, flash); b = flash;
  }
  setLED_RGBRhythmFront(IDX_FRONT_L, r, g, b);
  setLED_RGBRhythmFront(IDX_FRONT_R, r, g, b);
}

#include "pi_link.h"
#include "rhythm_game.h"
#include <string.h>

static bool s_piGame = false;
static uint32_t s_lastPiGameCommandMs = 0;
static constexpr uint32_t PI_LINK_FAILSAFE_MS = 3000;
static char s_line[64];
static size_t s_lineLen = 0;
static bool s_discardLine = false;

static void piLinkSetGameMode(bool enabled, uint32_t nowMs) {
  if (enabled) {
    s_lastPiGameCommandMs = nowMs;
    if (!s_piGame) {
      // The Pi owns song playback. Make sure the controller's internal MP3 mode
      // cannot play a second copy through the shared analog mixer.
      rhythmGameStopForExternalAudio();
      s_piGame = true;
      Serial.println("[PI] MODE PI_GAME OK (local synth muted)");
    }
  } else if (s_piGame) {
    s_piGame = false;
    Serial.println("[PI] MODE NORMAL OK (local synth restored)");
  }
}

static void piLinkHandleLine(const char *line, uint32_t nowMs) {
  if (strcmp(line, "PPR1 MODE PI_GAME") == 0) {
    piLinkSetGameMode(true, nowMs);
  } else if (strcmp(line, "PPR1 MODE NORMAL") == 0) {
    piLinkSetGameMode(false, nowMs);
  } else if (strncmp(line, "PPR1", 4) == 0) {
    Serial.printf("[PI] ERR unknown command: %s\n", line);
  }
}

void piLinkSetup() {
  s_piGame = false;
  s_lastPiGameCommandMs = 0;
  s_lineLen = 0;
  s_discardLine = false;
}

void piLinkLoop(uint32_t nowMs) {
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
    Serial.println("[PI] heartbeat timeout: local synth restored");
  }
}

bool piLinkSynthMuted() { return s_piGame; }

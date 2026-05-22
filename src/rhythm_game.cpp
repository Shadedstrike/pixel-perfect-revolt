#include "rhythm_game.h"
#include "config.h"
#include "display.h"
#include "leds.h"
#include "rhythm_mp3.h"
#include <ctype.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Populated by scanning /rhythm/*.mp3 on SD only.
struct RhythmSongRuntime {
  char title[RHYTHM_TITLE_MAX];
  char path[RHYTHM_PATH_MAX];
  uint8_t difficulty;
  uint16_t bpm;
  uint16_t beatOffsetMs;
  uint32_t durationMs;
};
static RhythmSongRuntime s_songs[RHYTHM_MAX_SCAN_TRACKS];
static int s_numSongs = 0;
// Scan temp must not live on loopTask stack (~7.4 KiB) — default loop stack is 8 KiB and triggers a canary fault.
static RhythmTrackInfo s_scanScratch[RHYTHM_MAX_SCAN_TRACKS];

// 1..9 from BPM + approx beat count (duration × BPM); no DSP — good enough to order the menu.
static uint8_t rhythmDifficultyHeuristic(uint16_t bpm, uint32_t durationMs) {
  if (bpm < 40u)
    bpm = 120u;
  uint8_t d;
  if (bpm < 92u)
    d = 1;
  else if (bpm < 102u)
    d = 2;
  else if (bpm < 112u)
    d = 3;
  else if (bpm < 122u)
    d = 4;
  else if (bpm < 132u)
    d = 5;
  else if (bpm < 144u)
    d = 6;
  else if (bpm < 158u)
    d = 7;
  else if (bpm < 175u)
    d = 8;
  else
    d = 9;
  uint64_t estBeats = (uint64_t)durationMs * (uint64_t)bpm / 60000ull;
  if (estBeats >= 480ull && d < 9u)
    d++;
  if (estBeats >= 600ull && d < 9u)
    d++;
  if (estBeats >= 720ull)
    d = 9;
  return d;
}

static int cmp_songs_diff_then_title(const void *a, const void *b) {
  const RhythmSongRuntime *x = (const RhythmSongRuntime *)a;
  const RhythmSongRuntime *y = (const RhythmSongRuntime *)b;
  if (x->difficulty != y->difficulty)
    return (int)x->difficulty - (int)y->difficulty;
  const char *pa = x->title;
  const char *pb = y->title;
  while (*pa && *pb) {
    int ca = tolower((unsigned char)*pa++);
    int cb = tolower((unsigned char)*pb++);
    if (ca != cb)
      return ca - cb;
  }
  return (int)(unsigned char)*pa - (int)(unsigned char)*pb;
}

enum RPhase : uint8_t {
  RG_NORMAL = 0,
  RG_FLASH,
  RG_MENU,
  RG_GET_READY,
  RG_PLAYING,
  RG_RESULTS
};

static RPhase s_phase = RG_NORMAL;
static uint32_t s_eightHoldStart = 0;
static uint32_t s_flashStartMs = 0;
static int s_menuIdx = 0;
static uint32_t s_bothFrontMenuStart = 0;
static uint32_t s_exitHoldStart = 0;

static uint32_t s_songStartMs = 0;
static uint32_t s_songDurationMs = 0;
static uint16_t s_chartBpm = 120;
static uint16_t s_chartOffMs = 0;
static uint8_t s_chartDiff = 1;
static int s_playSongIdx = 0;

static uint32_t s_tapMs[192];
static int s_tapCount = 0;
// Wall time of last real button edge during play (for LCD hit flash; not synth-arm auto taps).
static uint32_t s_lastUiTapWallMs = 0;

// Front-button (16/46) beat feedback while playing
static uint32_t s_chartBeatsBuf[256];
static int s_nChartBeatsCached = 0;
static int s_lastJudgedBeat = -1;
// Beats after index 255 still use the same BPM grid for LED feedback (scoring uses full buildBeatTimes elsewhere).
static int s_ledExtrapBeatsJudged = 0;
// Last ~2s of judged beats only → front 16/46 hue (recent timing, not whole-song average).
static const int kLedRecentRing = 48;
static const uint32_t kLedRecentWindowMs = 2000;
static uint32_t s_ledRingBeatB[kLedRecentRing];
static float s_ledRingQ[kLedRecentRing];
static int s_ledRingW = 0;
static int s_ledRingSz = 0;

static int s_lastMainPct = 0;
static int s_lastBonusPct = 0;
static int s_lastCombinedPct = 0; // main + bonus (same as grade), for results LED mood
static char s_lastGrade = 'F';
static uint32_t s_resultsStartMs = 0;
static uint8_t s_resultsStage = 0;
static uint32_t s_resultsPromptStartMs = 0;

// In-play AFK: no button activity -> prompt, then return to normal console.
static const uint32_t RG_PLAY_AFK_AFTER_MS = 15000;
static const uint32_t RG_PLAY_AFK_PROMPT_MS = 10000;
// After AFK prompt times out: linear fade to silence before returning to normal idle.
static const uint32_t RG_AFK_FADE_OUT_MS = 3000;
// Ignore AFK until the song has run this long (avoids bogus idle if timestamps mis-order).
static const uint32_t RG_AFK_ARM_AFTER_SONG_MS = 4000;
static bool s_playingAfkPrompt = false;
static uint32_t s_lastPlayInteractMs = 0;
static uint32_t s_afkPromptStartMs = 0;
static uint32_t s_afkFadeStartMs = 0;
// Pre-kick intro: hold a side-column key to register one tap per chart beat (synth-arm feel).
static uint32_t s_synthArmLastBeatBucket = 0xffffffffu;

// Pre-roll before audio: GET READY … 3 … 2 … 1
static int s_grSongIdx = 0;
static uint32_t s_grStartMs = 0;
static const uint32_t RG_GR_READY_MS = 1300;
static const uint32_t RG_GR_COUNT_MS = 850;
static const uint32_t RG_GET_READY_TOTAL_MS = RG_GR_READY_MS + 3u * RG_GR_COUNT_MS;

static void rhythmReloadSongList() {
  int n = rhythmScanTrackFolder(s_scanScratch, RHYTHM_MAX_SCAN_TRACKS);
  if (n < 0)
    n = 0;
  if (n > RHYTHM_MAX_SCAN_TRACKS)
    n = RHYTHM_MAX_SCAN_TRACKS;
  s_numSongs = n;
  for (int i = 0; i < n; i++) {
    memset(&s_songs[i], 0, sizeof(s_songs[i]));
    strncpy(s_songs[i].title, s_scanScratch[i].title, sizeof(s_songs[i].title) - 1);
    strncpy(s_songs[i].path, s_scanScratch[i].path, sizeof(s_songs[i].path) - 1);
    s_songs[i].path[sizeof(s_songs[i].path) - 1] = 0;
    s_songs[i].durationMs = s_scanScratch[i].approxDurationMs;
    s_songs[i].bpm = s_scanScratch[i].bpm ? s_scanScratch[i].bpm : 120;
    s_songs[i].beatOffsetMs = s_scanScratch[i].beatOffsetMs;
    uint8_t tag = rhythmDifficultyFromFilename(s_songs[i].path);
    uint8_t fromSidecar = s_scanScratch[i].difficulty;
    s_songs[i].difficulty =
        fromSidecar ? fromSidecar : (tag ? tag : rhythmDifficultyHeuristic(s_songs[i].bpm, s_songs[i].durationMs));
    if (s_songs[i].difficulty < 1u)
      s_songs[i].difficulty = 1u;
    else if (s_songs[i].difficulty > 9u)
      s_songs[i].difficulty = 9u;
  }
  if (n > 1)
    qsort(s_songs, (size_t)n, sizeof(RhythmSongRuntime), cmp_songs_diff_then_title);
  if (s_numSongs > 0 && s_menuIdx >= s_numSongs)
    s_menuIdx = 0;
  Serial.printf("[RHYTHM] Loaded %d song(s) from /rhythm (by D then title)\n", s_numSongs);
}

static const uint32_t RG_ENTER_HOLD_MS = 1500;
// During PLAYING only: hold front 16 or 46 this long to menu / next track (taps still score on edge).
static const uint32_t RG_PLAY_FRONT_HOLD_MS = 1500;
static uint32_t s_playHold16Start = 0;
static uint32_t s_playHold46Start = 0;
static bool s_playHold16Fired = false;
static bool s_playHold46Fired = false;

static bool anyLeftColumnDown(const bool *down) {
  for (int k = 0; k < 4; k++) {
    if (down[IDX_LEFT[k]])
      return true;
  }
  return false;
}

static bool anyRightColumnDown(const bool *down) {
  for (int k = 0; k < 4; k++) {
    if (down[IDX_RIGHT[k]])
      return true;
  }
  return false;
}

static bool anySideColumnDown(const bool *down) {
  return anyLeftColumnDown(down) || anyRightColumnDown(down);
}

static bool anyRhythmPlayKeyDown(const bool *down) {
  return anySideColumnDown(down) || down[IDX_FRONT_L] || down[IDX_FRONT_R];
}

static bool edgeOnLeftColumn(const bool *edgeDown) {
  for (int k = 0; k < 4; k++) {
    if (edgeDown[IDX_LEFT[k]])
      return true;
  }
  return false;
}

static bool edgeOnRightColumn(const bool *edgeDown) {
  for (int k = 0; k < 4; k++) {
    if (edgeDown[IDX_RIGHT[k]])
      return true;
  }
  return false;
}

// Enter rhythm: GPIO 38 (left bottom) + GPIO 39 (yellow / right top), held RG_ENTER_HOLD_MS.
static bool rhythmEnterHold(const bool *down) {
  return down[IDX_38] && down[IDX_11];
}

static void buildBeatTimes(uint16_t bpm, uint16_t offsetMs, uint32_t durationMs, uint32_t *beats, int maxBeats,
                           int *nOut);
static void rhythmRebuildPlayingBeatCache();

static void rhythmBeginChartAt(int idx, uint32_t now) {
  if (s_numSongs <= 0 || idx < 0 || idx >= s_numSongs)
    return;
  const RhythmSongRuntime &S = s_songs[idx];
  s_playSongIdx = idx;
  s_menuIdx = idx;
  s_chartBpm = S.bpm;
  s_chartOffMs = S.beatOffsetMs;
  s_songDurationMs = S.durationMs;
  s_chartDiff = S.difficulty;
  s_tapCount = 0;
  s_lastUiTapWallMs = 0;
  s_songStartMs = now;
  s_phase = RG_PLAYING;
  s_bothFrontMenuStart = 0;
  s_resultsStage = 0;
  s_playingAfkPrompt = false;
  s_afkPromptStartMs = 0;
  s_afkFadeStartMs = 0;
  rhythmStreamSetFadeMul(1.f);
  s_lastPlayInteractMs = now;
  s_playHold16Start = 0;
  s_playHold46Start = 0;
  s_playHold16Fired = false;
  s_playHold46Fired = false;
  s_synthArmLastBeatBucket = 0xffffffffu;
  bool ok = false;
  if (S.path[0])
    ok = rhythmMp3TryPlay(S.path);
  if (!ok)
    rhythmSynthStart(s_chartBpm, s_chartOffMs, s_songDurationMs);
  uint16_t srcBpm = rhythmStreamSourceBpm();
  if (srcBpm >= 40u && srcBpm <= 320u) {
    s_chartBpm = srcBpm;
    s_songs[idx].bpm = srcBpm;
  }
  rhythmRebuildPlayingBeatCache();
}

static void rhythmStartGetReady(int idx, uint32_t now) {
  if (s_numSongs <= 0 || idx < 0 || idx >= s_numSongs)
    return;
  s_grSongIdx = idx;
  s_menuIdx = idx;
  s_grStartMs = now;
  s_phase = RG_GET_READY;
  s_bothFrontMenuStart = 0;
  Serial.printf("[RHYTHM] Get ready countdown for track %d\n", idx);
}

// Main score window (ms): looser than before — vibe / delayed taps, not frame-perfect.
static int rhythmMainWinMs() {
  // Tighter window as D rises (1=easiest … 9=harshest); floor keeps very high D playable.
  int w = 132 - (int)s_chartDiff * 7;
  return w < 68 ? 68 : w;
}

// Extra ms after the beat where taps still count as “on” (typical human lag vs audio).
static int rhythmFeedbackLateSlackMs() { return 68; }

static bool tapWithinFeedbackWindow(int tapRelMs, int beatMs) {
  int dt = tapRelMs - beatMs;
  int early = rhythmMainWinMs();
  int late = rhythmMainWinMs() + rhythmFeedbackLateSlackMs();
  return dt >= -early && dt <= late;
}

// Wall clock vs decoder clock can differ on MP3 (I2S starts after s_songStartMs). Use stream time for taps + LED grid.
static uint32_t rhythmSongPositionMs(uint32_t wallNow) {
  if (s_phase != RG_PLAYING)
    return 0;
  if (rhythmStreamIsActive())
    return rhythmStreamElapsedMs();
  return (wallNow > s_songStartMs) ? (wallNow - s_songStartMs) : 0;
}

static void ledRingReset() {
  s_ledRingW = 0;
  s_ledRingSz = 0;
}

static void ledRingPush(uint32_t beatMs, float q) {
  s_ledRingBeatB[s_ledRingW] = beatMs;
  s_ledRingQ[s_ledRingW] = q;
  s_ledRingW = (s_ledRingW + 1) % kLedRecentRing;
  if (s_ledRingSz < kLedRecentRing)
    s_ledRingSz++;
}

// Soft misses + bonus toward 1.0 when taps are near center of the window (not harsh red).
static float ledQualityForBeat(uint32_t beatMs) {
  int win = rhythmMainWinMs();
  int late = win + rhythmFeedbackLateSlackMs();
  bool haveTap = false;
  int bestAbs = 1000000000;
  int bestDt = 0;
  for (int t = 0; t < s_tapCount; t++) {
    int dt = (int)s_tapMs[t] - (int)beatMs;
    int a = abs(dt);
    if (!haveTap || a < bestAbs) {
      haveTap = true;
      bestAbs = a;
      bestDt = dt;
    }
  }
  if (!haveTap)
    return 0.62f;
  if (bestDt < -win || bestDt > late)
    return 0.70f;
  float denom = bestDt <= 0 ? (float)win : (float)late;
  float mag = fminf(1.f, fabsf((float)bestDt) / fmaxf(1.f, denom));
  float q = 0.76f + 0.24f * (1.f - mag);
  q += 0.14f * (1.f - mag) * (1.f - mag);
  if (q > 1.f)
    q = 1.f;
  return q;
}

static int ledRecentCountInWindow(uint32_t songRel) {
  int judgeLag = rhythmMainWinMs() + rhythmFeedbackLateSlackMs();
  int cnt = 0;
  for (int i = 0; i < s_ledRingSz; i++) {
    int idx = (s_ledRingW - 1 - i + kLedRecentRing * 4) % kLedRecentRing;
    uint32_t b = s_ledRingBeatB[idx];
    if (b > songRel)
      continue;
    if ((uint32_t)b + (uint32_t)judgeLag > songRel)
      continue;
    if (songRel - b > kLedRecentWindowMs)
      continue;
    cnt++;
  }
  return cnt;
}

static float ledRecentAverageQuality(uint32_t songRel) {
  int judgeLag = rhythmMainWinMs() + rhythmFeedbackLateSlackMs();
  float sum = 0.f;
  int cnt = 0;
  for (int i = 0; i < s_ledRingSz; i++) {
    int idx = (s_ledRingW - 1 - i + kLedRecentRing * 4) % kLedRecentRing;
    uint32_t b = s_ledRingBeatB[idx];
    if (b > songRel)
      continue;
    if ((uint32_t)b + (uint32_t)judgeLag > songRel)
      continue;
    if (songRel - b > kLedRecentWindowMs)
      continue;
    sum += s_ledRingQ[idx];
    cnt++;
  }
  if (cnt == 0)
    return 0.52f;
  return sum / (float)cnt;
}

static void rhythmRebuildPlayingBeatCache() {
  buildBeatTimes(s_chartBpm, s_chartOffMs, s_songDurationMs, s_chartBeatsBuf, 256, &s_nChartBeatsCached);
  s_lastJudgedBeat = -1;
  s_ledExtrapBeatsJudged = 0;
  ledRingReset();
}

static void rhythmAdvanceBeatLedState(uint32_t now) {
  uint32_t songRel = rhythmSongPositionMs(now);
  int judgeLag = rhythmMainWinMs() + rhythmFeedbackLateSlackMs();
  while (s_lastJudgedBeat + 1 < s_nChartBeatsCached) {
    int j = s_lastJudgedBeat + 1;
    if ((int)s_chartBeatsBuf[j] + judgeLag >= (int)songRel)
      break;
    uint32_t b = s_chartBeatsBuf[j];
    ledRingPush(b, ledQualityForBeat(b));
    s_lastJudgedBeat++;
  }

  // After the 256-sample cache, keep judging on the same fixed grid so LED feedback does not freeze mid-song.
  if (s_nChartBeatsCached > 0) {
    uint32_t periodMs = (uint32_t)(60000.f / fmaxf(40.f, (float)s_chartBpm) + 0.5f);
    if (periodMs < 1u)
      periodMs = 1u;
    uint32_t lastB = s_chartBeatsBuf[s_nChartBeatsCached - 1];
    uint32_t songCap = s_songDurationMs + (uint32_t)judgeLag + 2000u;
    for (;;) {
      uint64_t b64 = (uint64_t)lastB + (uint64_t)(s_ledExtrapBeatsJudged + 1) * (uint64_t)periodMs;
      if (b64 > (uint64_t)songCap)
        break;
      uint32_t b = (uint32_t)b64;
      if ((int)b + judgeLag >= (int)songRel)
        break;
      ledRingPush(b, ledQualityForBeat(b));
      s_ledExtrapBeatsJudged++;
    }
  }
}

static float rhythmNextBeatPulse(uint32_t songRel) {
  float bpm = fmaxf(40.f, (float)s_chartBpm);
  float periodF = 60000.f / bpm;
  uint32_t periodMs = (uint32_t)(periodF + 0.5f);
  if (periodMs < 1u)
    periodMs = 1u;

  if (s_nChartBeatsCached <= 0) {
    uint32_t rem = periodMs ? (songRel % periodMs) : 0u;
    uint32_t d = (rem == 0u) ? 0u : (periodMs - rem);
    float w = periodF * 0.3f;
    if (w < 45.f)
      w = 45.f;
    float p = 1.f - (float)d / w;
    if (p < 0.f)
      p = 0.f;
    if (p > 1.f)
      p = 1.f;
    return p;
  }

  int i = 0;
  while (i < s_nChartBeatsCached && s_chartBeatsBuf[i] < songRel)
    i++;

  uint32_t dms;
  if (i < s_nChartBeatsCached) {
    dms = s_chartBeatsBuf[i] - songRel;
  } else {
    // Past last cached beat: continue metronome from last grid time (was bug: next=songRel+period → pulse stuck at 0).
    uint32_t lastB = s_chartBeatsBuf[s_nChartBeatsCached - 1];
    if (songRel <= lastB) {
      dms = 0u;
    } else {
      uint64_t past = (uint64_t)songRel - (uint64_t)lastB;
      uint32_t rem = (uint32_t)(past % (uint64_t)periodMs);
      dms = (rem == 0u) ? 0u : (periodMs - rem);
    }
  }

  float w = periodF * 0.3f;
  if (w < 45.f)
    w = 45.f;
  float p = 1.f - (float)dms / w;
  if (p < 0.f)
    p = 0.f;
  if (p > 1.f)
    p = 1.f;
  return p;
}

// Time until next chart beat (same grid as scoring / LEDs); for LCD beat lane.
static uint32_t rhythmMsToNextBeat(uint32_t songRel) {
  float bpm = fmaxf(40.f, (float)s_chartBpm);
  uint32_t periodMs = (uint32_t)(60000.f / bpm + 0.5f);
  if (periodMs < 1u)
    periodMs = 1u;

  if (s_nChartBeatsCached <= 0) {
    uint32_t rem = periodMs ? (songRel % periodMs) : 0u;
    return (rem == 0u) ? 0u : (periodMs - rem);
  }

  int i = 0;
  while (i < s_nChartBeatsCached && s_chartBeatsBuf[i] < songRel)
    i++;

  if (i < s_nChartBeatsCached)
    return s_chartBeatsBuf[i] - songRel;

  uint32_t lastB = s_chartBeatsBuf[s_nChartBeatsCached - 1];
  if (songRel <= lastB)
    return 0u;
  uint64_t past = (uint64_t)songRel - (uint64_t)lastB;
  uint32_t rem = (uint32_t)(past % (uint64_t)periodMs);
  return (rem == 0u) ? 0u : (periodMs - rem);
}

static void buildBeatTimes(uint16_t bpm, uint16_t offsetMs, uint32_t durationMs, uint32_t *beats, int maxBeats,
                           int *nOut) {
  if (bpm < 40)
    bpm = 40;
  float period = 60000.0f / (float)bpm;
  int n = 0;
  float t = (float)offsetMs;
  while (t <= (float)durationMs + 5.f && n < maxBeats) {
    beats[n++] = (uint32_t)(t + 0.5f);
    t += period;
  }
  if (n == 0 && maxBeats > 0) {
    beats[0] = 0;
    n = 1;
  }
  *nOut = n;
}

static int scoreAgainstGrid(const uint32_t *taps, int nTaps, const uint32_t *beats, int nBeats, int winEarlyMs,
                            int winLateMs) {
  if (nBeats <= 0)
    return 0;
  int ti = 0;
  int hits = 0;
  int timingBonus = 0;
  for (int bi = 0; bi < nBeats; bi++) {
    while (ti < nTaps && (int)taps[ti] + winEarlyMs < (int)beats[bi])
      ti++;
    if (ti >= nTaps)
      break;
    int dt = (int)taps[ti] - (int)beats[bi];
    if (dt >= -winEarlyMs && dt <= winLateMs) {
      hits++;
      float denom = dt <= 0 ? (float)winEarlyMs : (float)winLateMs;
      float mag = fminf(1.f, fabsf((float)dt) / fmaxf(1.f, denom));
      timingBonus += (int)(4.f + 9.f * (1.f - mag));
      ti++;
    }
  }
  int base = (hits * 100) / nBeats;
  if (timingBonus > 20)
    timingBonus = 20;
  int out = base + timingBonus;
  if (out > 100)
    out = 100;
  return out;
}

static int scoreBonusHits(const uint32_t *taps, int nTaps, const uint32_t *bonus, int nBonus, int winMs, uint8_t diff) {
  if (nBonus <= 0 || diff < 2)
    return 0;
  int cap = 8 + (int)diff * 4;
  int got = 0;
  for (int b = 0; b < nBonus && got < cap; b++) {
    for (int t = 0; t < nTaps; t++) {
      if (abs((int)taps[t] - (int)bonus[b]) <= winMs) {
        got++;
        break;
      }
    }
  }
  return (got * 100) / cap;
}

static char gradeFromPercent(int p) {
  if (p >= 93)
    return 'A';
  if (p >= 85)
    return 'B';
  if (p >= 75)
    return 'C';
  if (p >= 60)
    return 'D';
  return 'F';
}

static void finishSong(uint32_t now) {
  s_playingAfkPrompt = false;
  s_afkPromptStartMs = 0;
  s_afkFadeStartMs = 0;
  rhythmMp3Stop();

  uint32_t beats[256];
  int nBeats = 0;
  buildBeatTimes(s_chartBpm, s_chartOffMs, s_songDurationMs, beats, 256, &nBeats);

  int winMain = rhythmMainWinMs();
  int winLate = winMain + rhythmFeedbackLateSlackMs();
  int mainPct = scoreAgainstGrid(s_tapMs, s_tapCount, beats, nBeats, winMain, winLate);

  const uint32_t *bonus = nullptr;
  int nBonus = 0;
  rhythmStreamGetBonusOnsets(&bonus, &nBonus);
  int bonusWin = winMain + 12;
  if (bonusWin > 160)
    bonusWin = 160;
  int bonusPct = scoreBonusHits(s_tapMs, s_tapCount, bonus, nBonus, bonusWin, s_chartDiff);

  int combined = mainPct;
  if (s_chartDiff >= 2 && bonusPct > 0) {
    combined = mainPct + bonusPct / 5;
    if (combined > 100)
      combined = 100;
  }

  s_lastMainPct = mainPct;
  s_lastBonusPct = bonusPct;
  s_lastCombinedPct = combined;
  s_lastGrade = gradeFromPercent(combined);
  s_phase = RG_RESULTS;
  s_resultsStage = 0;
  s_resultsStartMs = now;
  s_tapCount = 0;
}

void rhythmGameSetup() {
  if (!rhythmMp3FsInit())
    Serial.println("[RHYTHM] SD not mounted — insert FAT32 card with /rhythm/*.mp3 (synthetic fallback if no file)");
  rhythmReloadSongList();
}

static void rhythmMaybeSynthArmHoldTaps(uint32_t now, const bool *down);

void rhythmGameLoop(uint32_t now, const bool *down, const bool *edgeDown) {

  if (s_phase == RG_NORMAL) {
    if (rhythmEnterHold(down)) {
      if (s_eightHoldStart == 0)
        s_eightHoldStart = now;
      else if (now - s_eightHoldStart >= RG_ENTER_HOLD_MS) {
        s_phase = RG_FLASH;
        s_flashStartMs = now;
        s_eightHoldStart = 0;
        s_menuIdx = 0;
        Serial.println("[RHYTHM] Enter retro mode (GPIO38+GPIO39 held >=1.5s)");
      }
    } else
      s_eightHoldStart = 0;
    return;
  }

  bool exitHold = down[IDX_38] && down[IDX_11];
  if (exitHold) {
    if (s_exitHoldStart == 0)
      s_exitHoldStart = now;
    else if (s_phase != RG_PLAYING && now - s_exitHoldStart >= 2000) {
      rhythmMp3Stop();
      s_phase = RG_NORMAL;
      s_exitHoldStart = 0;
      s_bothFrontMenuStart = 0;
      Serial.println("[RHYTHM] Exit to normal mode");
      return;
    }
  } else
    s_exitHoldStart = 0;

  if (s_phase == RG_FLASH) {
    if (now - s_flashStartMs > lcdRetroFlashDurationMs()) {
      rhythmReloadSongList();
      s_phase = RG_MENU;
    }
    return;
  }

  if (s_phase == RG_MENU) {
    // Left column: exit rhythm → normal. Right column: play. 16/46: scroll list.
    if (edgeOnLeftColumn(edgeDown)) {
      rhythmMp3Stop();
      s_phase = RG_NORMAL;
      s_exitHoldStart = 0;
      s_bothFrontMenuStart = 0;
      Serial.println("[RHYTHM] Left column: exit to normal mode");
      return;
    }
    if (s_numSongs > 0) {
      if (edgeDown[IDX_FRONT_L])
        s_menuIdx = (s_menuIdx + s_numSongs - 1) % s_numSongs;
      if (edgeDown[IDX_FRONT_R])
        s_menuIdx = (s_menuIdx + 1) % s_numSongs;
      if (edgeOnRightColumn(edgeDown))
        rhythmStartGetReady(s_menuIdx, now);
    }
    return;
  }

  if (s_phase == RG_GET_READY) {
    if (edgeDown[IDX_FRONT_L]) {
      rhythmMp3Stop();
      s_phase = RG_MENU;
      s_menuIdx = s_grSongIdx;
      if (s_numSongs > 0)
        s_menuIdx %= s_numSongs;
      Serial.println("[RHYTHM] Front left (16): cancel get-ready");
      return;
    }
    uint32_t ge = now - s_grStartMs;
    if (ge >= RG_GET_READY_TOTAL_MS) {
#if defined(ARDUINO_ARCH_ESP32)
      esp_task_wdt_reset();
#endif
      rhythmBeginChartAt(s_grSongIdx, now);
      return;
    }
    return;
  }

  if (s_phase == RG_PLAYING) {
    rhythmAdvanceBeatLedState(now);
    rhythmMaybeSynthArmHoldTaps(now, down);
    if (!rhythmStreamIsActive()) {
      finishSong(now);
      return;
    }
    if (now - s_songStartMs >= s_songDurationMs + 800) {
      finishSong(now);
      return;
    }

    // Front 16 / 46: 1.5s hold only (short edges still score via rhythmGameOnButtonEdge).
    if (down[IDX_FRONT_L]) {
      if (s_playHold16Start == 0)
        s_playHold16Start = now;
      else if (!s_playHold16Fired && (now - s_playHold16Start) >= RG_PLAY_FRONT_HOLD_MS) {
        s_playHold16Fired = true;
        rhythmMp3Stop();
        s_phase = RG_MENU;
        s_menuIdx = s_playSongIdx;
        if (s_numSongs > 0)
          s_menuIdx %= s_numSongs;
        s_playingAfkPrompt = false;
        s_afkPromptStartMs = 0;
        s_afkFadeStartMs = 0;
        s_bothFrontMenuStart = 0;
        s_tapCount = 0;
        Serial.println("[RHYTHM] Front left held 1.5s: song menu");
        return;
      }
    } else {
      s_playHold16Start = 0;
      s_playHold16Fired = false;
    }

    if (down[IDX_FRONT_R] && s_numSongs > 0) {
      if (s_playHold46Start == 0)
        s_playHold46Start = now;
      else if (!s_playHold46Fired && (now - s_playHold46Start) >= RG_PLAY_FRONT_HOLD_MS) {
        s_playHold46Fired = true;
        rhythmMp3Stop();
        s_playingAfkPrompt = false;
        s_afkPromptStartMs = 0;
        s_afkFadeStartMs = 0;
        s_bothFrontMenuStart = 0;
        s_tapCount = 0;
        rhythmStartGetReady((s_playSongIdx + 1) % s_numSongs, now);
        Serial.println("[RHYTHM] Front right held 1.5s: next track");
        return;
      }
    } else {
      s_playHold46Start = 0;
      s_playHold46Fired = false;
    }

    bool anyDown = false;
    for (int i = 0; i < 10; i++) {
      if (down[i])
        anyDown = true;
    }
    if (anyDown) {
      s_lastPlayInteractMs = now;
      if (s_playingAfkPrompt) {
        s_playingAfkPrompt = false;
        s_afkPromptStartMs = 0;
        s_afkFadeStartMs = 0;
        rhythmStreamSetFadeMul(1.f);
        Serial.println("[RHYTHM] AFK prompt dismissed");
      }
    }

    if (s_playingAfkPrompt) {
      if (s_afkPromptStartMs > 0 && now >= s_afkPromptStartMs &&
          (now - s_afkPromptStartMs) >= RG_PLAY_AFK_PROMPT_MS) {
        if (s_afkFadeStartMs == 0) {
          s_afkFadeStartMs = now;
          Serial.println("[RHYTHM] AFK fade-out 3s then idle");
        }
        uint32_t fe = now - s_afkFadeStartMs;
        if (fe >= RG_AFK_FADE_OUT_MS) {
          rhythmStreamSetFadeMul(1.f);
          rhythmMp3Stop();
          s_phase = RG_NORMAL;
          s_playingAfkPrompt = false;
          s_afkPromptStartMs = 0;
          s_afkFadeStartMs = 0;
          s_bothFrontMenuStart = 0;
          s_exitHoldStart = 0;
          s_tapCount = 0;
          Serial.println("[RHYTHM] AFK prompt timeout -> normal idle");
        } else {
          float t = 1.f - (float)fe / (float)RG_AFK_FADE_OUT_MS;
          rhythmStreamSetFadeMul(t);
        }
      }
    } else {
      uint32_t idleMs = (now >= s_lastPlayInteractMs) ? (now - s_lastPlayInteractMs) : 0;
      bool songOldEnough = (now >= s_songStartMs) && ((now - s_songStartMs) >= RG_AFK_ARM_AFTER_SONG_MS);
      if (songOldEnough && idleMs >= RG_PLAY_AFK_AFTER_MS) {
        s_playingAfkPrompt = true;
        s_afkPromptStartMs = now;
        s_afkFadeStartMs = 0;
        Serial.println("[RHYTHM] AFK prompt (no input 15s)");
      }
    }
    return;
  }

  if (s_phase == RG_RESULTS) {
    if (edgeDown[IDX_FRONT_L]) {
      rhythmReloadSongList();
      s_phase = RG_MENU;
      s_menuIdx = s_playSongIdx;
      if (s_numSongs > 0)
        s_menuIdx %= s_numSongs;
      s_resultsStage = 0;
      Serial.println("[RHYTHM] Results: 16 -> song menu");
      return;
    }
    if (edgeDown[IDX_FRONT_R] && s_numSongs > 0) {
      rhythmStartGetReady((s_playSongIdx + 1) % s_numSongs, now);
      s_resultsStage = 0;
      Serial.println("[RHYTHM] Results: 46 -> next track");
      return;
    }
    if (s_resultsStage == 0) {
      if (now - s_resultsStartMs >= 5000) {
        s_resultsStage = 1;
        s_resultsPromptStartMs = now;
      }
    } else if (now - s_resultsPromptStartMs >= 15000) {
      rhythmMp3Stop();
      s_phase = RG_NORMAL;
      s_resultsStage = 0;
      s_bothFrontMenuStart = 0;
      s_exitHoldStart = 0;
      Serial.println("[RHYTHM] Prompt timeout -> normal idle");
    }
  }
}

bool rhythmGameIsActive() { return s_phase != RG_NORMAL; }

bool rhythmGameShouldSilenceSynth() {
  return s_phase == RG_PLAYING || s_phase == RG_FLASH || s_phase == RG_GET_READY;
}

bool rhythmGameSuppressNormalUi() { return s_phase != RG_NORMAL; }

void rhythmGameAudioPump() {
  if (s_phase == RG_PLAYING)
    rhythmStreamLoop();
}

bool rhythmGameOwnsAudioOutput() { return s_phase == RG_PLAYING; }

void rhythmGameGetSideColumnMusicLeds(uint32_t wallMs, uint8_t musicL[4][3], uint8_t musicR[4][3]) {
  for (int k = 0; k < 4; k++) {
    musicL[k][0] = musicL[k][1] = musicL[k][2] = 0;
    musicR[k][0] = musicR[k][1] = musicR[k][2] = 0;
  }
  if (!rhythmGameOwnsAudioOutput())
    return;
  if (s_playingAfkPrompt) {
    uint8_t r, g, b;
    hsv2rgb(215.f, 0.28f, 0.07f, r, g, b);
    for (int k = 0; k < 4; k++) {
      musicL[k][0] = r;
      musicL[k][1] = g;
      musicL[k][2] = b;
      musicR[k][0] = r;
      musicR[k][1] = g;
      musicR[k][2] = b;
    }
    return;
  }
  float bp, mp, bl, ml;
  rhythmStreamGetMusicVis(&bp, &mp, &bl, &ml);
  const float tw = wallMs * 0.001f;
  const float TAU = 6.28318530718f;
  for (int k = 0; k < 4; k++) {
    float stagger = (float)k * 0.11f;
    float bpk = bp * expf(-stagger * 4.2f);
    float bMix = fminf(1.f, bl * (0.38f + 0.17f * (float)k) + bpk * 0.92f);
    float hueL = fmodf(255.f + 42.f * bl + sinf(TAU * 0.35f * tw + (float)k * 0.55f) * 14.f, 360.f);
    float vL = 0.06f + 0.9f * powf(fmaxf(bMix, 0.f), 0.82f);
    float satL = 0.68f + 0.28f * bp;
    hsv2rgb(hueL, satL, vL, musicL[k][0], musicL[k][1], musicL[k][2]);

    float mpk = mp * expf(-stagger * 4.8f);
    float mMix = fminf(1.f, ml * (0.42f + 0.14f * (float)k) + mpk * 0.88f);
    float hueR = fmodf(158.f + 55.f * ml + sinf(TAU * 0.42f * tw + (float)k * 0.73f) * 18.f, 360.f);
    float vR = 0.05f + 0.88f * powf(fmaxf(mMix, 0.f), 0.8f);
    float satR = 0.62f + 0.32f * mp;
    hsv2rgb(hueR, satR, vR, musicR[k][0], musicR[k][1], musicR[k][2]);
  }
}

void rhythmGameGetFrontPlayingLeds(uint32_t now, uint8_t &rL, uint8_t &gL, uint8_t &bL, uint8_t &rR, uint8_t &gR,
                                   uint8_t &bR) {
  if (s_phase != RG_PLAYING) {
    rL = gL = bL = rR = gR = bR = 0;
    return;
  }
  if (s_playingAfkPrompt) {
    hsv2rgb(210.f, 0.45f, 0.13f, rL, gL, bL);
    rR = rL;
    gR = gL;
    bR = bL;
    return;
  }
  // Beat-sync + quality coloring only when the player has tapped recently (any play key edge).
  // Otherwise steady dim cyan — avoids 16/46 "dancing" on intro / AFK / synth-arm-only grids.
  const uint32_t kFrontLedRecentTapMs = 12000u;
  if (s_lastUiTapWallMs == 0 || (now - s_lastUiTapWallMs) > kFrontLedRecentTapMs) {
    hsv2rgb(210.f, 0.40f, 0.10f, rL, gL, bL);
    rR = rL;
    gR = gL;
    bR = bL;
    return;
  }
  uint32_t songRel = rhythmSongPositionMs(now);
  float pulse = rhythmNextBeatPulse(songRel);
  float flash = powf(pulse, 3.2f);
  float v = 0.09f + 0.86f * flash;
  // Red (0°) → green (120°): map actual ring quality band. Old "2 + aq*120" made aq=0.52 → yellow-green always.
  const float qLo = 0.62f; // soft miss / floor from ledQualityForBeat
  const float qHi = 0.99f; // near-perfect taps
  int nWin = ledRecentCountInWindow(songRel);
  if (nWin <= 0) {
    // No judged beats in the sliding window yet — beat flash only (cool), not misleading green.
    hsv2rgb(210.f, 0.48f, v, rL, gL, bL);
  } else {
    float aq = ledRecentAverageQuality(songRel);
    float t = (aq - qLo) / (qHi - qLo);
    if (t < 0.f)
      t = 0.f;
    else if (t > 1.f)
      t = 1.f;
    float hue = t * 120.f;
    hsv2rgb(hue, 0.93f, v, rL, gL, bL);
  }
  rR = rL;
  gR = gL;
  bR = bL;
}

static void rhythmGameRegisterTap(uint32_t songRelMs) {
  if (s_phase != RG_PLAYING)
    return;
  if (s_tapCount < (int)(sizeof(s_tapMs) / sizeof(s_tapMs[0])))
    s_tapMs[s_tapCount++] = songRelMs;
}

// Before the first strong bass hit in the decoded track, allow holding any play key (columns or 16/46)
// so each chart beat registers a tap — like arming with the guide grid without spamming edges in intros.
static void rhythmMaybeSynthArmHoldTaps(uint32_t now, const bool *down) {
  if (s_playingAfkPrompt)
    return;
  if (!rhythmStreamIsActive())
    return;
  if (rhythmStreamHasStrongBassOnsetYet())
    return;
  if (!anyRhythmPlayKeyDown(down))
    return;
  uint32_t rel = rhythmSongPositionMs(now);
  if (rhythmNextBeatPulse(rel) < 0.84f)
    return;
  uint16_t bpm = s_chartBpm >= 40u ? s_chartBpm : 120u;
  uint32_t period = (uint32_t)(60000u / (uint32_t)bpm);
  if (period < 1u)
    period = 1u;
  int64_t adjRel = (int64_t)rel - (int64_t)s_chartOffMs;
  uint32_t bucket = adjRel <= 0 ? 0u : (uint32_t)((uint64_t)adjRel / (uint64_t)period);
  if (bucket == s_synthArmLastBeatBucket)
    return;
  s_synthArmLastBeatBucket = bucket;
  rhythmGameRegisterTap(rel);
  s_lastPlayInteractMs = now;
}

void rhythmGameOnButtonEdge(uint32_t now) {
  if (s_phase != RG_PLAYING)
    return;
  s_lastPlayInteractMs = now;
  if (s_playingAfkPrompt) {
    s_playingAfkPrompt = false;
    s_afkPromptStartMs = 0;
    s_afkFadeStartMs = 0;
    rhythmStreamSetFadeMul(1.f);
    Serial.println("[RHYTHM] AFK prompt dismissed (tap)");
    return;
  }
  uint32_t rel = rhythmSongPositionMs(now);
  rhythmGameRegisterTap(rel);
  s_lastUiTapWallMs = now;
}

bool rhythmGameDrawLcd(uint32_t now) {
  static uint32_t s_lastLcd = 0;
  if (s_phase == RG_NORMAL)
    return false;

  uint32_t lcdThrottleMs = 220;
  if (s_phase == RG_MENU)
    lcdThrottleMs = 200;
  if (s_phase == RG_GET_READY)
    lcdThrottleMs = 80;
  if (s_phase == RG_RESULTS && s_resultsStage == 0)
    lcdThrottleMs = 350;
  if (s_phase == RG_PLAYING && !s_playingAfkPrompt)
    lcdThrottleMs = 110;
  if (s_phase == RG_PLAYING && s_playingAfkPrompt)
    lcdThrottleMs = 350;
  if (s_phase == RG_FLASH)
    lcdThrottleMs = 70;
  if (now - s_lastLcd < lcdThrottleMs)
    return false;
  s_lastLcd = now;

  if (s_phase == RG_FLASH) {
    lcdRetroFlashScreen(now, s_flashStartMs);
    return true;
  }
  if (s_phase == RG_GET_READY) {
    const char *st = (s_numSongs > 0 && s_grSongIdx >= 0 && s_grSongIdx < s_numSongs) ? s_songs[s_grSongIdx].title
                                                                                       : "";
    lcdRetroGetReady(now, s_grStartMs, st, RG_GR_READY_MS, RG_GR_COUNT_MS);
    return true;
  }
  if (s_phase == RG_MENU) {
    static RhythmSongRow s_menuRows[RHYTHM_MAX_SCAN_TRACKS];
    for (int i = 0; i < s_numSongs; i++) {
      s_menuRows[i].title = s_songs[i].title;
      s_menuRows[i].difficulty = s_songs[i].difficulty;
    }
    lcdRetroMenu(s_menuIdx, s_menuRows, s_numSongs, now);
    return true;
  }
  if (s_phase == RG_PLAYING) {
    if (s_playingAfkPrompt) {
      lcdRetroStillTherePrompt(now, s_afkPromptStartMs);
      return true;
    }
    uint32_t el = rhythmStreamElapsedMs();
    uint32_t rel = rhythmSongPositionMs(now);
    uint32_t mtn = rhythmMsToNextBeat(rel);
    lcdRetroPlaying(s_songs[s_playSongIdx].title, el, s_songDurationMs, s_chartBpm, now, mtn, s_lastUiTapWallMs);
    return true;
  }
  if (s_phase == RG_RESULTS) {
    if (s_resultsStage == 0)
      lcdRetroResultsScore(s_songs[s_playSongIdx].title, s_lastGrade, s_lastMainPct, s_lastBonusPct,
                           s_lastCombinedPct, now);
    else {
      const char *nextTitle = "(none)";
      if (s_numSongs > 0)
        nextTitle = s_songs[(s_playSongIdx + 1) % s_numSongs].title;
      lcdRetroResultsPrompt(now, nextTitle);
    }
    return true;
  }
  return true;
}

bool rhythmGameResultsAmbientLedsActive() { return s_phase == RG_RESULTS; }

void rhythmGameGetResultsMoodRgb(uint32_t now, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s_phase != RG_RESULTS) {
    r = g = b = 0;
    return;
  }
  float t = (float)s_lastCombinedPct * 0.01f;
  t = fmaxf(0.f, fminf(1.f, t));
  float hue = t * 120.f;
  float sat = 0.9f;
  float wave = sinf((float)now * 0.0024f);
  float pulseAmt = fmaxf(0.f, 1.f - t / 0.82f);
  pulseAmt *= pulseAmt;
  float vMid = 0.13f + 0.5f * t;
  float v = vMid + pulseAmt * 0.26f * wave;
  v = fmaxf(0.08f, fminf(0.92f, v));
  hsv2rgb(hue, sat, v, r, g, b);
}

void rhythmGameGetResultsFrontRgb(uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s_phase != RG_RESULTS) {
    r = g = b = 0;
    return;
  }
  float t = (float)s_lastCombinedPct * 0.01f;
  t = fmaxf(0.f, fminf(1.f, t));
  float hue = t * 120.f;
  float v = 0.12f + 0.30f * t;
  hsv2rgb(hue, 0.88f, v, r, g, b);
}

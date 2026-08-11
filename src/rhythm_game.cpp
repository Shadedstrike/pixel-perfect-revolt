#include "rhythm_game.h"
#include "buttons.h"
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
// Name is historic — this is the 4-top-key enter hold, not an eight-key one.
static uint32_t s_eightHoldStart = 0;
// How long the enter gesture has been held, 0 when not holding. Drives the LCD
// countdown so main.cpp can take the screen over without duplicating the timing.
static uint32_t s_enterHeldMs = 0;
static uint32_t s_flashStartMs = 0;
static int s_menuIdx = 0;
static uint32_t s_bothFrontMenuStart = 0;
static uint32_t s_exitHoldStart = 0;
static bool s_exitMenuLatched = false;

static uint32_t s_songStartMs = 0;
static uint32_t s_songDurationMs = 0;
static uint16_t s_chartBpm = 120;
static uint16_t s_chartOffMs = 0;
static uint8_t s_chartDiff = 1;
static int s_playSongIdx = 0;

static uint32_t s_tapMs[192];
static int s_tapCount = 0;
// Wall time of the last SUCCESSFUL hit during play.  Drives the HIT text flash.
static uint32_t s_lastHitWallMs = 0;
// Wall time of the last missed beat passing through the reticle.
static uint32_t s_lastMissWallMs = 0;
static constexpr int kMaxMissed = 24;
static uint32_t s_missedBeatMs[kMaxMissed];
static int      s_nMissed = 0;

// Per-beat consumed table — each entry records a beat's song-rel timestamp and
// the wall time it was consumed.  The display uses the per-entry wall time to
// drive burst (0-250ms) then hide (250ms-2s) independently per beat so hearts
// can't reappear after passing through the zone.
static constexpr int kMaxConsumed = 16;
static LcdConsumedEntry s_consumed[kMaxConsumed];
static int              s_nConsumed = 0;

// Buffer holding the per-frame beat list that the LCD beat lane renders.  Each frame
// we fill this with: real detected bass onsets (past) + extrapolated bass predictions
// (near future) so the screen shows a Guitar-Hero-style approaching ribbon whose
// density tracks the song's actual rhythm.  Falls back to chart beats when no bass
// has been detected yet.
static uint32_t s_displayBeatsBuf[64];
static int      s_nDisplayBeats = 0;
// Last sample-clock song ms while audio was running — frozen after stream stops.
static uint32_t s_lastAudioRelMs = 0;
// Locked kick tempo + phase for the lane grid (stable scroll, attuned to bass).
static uint32_t s_laneScrollPeriodMs = 0;

// Front-button (16/46) beat feedback while playing
static uint32_t s_chartBeatsBuf[256];
static int s_nChartBeatsCached = 0;
static int s_lastJudgedBeat = -1;
static int s_lastJudgedBassOnset = -1;
// Mirrors rhythmStreamBassOnsetsDropped() the last time we resynced the index
// above. When the stream's ring slides, we subtract the delta from
// s_lastJudgedBassOnset so it keeps pointing at the same logical onset.
static uint32_t s_lastJudgedBassOnsetDropSeen = 0;
// Beats after index 255 still use the same BPM grid for LCD lane fallback.
static int s_ledExtrapBeatsJudged = 0;
// Rolling beat-quality ring → live 16/46 + LCD % use only the last kLiveFeedbackWindowMs.
static const int kLedRecentRing = 48;
static const uint32_t kLiveFeedbackWindowMs = 4000;
static uint32_t s_ledRingBeatB[kLedRecentRing];
static float s_ledRingQ[kLedRecentRing];
static int s_ledRingW = 0;
static int s_ledRingSz = 0;

// Kept during play so finish scoring still has onsets after MP3 stop clears the live tracker.
static uint32_t s_playBassSnap[128];
static int s_playBassSnapN = 0;
static uint32_t s_playBonusSnap[128];
static int s_playBonusSnapN = 0;

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
// Final grade + AFK only: first/last 15s of the song (live 16/46 score from beat 0).
static const uint32_t RG_SONG_EDGE_GRACE_MS = 15000;
// Ignore AFK until 15s into the song (and never in the last 15s).
static const uint32_t RG_AFK_ARM_AFTER_SONG_MS = 15000;
// Menu / results / get-ready: return to synth if no rhythm UI input for this long.
static const uint32_t RG_UI_IDLE_TO_SYNTH_MS = 25000;
static uint32_t s_rhythmUiLastMs = 0;
static bool s_playingAfkPrompt = false;
static uint32_t s_lastPlayInteractMs = 0;
static uint32_t s_afkPromptStartMs = 0;
static uint32_t s_afkFadeStartMs = 0;

// 5s any-key hold during play → LCD meltdown easter-egg (hearts explode until then).
static const uint32_t RG_PLAY_MELTDOWN_HOLD_MS = 5000u;
static uint32_t s_playAnyHoldStartMs = 0;
static bool     s_holdMeltdownActive = false;
static uint32_t s_holdMeltdownStartMs = 0;
static bool     s_meltdownRecoverActive = false;
static uint32_t s_meltdownRecoverStartMs = 0;
static uint32_t s_meltdownRecoverDurMs = 0;

// Pre-roll before audio: GET READY … 3 … 2 … 1
static int s_grSongIdx = 0;
static uint32_t s_grStartMs = 0;
static const uint32_t RG_GR_COUNT_MS = 850;

static uint32_t rgGetReadyScrollMs(int idx) {
  const char *t = "";
  if (idx >= 0 && idx < s_numSongs)
    t = s_songs[idx].title;
  return lcdRetroTitleScrollDurationMs(t, 20u);
}

static uint32_t rgGetReadyTotalMs(int idx) { return rgGetReadyScrollMs(idx) + 3u * RG_GR_COUNT_MS; }

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

static const uint32_t RG_ENTER_HOLD_MS = 5000;
// Yellow pair (GPIO 38 + 39, top of each column): 3s → song menu, 4s total → main idle (all rhythm phases).
static const uint32_t RG_EXIT_HOLD_MENU_MS = 3000;
static const uint32_t RG_EXIT_HOLD_IDLE_MS = 4000;
// Cap beat scoring / LED ring work per frame so fast BPM + many taps cannot wedge loopTask.
static const int kMaxBeatJudgePerFrame = 8;
// Pre-kick hold-to-tap assist disabled above this BPM (manual taps still score).
static const uint32_t kStreamEndGraceMs = 1200;
static const uint32_t RG_PAUSE_HOLD_MS = 3000;
static bool s_playPaused = false;
static bool s_resumeCountdown = false;
static uint32_t s_resumeCountStartMs = 0;
static uint32_t s_pauseHoldStart = 0;

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

// Exit rhythm: both yellow keys (GPIO 38 left top + GPIO 39 right top), held together.
static bool rhythmYellowPairHold(const bool *down) { return down[IDX_38] && down[IDX_11]; }

// Enter rhythm: all four top keys — both yellow (GPIO 38 / 39) AND both blue
// (GPIO 42 / 2). Four keys rather than two so it cannot be triggered by ordinary
// two-handed play. Exit stays on the yellow pair, which is a subset — harmless,
// since the exit check only runs once the phase is no longer RG_NORMAL.
static bool rhythmEnterHold(const bool *down) {
  return down[IDX_LEFT[0]] && down[IDX_LEFT[1]] && down[IDX_RIGHT[0]] && down[IDX_RIGHT[1]];
}

// Returns true when phase changed (caller should return from rhythmGameLoop).
static bool rhythmProcessBottomExitHold(uint32_t now, const bool *down) {
  if (!rhythmYellowPairHold(down)) {
    s_exitHoldStart = 0;
    s_exitMenuLatched = false;
    return false;
  }
  if (s_exitHoldStart == 0)
    s_exitHoldStart = now;
  uint32_t held = now - s_exitHoldStart;

  if (held >= RG_EXIT_HOLD_IDLE_MS) {
    rhythmMp3Stop();
    s_phase = RG_NORMAL;
    s_exitHoldStart = 0;
    s_exitMenuLatched = false;
    s_bothFrontMenuStart = 0;
    s_eightHoldStart = 0;
    s_playingAfkPrompt = false;
    s_afkPromptStartMs = 0;
    s_afkFadeStartMs = 0;
    s_tapCount = 0;
    rhythmStreamSetFadeMul(1.f);
    Serial.println("[RHYTHM] Bottom pair 4s: exit to idle");
    return true;
  }

  if (held >= RG_EXIT_HOLD_MENU_MS && !s_exitMenuLatched) {
    s_exitMenuLatched = true;
    if (s_phase == RG_MENU)
      return false;
    RPhase prev = s_phase;
    rhythmMp3Stop();
    s_phase = RG_MENU;
    if (prev == RG_GET_READY)
      s_menuIdx = s_grSongIdx;
    else if (s_playSongIdx >= 0)
      s_menuIdx = s_playSongIdx;
    if (s_numSongs > 0 && s_menuIdx >= s_numSongs)
      s_menuIdx %= s_numSongs;
    s_playingAfkPrompt = false;
    s_afkPromptStartMs = 0;
    s_afkFadeStartMs = 0;
    s_bothFrontMenuStart = 0;
    s_tapCount = 0;
    s_resultsStage = 0;
    rhythmStreamSetFadeMul(1.f);
    Serial.println("[RHYTHM] Bottom pair 3s: exit to song menu");
    return true;
  }
  return false;
}

static void buildBeatTimes(uint16_t bpm, uint16_t offsetMs, uint32_t durationMs, uint32_t *beats, int maxBeats,
                           int *nOut);
static void rhythmRebuildPlayingBeatCache();

static bool rhythmInSongIntroGrace(uint32_t songRelMs) {
  return songRelMs < RG_SONG_EDGE_GRACE_MS;
}

// AFK outro: last 15s of catalog length, or last 15s once playback has passed the estimate.
static bool rhythmInSongOutroWindow(uint32_t songRelMs) {
  uint32_t endMs = s_songDurationMs;
  uint32_t el = rhythmStreamElapsedMs();
  if (el > endMs)
    endMs = el;
  if (endMs <= RG_SONG_EDGE_GRACE_MS)
    return false;
  return songRelMs + RG_SONG_EDGE_GRACE_MS >= endMs;
}

static bool rhythmBeatOutsideScoreEdges(uint32_t beatMs, uint32_t playedMs) {
  if (beatMs < RG_SONG_EDGE_GRACE_MS)
    return true;
  uint32_t endMs = s_songDurationMs;
  if (playedMs > endMs)
    endMs = playedMs;
  if (endMs > RG_SONG_EDGE_GRACE_MS && beatMs + RG_SONG_EDGE_GRACE_MS > endMs)
    return true;
  return false;
}

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
  s_lastHitWallMs = 0;
  s_lastMissWallMs = 0;
  s_nMissed       = 0;
  s_nConsumed     = 0;
  s_laneScrollPeriodMs = 0;
  s_songStartMs   = now;
  s_phase = RG_PLAYING;
  s_bothFrontMenuStart = 0;
  s_resultsStage = 0;
  s_playingAfkPrompt = false;
  s_afkPromptStartMs = 0;
  s_afkFadeStartMs = 0;
  rhythmStreamSetFadeMul(1.f);
  s_lastPlayInteractMs = now;
  s_exitHoldStart = 0;
  s_exitMenuLatched = false;
  s_playPaused = false;
  s_resumeCountdown = false;
  s_pauseHoldStart = 0;
  rhythmStreamSetPaused(false);
  lcdRetroPlayingInvalidate();
  s_playAnyHoldStartMs  = 0;
  s_holdMeltdownActive  = false;
  s_holdMeltdownStartMs = 0;
  s_meltdownRecoverActive = false;
  s_meltdownRecoverStartMs = 0;
  s_meltdownRecoverDurMs = 0;
  s_lastAudioRelMs = 0;
  bool ok = false;
  if (S.path[0])
    ok = rhythmMp3TryPlay(S.path);
  if (!ok) {
    Serial.printf("[RHYTHM] MP3 failed for %s — rhythm mode is MP3 only (no guide tone)\n", S.path);
    s_phase = RG_MENU;
    return;
  }
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
  s_bothFrontMenuStart = 0;
  s_grStartMs = now;
  s_phase = RG_GET_READY;
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

// Use the audio sample clock so lane positions match bass-onset timestamps.
static uint32_t rhythmSongPositionMs(uint32_t wallNow) {
  if (s_phase != RG_PLAYING)
    return 0;
  if (rhythmStreamIsActive()) {
    s_lastAudioRelMs = rhythmStreamAudioPositionMs();
    return s_lastAudioRelMs;
  }
  return s_lastAudioRelMs;
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

static int rhythmPctFromAvgQuality(float q) {
  int pct = (int)(((q - 0.55f) / 0.45f) * 100.f + 0.5f);
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  return pct;
}

// Soft misses + bonus toward 1.0 when taps are near center of the window (not harsh red).
static float ledQualityForBeat(uint32_t beatMs) {
  int win = rhythmMainWinMs();
  int late = win + rhythmFeedbackLateSlackMs();
  if (s_tapCount <= 0)
    return 0.62f;
  bool haveTap = false;
  int bestAbs = 1000000000;
  int bestDt = 0;
  // Taps are chronological; scan backward — O(1) per beat when the chart is dense.
  const int kMaxTapScan = 56;
  int scanned = 0;
  for (int t = s_tapCount - 1; t >= 0 && scanned < kMaxTapScan; t--, scanned++) {
    int dt = (int)s_tapMs[t] - (int)beatMs;
    if (dt < -(win + 96))
      break;
    if (dt > late + 96)
      continue;
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
    if (songRel - b > kLiveFeedbackWindowMs)
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
    if (songRel - b > kLiveFeedbackWindowMs)
      continue;
    sum += s_ledRingQ[idx];
    cnt++;
  }
  if (cnt == 0)
    return 0.65f;
  return sum / (float)cnt;
}

// Fallback when the 4s window is empty but we still have older ring entries (avoids stuck 50% / yellow).
static float ledRecentAverageQualityFallback(uint32_t songRel) {
  int judgeLag = rhythmMainWinMs() + rhythmFeedbackLateSlackMs();
  float sum = 0.f;
  int cnt = 0;
  const int kFallbackMax = 12;
  for (int i = 0; i < s_ledRingSz && cnt < kFallbackMax; i++) {
    int idx = (s_ledRingW - 1 - i + kLedRecentRing * 4) % kLedRecentRing;
    uint32_t b = s_ledRingBeatB[idx];
    if (b > songRel)
      continue;
    if ((uint32_t)b + (uint32_t)judgeLag > songRel)
      continue;
    sum += s_ledRingQ[idx];
    cnt++;
  }
  if (cnt == 0)
    return 0.65f;
  return sum / (float)cnt;
}

// Live HUD only: last ~4s of beats (red=poor … green=great). Full song % is results only.
int rhythmGameLiveScorePct(uint32_t songRel) {
  int cnt = ledRecentCountInWindow(songRel);
  float q;
  if (cnt > 0)
    q = ledRecentAverageQuality(songRel);
  else if (s_ledRingSz > 0)
    q = ledRecentAverageQualityFallback(songRel);
  else
    return 0;
  return rhythmPctFromAvgQuality(q);
}

static void rhythmRebuildPlayingBeatCache() {
  buildBeatTimes(s_chartBpm, s_chartOffMs, s_songDurationMs, s_chartBeatsBuf, 256, &s_nChartBeatsCached);
  s_lastJudgedBeat = -1;
  s_lastJudgedBassOnset = -1;
  s_lastJudgedBassOnsetDropSeen = 0;
  s_ledExtrapBeatsJudged = 0;
  s_playBassSnapN = 0;
  s_playBonusSnapN = 0;
  ledRingReset();
}

static void rhythmRefreshPlayOnsetSnap() {
  const uint32_t *bass = nullptr;
  int nBass = 0;
  rhythmStreamGetBassOnsets(&bass, &nBass);
  if (nBass > (int)(sizeof(s_playBassSnap) / sizeof(s_playBassSnap[0])))
    nBass = (int)(sizeof(s_playBassSnap) / sizeof(s_playBassSnap[0]));
  if (nBass > 0 && bass) {
    memcpy(s_playBassSnap, bass, (size_t)nBass * sizeof(uint32_t));
    s_playBassSnapN = nBass;
  }
  const uint32_t *bonus = nullptr;
  int nBonus = 0;
  rhythmStreamGetBonusOnsets(&bonus, &nBonus);
  if (nBonus > (int)(sizeof(s_playBonusSnap) / sizeof(s_playBonusSnap[0])))
    nBonus = (int)(sizeof(s_playBonusSnap) / sizeof(s_playBonusSnap[0]));
  if (nBonus > 0 && bonus) {
    memcpy(s_playBonusSnap, bonus, (size_t)nBonus * sizeof(uint32_t));
    s_playBonusSnapN = nBonus;
  }
}

static int rhythmFinishMainPctFromRing(uint32_t playedMs) {
  if (s_ledRingSz <= 0)
    return -1;
  float sum = 0.f;
  int cnt = 0;
  for (int i = 0; i < s_ledRingSz; i++) {
    int idx = (s_ledRingW - s_ledRingSz + i + kLedRecentRing * 4) % kLedRecentRing;
    uint32_t b = s_ledRingBeatB[idx];
    if (rhythmBeatOutsideScoreEdges(b, playedMs))
      continue;
    sum += s_ledRingQ[idx];
    cnt++;
  }
  if (cnt <= 0)
    return -1;
  return rhythmPctFromAvgQuality(sum / (float)cnt);
}

static void rhythmExitToSynthIdle(const char *reason) {
  rhythmMp3Stop();
  s_phase = RG_NORMAL;
  s_exitHoldStart = 0;
  s_exitMenuLatched = false;
  s_bothFrontMenuStart = 0;
  s_eightHoldStart = 0;
  s_playingAfkPrompt = false;
  s_afkPromptStartMs = 0;
  s_afkFadeStartMs = 0;
  s_tapCount = 0;
  s_resultsStage = 0;
  s_rhythmUiLastMs = 0;
  rhythmStreamSetFadeMul(1.f);
  Serial.printf("[RHYTHM] %s\n", reason);
}

static bool rhythmAnyUiInput(const bool *down, const bool *edgeDown) {
  for (int i = 0; i < 10; i++) {
    if (down[i] || edgeDown[i])
      return true;
  }
  return false;
}

static uint32_t rhythmChartPeriodMs() {
  float bpm = fmaxf(40.f, (float)s_chartBpm);
  uint32_t periodMs = (uint32_t)(60000.f / bpm + 0.5f);
  return periodMs < 1u ? 1u : periodMs;
}

// Cached buildBeatTimes() stops at 256 entries (~2:08 @ 120 BPM); keep judging for live LEDs.
static uint32_t rhythmExtrapChartBeatMs(int extrapIndex) {
  if (s_nChartBeatsCached <= 0 || extrapIndex < 0)
    return UINT32_MAX;
  uint32_t lastB = s_chartBeatsBuf[s_nChartBeatsCached - 1];
  return lastB + rhythmChartPeriodMs() * (uint32_t)(extrapIndex + 1);
}

static void rhythmAdvanceBeatLedState(uint32_t now) {
  uint32_t songRel = rhythmSongPositionMs(now);
  int judgeLag = rhythmMainWinMs() + rhythmFeedbackLateSlackMs();
  int budget = kMaxBeatJudgePerFrame;

  const uint32_t *bass = nullptr;
  int nBass = 0;
  rhythmStreamGetBassOnsets(&bass, &nBass);
  // Compensate for any onsets that have rolled off the front of the ring so
  // s_lastJudgedBassOnset still references the same logical onset (or -1 if
  // it has itself been evicted).
  uint32_t dropNow = rhythmStreamBassOnsetsDropped();
  if (dropNow != s_lastJudgedBassOnsetDropSeen) {
    uint32_t delta = dropNow - s_lastJudgedBassOnsetDropSeen;
    s_lastJudgedBassOnsetDropSeen = dropNow;
    if (s_lastJudgedBassOnset >= 0) {
      int shifted = s_lastJudgedBassOnset - (int)delta;
      s_lastJudgedBassOnset = (shifted < -1) ? -1 : shifted;
    }
  }
  if (rhythmStreamHasStrongBassOnsetYet() && nBass > 0) {
    while (budget > 0 && s_lastJudgedBassOnset + 1 < nBass) {
      int j = s_lastJudgedBassOnset + 1;
      if ((int)bass[j] + judgeLag >= (int)songRel)
        break;
      ledRingPush(bass[j], ledQualityForBeat(bass[j]));
      s_lastJudgedBassOnset++;
      budget--;
    }
    if (budget <= 0)
      return;
  }

  // No kicks detected yet (or extra budget): fall back to chart grid so hue is not stuck neutral.
  while (budget > 0 && s_lastJudgedBeat + 1 < s_nChartBeatsCached) {
    int j = s_lastJudgedBeat + 1;
    if ((int)s_chartBeatsBuf[j] + judgeLag >= (int)songRel)
      break;
    ledRingPush(s_chartBeatsBuf[j], ledQualityForBeat(s_chartBeatsBuf[j]));
    s_lastJudgedBeat++;
    budget--;
  }

  // Past 256 cached grid beats (and 128 bass onsets): extrapolate BPM grid for live 16/46 + LCD %.
  while (budget > 0 && s_nChartBeatsCached > 0) {
    uint32_t beatMs = rhythmExtrapChartBeatMs(s_ledExtrapBeatsJudged);
    if (beatMs == UINT32_MAX)
      break;
    if ((int)beatMs + judgeLag >= (int)songRel)
      break;
    ledRingPush(beatMs, ledQualityForBeat(beatMs));
    s_ledExtrapBeatsJudged++;
    budget--;
  }
}

// Pulse from detected kick/low-mid onsets (returns <0 if none yet).
static float rhythmBassBeatPulse(uint32_t songRel) {
  const uint32_t *bass = nullptr;
  int nBass = 0;
  rhythmStreamGetBassOnsets(&bass, &nBass);
  if (nBass <= 0)
    return -1.f;

  uint32_t dms;
  if (songRel <= bass[0]) {
    dms = bass[0] - songRel;
  } else {
    int i = 0;
    while (i < nBass && bass[i] < songRel)
      i++;
    if (i < nBass)
      dms = bass[i] - songRel;
    else {
      uint32_t lastB = bass[nBass - 1];
      uint32_t periodMs = 140u;
      if (nBass >= 2) {
        periodMs = bass[nBass - 1] - bass[nBass - 2];
        if (periodMs < 80u)
          periodMs = 80u;
        if (periodMs > 400u)
          periodMs = 400u;
      }
      if (songRel <= lastB)
        dms = 0u;
      else {
        uint64_t past = (uint64_t)songRel - (uint64_t)lastB;
        uint32_t rem = (uint32_t)(past % (uint64_t)periodMs);
        dms = (rem == 0u) ? 0u : (periodMs - rem);
      }
    }
  }

  float w = 58.f;
  float p = 1.f - (float)dms / w;
  if (p < 0.f)
    p = 0.f;
  if (p > 1.f)
    p = 1.f;
  return p;
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

// Constant beat period (ms) derived from s_chartBpm — used by the visual scroll.
// We deliberately do NOT use detected-bass spacing here: kick detection produces
// per-onset gap variation (240ms / 280ms / 220ms…) which would make the scroll model
// re-scale every frame, causing hearts to teleport sideways like a spectrograph.
static uint32_t rhythmChartBeatPeriodMs() {
  float bpm = fmaxf(40.f, (float)s_chartBpm);
  uint32_t p = (uint32_t)(60000.f / bpm + 0.5f);
  return p < 1u ? 1u : p;
}

// Time until next kick (bass onsets) or chart grid fallback; for LCD beat lane.
static uint32_t rhythmMsToNextBeat(uint32_t songRel) {
  if (rhythmStreamHasStrongBassOnsetYet()) {
    const uint32_t *bass = nullptr;
    int nBass = 0;
    rhythmStreamGetBassOnsets(&bass, &nBass);
    for (int i = 0; i < nBass; i++) {
      if (bass[i] >= songRel)
        return bass[i] - songRel;
    }
    if (nBass >= 2) {
      uint32_t lastB = bass[nBass - 1];
      uint32_t periodMs = bass[nBass - 1] - bass[nBass - 2];
      if (periodMs < 80u)
        periodMs = 80u;
      if (periodMs > 400u)
        periodMs = 400u;
      if (songRel > lastB) {
        uint32_t rem = (songRel - lastB) % periodMs;
        return (rem == 0u) ? 0u : (periodMs - rem);
      }
    }
  }

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
                            int winLateMs, uint32_t playEndMs) {
  if (nBeats <= 0)
    return (nTaps > 0) ? 20 : 0;
  int ti = 0;
  int hits = 0;
  int timingBonus = 0;
  int scoredBeats = 0;
  for (int bi = 0; bi < nBeats; bi++) {
    if (beats[bi] > playEndMs + (uint32_t)winLateMs)
      break;
    scoredBeats++;
    while (ti < nTaps && (int)taps[ti] + winEarlyMs < (int)beats[bi])
      ti++;
    if (ti >= nTaps)
      continue;
    int dt = (int)taps[ti] - (int)beats[bi];
    if (dt >= -winEarlyMs && dt <= winLateMs) {
      hits++;
      float denom = dt <= 0 ? (float)winEarlyMs : (float)winLateMs;
      float mag = fminf(1.f, fabsf((float)dt) / fmaxf(1.f, denom));
      timingBonus += (int)(4.f + 9.f * (1.f - mag));
      ti++;
    }
  }
  if (scoredBeats <= 0)
    return (nTaps > 0) ? 20 : 0;
  int base = (hits * 100) / scoredBeats;
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

static int buildFinishScoreBeats(uint32_t *beats, int maxBeats, const uint32_t *bassSnap, int nBassSnap,
                                 uint32_t playedMs) {
  int n = 0;
  if (nBassSnap >= 4) {
    for (int i = 0; i < nBassSnap && n < maxBeats; i++) {
      uint32_t b = bassSnap[i];
      if (rhythmBeatOutsideScoreEdges(b, playedMs))
        continue;
      beats[n++] = b;
    }
    if (n >= 4)
      return n;
    n = 0;
  }

  uint32_t grid[256];
  int nGrid = 0;
  uint32_t gridDur = playedMs > 0 ? playedMs : s_songDurationMs;
  buildBeatTimes(s_chartBpm, s_chartOffMs, gridDur, grid, 256, &nGrid);
  for (int i = 0; i < nGrid && n < maxBeats; i++) {
    if (rhythmBeatOutsideScoreEdges(grid[i], playedMs))
      continue;
    beats[n++] = grid[i];
  }
  return n;
}

static void finishSong(uint32_t now) {
  s_playingAfkPrompt = false;
  s_afkPromptStartMs = 0;
  s_afkFadeStartMs = 0;

  uint32_t playedMs = rhythmStreamElapsedMs();
  if (playedMs == 0 && now > s_songStartMs)
    playedMs = now - s_songStartMs;

  rhythmRefreshPlayOnsetSnap();
  int nBassSnap = s_playBassSnapN;
  const uint32_t *bassSnap = nBassSnap > 0 ? s_playBassSnap : nullptr;
  int nBonusSnap = s_playBonusSnapN;
  const uint32_t *bonus = nBonusSnap > 0 ? s_playBonusSnap : nullptr;
  int nBonus = nBonusSnap;

  rhythmMp3Stop();

  uint32_t beats[256];
  int nBeats = buildFinishScoreBeats(beats, 256, bassSnap, nBassSnap, playedMs);

  int winMain = rhythmMainWinMs();
  int winLate = winMain + rhythmFeedbackLateSlackMs();
  int mainPct = scoreAgainstGrid(s_tapMs, s_tapCount, beats, nBeats, winMain, winLate, playedMs);
  int ringPct = rhythmFinishMainPctFromRing(playedMs);
  if (ringPct >= 0 && mainPct < ringPct)
    mainPct = ringPct;
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
    Serial.println("[RHYTHM] SD not mounted — insert FAT32 card with /rhythm/*.mp3");
  rhythmReloadSongList();
}

static void rhythmMaybeRegisterGoodHit(uint32_t rel, uint32_t now);
static void rhythmPlayHoldAssist(uint32_t rel, uint32_t now, const bool *down);
static void rhythmConsumeBeat(uint32_t beatMs, uint32_t now, uint32_t songRelMs, uint32_t scrollPeriodMs);
static int rhythmBeatDisplayCol(uint32_t beatMs, uint32_t songRelMs, uint32_t scrollPeriodMs);
static void rhythmBuildDisplayBeats(uint32_t songRel);
static uint32_t rhythmLaneBeatEndMs(uint32_t songRel, uint32_t scrollPeriod, bool haveBass, uint32_t lastKickMs);
static uint32_t rhythmLaneScrollPeriodMs(void);
static void rhythmCheckBeatMisses(uint32_t rel, uint32_t now, const bool *down);
static bool rhythmBeatNearMs(uint32_t a, uint32_t b, uint32_t fuzzMs);
static bool rhythmBeatWasConsumed(uint32_t beatMs, uint32_t fuzzMs);
static bool rhythmPlayAssistActive(const bool *down);
static uint32_t rhythmReticleHitWindowMs(void);
static uint32_t rhythmHoldAssistWindowMs(void);
static bool rhythmBeatAlreadyScored(uint32_t beatMs, uint32_t fuzzMs);
static void rhythmGameRegisterTapForBeat(uint32_t beatMs, uint32_t songRelMs);
static bool rhythmScoreNearestDisplayBeat(uint32_t rel, uint32_t now, bool requireUnscored);
static void rhythmUpdateHoldMeltdown(uint32_t now, const bool *down);

void rhythmGameLoop(uint32_t now, const bool *down, const bool *edgeDown) {

  if (s_phase == RG_NORMAL) {
    if (rhythmEnterHold(down)) {
      if (s_eightHoldStart == 0)
        s_eightHoldStart = now;
      s_enterHeldMs = now - s_eightHoldStart;
      if (s_enterHeldMs >= RG_ENTER_HOLD_MS) {
        s_phase = RG_FLASH;
        s_flashStartMs = now;
        s_eightHoldStart = 0;
        s_enterHeldMs = 0;
        s_menuIdx = 0;
        s_rhythmUiLastMs = now;
        Serial.println("[RHYTHM] Enter retro mode (4 top keys held >=5s)");
      }
    } else {
      s_eightHoldStart = 0;
      s_enterHeldMs = 0;
    }
    return;
  }

  if (rhythmProcessBottomExitHold(now, down))
    return;

  if (rhythmAnyUiInput(down, edgeDown))
    s_rhythmUiLastMs = now;
  else if (s_rhythmUiLastMs == 0)
    s_rhythmUiLastMs = now;

  if (s_phase != RG_PLAYING) {
    if (now - s_rhythmUiLastMs >= RG_UI_IDLE_TO_SYNTH_MS) {
      rhythmExitToSynthIdle("UI idle 25s -> synth");
      return;
    }
  }

  if (s_phase == RG_FLASH) {
    if (now - s_flashStartMs > lcdRetroFlashDurationMs()) {
      rhythmReloadSongList();
      s_phase = RG_MENU;
    }
    return;
  }

  if (s_phase == RG_MENU) {
    // Right column: play. 16/46: scroll. Exit: bottom pair hold (3s menu / 4s idle).
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
    uint32_t ge = now - s_grStartMs;
    if (ge >= rgGetReadyTotalMs(s_grSongIdx)) {
#if defined(ARDUINO_ARCH_ESP32)
      esp_task_wdt_reset();
#endif
      rhythmBeginChartAt(s_grSongIdx, now);
      return;
    }
    return;
  }

  if (s_phase == RG_PLAYING) {
    // 16+46: hold 3s to pause; while paused, hold 3s again to resume (release between toggles).
    if (!rhythmYellowPairHold(down) && down[IDX_FRONT_L] && down[IDX_FRONT_R]) {
      if (s_pauseHoldStart == 0)
        s_pauseHoldStart = now;
      else if (now - s_pauseHoldStart >= RG_PAUSE_HOLD_MS) {
        s_pauseHoldStart = 0;
        if (!s_playPaused) {
          rhythmStreamSetPaused(true);
          s_playPaused = true;
          s_resumeCountdown = false;
          Serial.println("[RHYTHM] Paused (16+46 held 3s)");
        } else {
          s_playPaused = false;
          s_resumeCountdown = true;
          s_resumeCountStartMs = now;
          rhythmStreamSetPaused(true);
          lcdRetroPlayingInvalidate();
          Serial.println("[RHYTHM] Resume countdown 3-2-1");
        }
      }
    } else if (!(down[IDX_FRONT_L] && down[IDX_FRONT_R])) {
      s_pauseHoldStart = 0;
    }

    if (s_resumeCountdown) {
      s_lastPlayInteractMs = now;
      rhythmStreamSetPaused(true);
      if (now - s_resumeCountStartMs >= 3u * RG_GR_COUNT_MS) {
        s_resumeCountdown = false;
        rhythmStreamSetPaused(false);
        lcdRetroPlayingInvalidate();
        Serial.println("[RHYTHM] Resumed after countdown");
      }
      return;
    }

    if (s_playPaused) {
      s_lastPlayInteractMs = now;
      rhythmStreamSetPaused(true);
      return;
    }

    // Feed MP3 before stream-active / duration checks (main loop also pumps, but order matters).
    rhythmGameAudioPump();
    rhythmRefreshPlayOnsetSnap();
    rhythmAdvanceBeatLedState(now);
    rhythmUpdateHoldMeltdown(now, down);

    if (s_holdMeltdownActive) {
      // Meltdown animation owns the LCD — keep the MP3 ring fed aggressively.
      rhythmGameAudioPumpN(6);
    } else {
      uint32_t rel = rhythmSongPositionMs(now);
      if (rhythmStreamIsActive() && rhythmStreamLaneActive()) {
        rhythmBuildDisplayBeats(rel);
        rhythmPlayHoldAssist(rel, now, down);
        rhythmCheckBeatMisses(rel, now, down);
      } else {
        s_nDisplayBeats = 0;
        s_lastMissWallMs = 0;
        s_lastHitWallMs = 0;
      }
      // Normal play: LCD + lane math starve I2S — pump harder than AFK (350 ms LCD throttle).
      if (!s_playPaused && !s_resumeCountdown && !s_playingAfkPrompt && !s_meltdownRecoverActive)
        rhythmGameAudioPumpN(4);
      else if (s_playAnyHoldStartMs != 0)
        rhythmGameAudioPump();
    }

    uint32_t playAge = (now >= s_songStartMs) ? (now - s_songStartMs) : 0;
    if (!rhythmStreamIsActive()) {
      if (playAge < kStreamEndGraceMs) {
        rhythmGameAudioPump();
      } else {
        Serial.println("[RHYTHM] MP3 stream ended — finishing chart");
        finishSong(now);
        return;
      }
    }
    uint32_t posMs = rhythmSongPositionMs(now);
    if (posMs >= s_songDurationMs + 800u) {
      if (rhythmStreamIsActive()) {
        uint32_t el = rhythmStreamElapsedMs();
        s_songDurationMs = el + 90000u;
      } else {
        finishSong(now);
        return;
      }
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
        lcdRetroPlayingInvalidate();
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
      uint32_t songRel = rhythmSongPositionMs(now);
      uint32_t idleMs = (now >= s_lastPlayInteractMs) ? (now - s_lastPlayInteractMs) : 0;
      bool songOldEnough = (now >= s_songStartMs) && ((now - s_songStartMs) >= RG_AFK_ARM_AFTER_SONG_MS);
      if (!rhythmInSongIntroGrace(songRel) && !rhythmInSongOutroWindow(songRel) && songOldEnough &&
          idleMs >= RG_PLAY_AFK_AFTER_MS) {
        s_playingAfkPrompt = true;
        s_afkPromptStartMs = now;
        s_afkFadeStartMs = 0;
        lcdRetroPlayingInvalidate();
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
  return rhythmGameIsActive();
}

bool rhythmGameSuppressNormalUi() { return s_phase != RG_NORMAL; }

bool rhythmGameEnterCountdownActive() { return s_phase == RG_NORMAL && s_eightHoldStart != 0; }
uint32_t rhythmGameEnterHeldMs() { return s_enterHeldMs; }
uint32_t rhythmGameEnterTotalMs() { return RG_ENTER_HOLD_MS; }

void rhythmGameAudioPump() {
  if (s_phase == RG_PLAYING)
    rhythmStreamLoop();
  // Sampling lives in the dedicated sampler task now (see inputFastPollStartTask).
  // Polling here as well was still gated by the same stalls it was meant to dodge.
  if (!inputFastPollTaskRunning())
    inputFastPoll(millis());
}

void rhythmGameAudioPumpN(int n) {
  if (n < 1)
    n = 1;
  for (int i = 0; i < n; i++)
    rhythmGameAudioPump();
}

// Entire retro rhythm flow (menu, countdown, play, results): MP3 only — never main-mode audioRender synth.
bool rhythmGameOwnsAudioOutput() { return rhythmGameIsActive(); }

bool rhythmGameFrontBeatLedsActive() {
  return s_phase == RG_PLAYING && !s_playPaused && !s_resumeCountdown && !s_playingAfkPrompt;
}

bool rhythmGameIsPaused() { return s_phase == RG_PLAYING && (s_playPaused || s_resumeCountdown); }

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
  if (s_playPaused || s_resumeCountdown) {
    hsv2rgb(42.f, 0.55f, 0.28f, rL, gL, bL);
    rR = rL;
    gR = gL;
    bR = bL;
    return;
  }
  if (s_playingAfkPrompt) {
    hsv2rgb(210.f, 0.45f, 0.13f, rL, gL, bL);
    rR = rL;
    gR = gL;
    bR = bL;
    return;
  }
  uint32_t songRel = rhythmSongPositionMs(now);
  float bassEnv = 0.f;
  rhythmStreamGetBassLedEnvelope(&bassEnv);

  float rawPulse = rhythmNextBeatPulse(songRel);
  if (rhythmStreamHasStrongBassOnsetYet()) {
    float bp = rhythmBassBeatPulse(songRel);
    if (bp >= 0.f)
      rawPulse = fmaxf(rawPulse, bp);
  }
  rawPulse = fmaxf(rawPulse, fminf(1.f, bassEnv * 1.15f));
  float beatFlash = powf(rawPulse, 0.22f);
  if (beatFlash < 0.08f)
    beatFlash = 0.08f;

  int scorePct = rhythmGameLiveScorePct(songRel);
  float hue = ((float)scorePct / 100.f) * 120.f;
  float sat = 0.90f + 0.10f * beatFlash;
  float v = 0.05f + 0.95f * beatFlash;
  hsv2rgb(hue, sat, v, rL, gL, bL);
  rR = rL;
  gR = gL;
  bR = bL;
}

static bool rhythmPlayAssistActive(const bool *down) {
  if (rhythmYellowPairHold(down))
    return false;
  if (down[IDX_FRONT_L] && down[IDX_FRONT_R])
    return false;
  return anyRhythmPlayKeyDown(down);
}

// Manual tap / miss window — bracket cols 8–12 plus a little slop.
static uint32_t rhythmReticleHitWindowMs(void) {
  uint32_t period = rhythmLaneScrollPeriodMs();
  uint32_t w = (4u * period) / 9u;
  if (w < 80u)
    w = 80u;
  if (w > 420u)
    w = 420u;
  return w;
}

// Generous window while holding — ~100% lane coverage at the cost of meltdown.
static uint32_t rhythmHoldAssistWindowMs(void) {
  uint32_t period = rhythmLaneScrollPeriodMs();
  uint32_t w = period / 2u;
  if (w < 100u)
    w = 100u;
  if (w > 520u)
    w = 520u;
  return w;
}

static bool rhythmBeatAlreadyScored(uint32_t beatMs, uint32_t fuzzMs) {
  for (int t = 0; t < s_tapCount; t++) {
    if (rhythmBeatNearMs(s_tapMs[t], beatMs, fuzzMs))
      return true;
  }
  return false;
}

static void rhythmGameRegisterTap(uint32_t songRelMs) {
  if (s_phase != RG_PLAYING)
    return;
  const int kMaxTaps = (int)(sizeof(s_tapMs) / sizeof(s_tapMs[0]));
  if (s_tapCount > 0) {
    uint32_t last = s_tapMs[s_tapCount - 1];
    uint32_t period = rhythmLaneScrollPeriodMs();
    uint32_t minGap = period / 8u;
    if (minGap < 16u)
      minGap = 16u;
    if (minGap > 42u)
      minGap = 42u;
    if (songRelMs >= last && (songRelMs - last) < minGap)
      return;
  }
  if (s_tapCount >= kMaxTaps) {
    memmove(s_tapMs, s_tapMs + 1, (size_t)(kMaxTaps - 1) * sizeof(s_tapMs[0]));
    s_tapCount = kMaxTaps - 1;
  }
  s_tapMs[s_tapCount++] = songRelMs;
}

// Perfect-align tap for scoring — one tap per beat, no global 42 ms choke on speedcore mash.
static void rhythmGameRegisterTapForBeat(uint32_t beatMs, uint32_t songRelMs) {
  if (s_phase != RG_PLAYING)
    return;
  if (rhythmBeatOutsideScoreEdges(beatMs, songRelMs))
    return;
  uint32_t scoreFuzz = rhythmLaneScrollPeriodMs() / 8u;
  if (scoreFuzz < 24u)
    scoreFuzz = 24u;
  if (rhythmBeatAlreadyScored(beatMs, scoreFuzz))
    return;
  const int kMaxTaps = (int)(sizeof(s_tapMs) / sizeof(s_tapMs[0]));
  if (s_tapCount >= kMaxTaps) {
    memmove(s_tapMs, s_tapMs + 1, (size_t)(kMaxTaps - 1) * sizeof(s_tapMs[0]));
    s_tapCount = kMaxTaps - 1;
  }
  s_tapMs[s_tapCount++] = beatMs;
}

static bool rhythmScoreNearestDisplayBeat(uint32_t rel, uint32_t now, bool requireUnscored) {
  if (s_nDisplayBeats <= 0)
    return false;
  uint32_t scrollPeriod = rhythmLaneScrollPeriodMs();
  uint32_t hitWindowMs = rhythmReticleHitWindowMs();
  uint32_t consumeFuzz = scrollPeriod / 4u;
  if (consumeFuzz < 40u)
    consumeFuzz = 40u;
  uint32_t scoreFuzz = scrollPeriod / 8u;
  if (scoreFuzz < 24u)
    scoreFuzz = 24u;

  int bestIdx = -1;
  uint32_t bestDiff = UINT32_MAX;
  for (int i = 0; i < s_nDisplayBeats; i++) {
    uint32_t b = s_displayBeatsBuf[i];
    uint32_t diff = (b > rel) ? (b - rel) : (rel - b);
    if (diff > hitWindowMs)
      continue;
    if (requireUnscored && rhythmBeatWasConsumed(b, consumeFuzz))
      continue;
    if (requireUnscored && rhythmBeatAlreadyScored(b, scoreFuzz))
      continue;
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIdx = i;
    }
  }
  if (bestIdx < 0)
    return false;
  uint32_t b = s_displayBeatsBuf[bestIdx];
  rhythmGameRegisterTapForBeat(b, rel);
  rhythmConsumeBeat(b, now, rel, scrollPeriod);
  s_lastHitWallMs = now;
  s_lastMissWallMs = 0;
  return true;
}

// Hold any play key: auto-hit every lane beat in window (visual explode + perfect score taps).
static void rhythmPlayHoldAssist(uint32_t rel, uint32_t now, const bool *down) {
  if (s_playingAfkPrompt || s_playPaused || s_resumeCountdown)
    return;
  if (!rhythmPlayAssistActive(down))
    return;
  if (s_nDisplayBeats <= 0)
    return;

  uint32_t scrollPeriod = rhythmLaneScrollPeriodMs();
  uint32_t hitWindowMs = rhythmHoldAssistWindowMs();
  uint32_t fuzz = scrollPeriod / 4u;
  if (fuzz < 40u)
    fuzz = 40u;

  bool anyHit = false;
  for (int i = 0; i < s_nDisplayBeats; i++) {
    uint32_t b = s_displayBeatsBuf[i];
    uint32_t diff = (b > rel) ? (b - rel) : (rel - b);
    if (diff > hitWindowMs)
      continue;
    if (rhythmBeatWasConsumed(b, fuzz))
      continue;
    rhythmGameRegisterTapForBeat(b, rel);
    rhythmConsumeBeat(b, now, rel, scrollPeriod);
    anyHit = true;
  }
  if (anyHit) {
    s_lastHitWallMs = now;
    s_lastMissWallMs = 0;
    s_lastPlayInteractMs = now;
  }
}

static void rhythmUpdateHoldMeltdown(uint32_t now, const bool *down) {
  if (s_meltdownRecoverActive) {
    if (now - s_meltdownRecoverStartMs >= s_meltdownRecoverDurMs) {
      s_meltdownRecoverActive = false;
      lcdRetroPlayingInvalidate();
    }
    return;
  }
  if (s_holdMeltdownActive) {
    if (!anyRhythmPlayKeyDown(down)) {
      uint32_t held = now - s_holdMeltdownStartMs;
      s_meltdownRecoverDurMs = lcdRetroMeltdownRecoverDurationMs(held);
      s_meltdownRecoverStartMs = now;
      s_meltdownRecoverActive = true;
      s_holdMeltdownActive = false;
      s_playAnyHoldStartMs = 0;
      lcdRetroPlayingInvalidate();
      Serial.printf("[RHYTHM] Meltdown release -> UI recover %lums (held %lums)\n",
                    (unsigned long)s_meltdownRecoverDurMs, (unsigned long)held);
    }
    return;
  }
  if (s_playPaused || s_resumeCountdown || s_playingAfkPrompt)
    return;
  if (rhythmYellowPairHold(down))
    return;
  if (down[IDX_FRONT_L] && down[IDX_FRONT_R])
    return;

  if (anyRhythmPlayKeyDown(down)) {
    if (s_playAnyHoldStartMs == 0)
      s_playAnyHoldStartMs = now;
    else if (now - s_playAnyHoldStartMs >= RG_PLAY_MELTDOWN_HOLD_MS) {
      s_holdMeltdownActive  = true;
      s_holdMeltdownStartMs = now;
      lcdRetroMeltdownBegin();
      Serial.println("[RHYTHM] Hold meltdown (5s any-key)");
    }
  } else {
    s_playAnyHoldStartMs = 0;
  }
}

// Add beatMs to the consumed table with a fresh wall timestamp, or refresh an
// existing entry.  Purges entries older than 2 s first to keep the table small.
static void rhythmConsumeBeat(uint32_t beatMs, uint32_t now, uint32_t songRelMs, uint32_t scrollPeriodMs) {
  // Drop consumed entries only once the beat has scrolled off the left edge.
  const int kCpp = 9;
  const int kHitColC = 10;
  uint32_t offLeftMs = ((uint32_t)(kHitColC + 1) * scrollPeriodMs + (uint32_t)kCpp - 1u) / (uint32_t)kCpp;
  int w = 0;
  for (int i = 0; i < s_nConsumed; i++) {
    uint32_t b = s_consumed[i].beatMs;
    if (songRelMs <= b || (songRelMs - b) < offLeftMs + 200u)
      s_consumed[w++] = s_consumed[i];
  }
  s_nConsumed = w;

  int col = rhythmBeatDisplayCol(beatMs, songRelMs, scrollPeriodMs);
  // Clamp to reticle interior so burst always lands inside the bracket.
  if (col < 9)
    col = 9;
  if (col > 11)
    col = 11;

  uint32_t fuzz = scrollPeriodMs / 4u;
  if (fuzz < 40u)
    fuzz = 40u;

  // Refresh column only on repeat consume — keep hitWallMs so burst/fade timer runs.
  for (int i = 0; i < s_nConsumed; i++) {
    if (rhythmBeatNearMs(s_consumed[i].beatMs, beatMs, fuzz)) {
      s_consumed[i].hitCol = (int8_t)col;
      return;
    }
  }
  // Insert new.
  if (s_nConsumed < kMaxConsumed) {
    s_consumed[s_nConsumed].beatMs    = beatMs;
    s_consumed[s_nConsumed].hitWallMs = now;
    s_consumed[s_nConsumed].hitCol    = (int8_t)col;
    s_nConsumed++;
  }
}

static void rhythmMaybeRegisterGoodHit(uint32_t rel, uint32_t now) {
  if (rhythmScoreNearestDisplayBeat(rel, now, true))
    return;

  uint32_t hitWindowMs = rhythmReticleHitWindowMs();
  uint32_t minDiff = UINT32_MAX;
  uint32_t nearestBeat = 0;

  if (rhythmStreamHasStrongBassOnsetYet()) {
    const uint32_t *bass = nullptr;
    int nBass = 0;
    rhythmStreamGetBassOnsets(&bass, &nBass);
    for (int i = 0; i < nBass; i++) {
      uint32_t diff = (bass[i] > rel) ? (bass[i] - rel) : (rel - bass[i]);
      if (diff < minDiff) {
        minDiff = diff;
        nearestBeat = bass[i];
      }
    }
  }
  if (minDiff > hitWindowMs && s_nChartBeatsCached > 0) {
    int lo = 0, hi = s_nChartBeatsCached - 1;
    while (lo < hi) {
      int mid = (lo + hi) >> 1;
      if (s_chartBeatsBuf[mid] < rel)
        lo = mid + 1;
      else
        hi = mid;
    }
    for (int i = lo - 1; i <= lo + 1; i++) {
      if (i < 0 || i >= s_nChartBeatsCached)
        continue;
      uint32_t b = s_chartBeatsBuf[i];
      uint32_t diff = (b > rel) ? (b - rel) : (rel - b);
      if (diff < minDiff) {
        minDiff = diff;
        nearestBeat = b;
      }
    }
  }
  if (minDiff > hitWindowMs)
    return;

  uint32_t scrollPeriod = rhythmLaneScrollPeriodMs();
  rhythmGameRegisterTapForBeat(nearestBeat, rel);
  rhythmConsumeBeat(nearestBeat, now, rel, scrollPeriod);
  s_lastHitWallMs = now;
  s_lastMissWallMs = 0;
}

void rhythmGameOnButtonEdge(uint32_t now) {
  if (s_phase != RG_PLAYING || s_playPaused || s_resumeCountdown)
    return;
  s_lastPlayInteractMs = now;
  if (s_playingAfkPrompt) {
    s_playingAfkPrompt = false;
    s_afkPromptStartMs = 0;
    s_afkFadeStartMs = 0;
    rhythmStreamSetFadeMul(1.f);
    lcdRetroPlayingInvalidate();
    Serial.println("[RHYTHM] AFK prompt dismissed (tap)");
    return;
  }
  uint32_t rel = rhythmSongPositionMs(now);
  rhythmBuildDisplayBeats(rel);
  if (!rhythmScoreNearestDisplayBeat(rel, now, true)) {
    rhythmGameRegisterTap(rel);
    rhythmMaybeRegisterGoodHit(rel, now);
  }
}

// Estimate the current bass-onset interval from the most recent detected onsets.
// Uses a sorted-median over up to the last 8 intervals to be robust against the
// detector occasionally missing or doubling a kick.  Returns 0 if not enough data.
static uint32_t rhythmEstimateBassPeriod(const uint32_t *bass, int nBass) {
  if (!bass || nBass < 2) return 0u;
  const int kMax = 8;
  uint32_t intervals[kMax];
  int nInt = 0;
  int start = (nBass - 1 - kMax > 0) ? (nBass - 1 - kMax) : 0;
  for (int i = start + 1; i < nBass && nInt < kMax; i++) {
    uint32_t d = bass[i] - bass[i - 1];
    if (d >= 80u && d <= 4000u)
      intervals[nInt++] = d;
  }
  if (nInt == 0) return 0u;
  for (int i = 0; i < nInt - 1; i++) {
    for (int j = 0; j < nInt - 1 - i; j++) {
      if (intervals[j] > intervals[j + 1]) {
        uint32_t t = intervals[j];
        intervals[j] = intervals[j + 1];
        intervals[j + 1] = t;
      }
    }
  }
  return intervals[nInt / 2];
}

static bool rhythmBeatNearMs(uint32_t a, uint32_t b, uint32_t fuzzMs) {
  return a >= b ? (a - b) <= fuzzMs : (b - a) <= fuzzMs;
}

static bool rhythmBeatWasConsumed(uint32_t beatMs, uint32_t fuzzMs) {
  for (int i = 0; i < s_nConsumed; i++) {
    if (rhythmBeatNearMs(s_consumed[i].beatMs, beatMs, fuzzMs))
      return true;
  }
  return false;
}

static bool rhythmBeatWasMarkedMissed(uint32_t beatMs, uint32_t fuzzMs) {
  for (int i = 0; i < s_nMissed; i++) {
    if (rhythmBeatNearMs(s_missedBeatMs[i], beatMs, fuzzMs))
      return true;
  }
  return false;
}

static void rhythmMarkBeatMissed(uint32_t beatMs) {
  if (s_nMissed >= kMaxMissed) {
    memmove(s_missedBeatMs, s_missedBeatMs + 1, (size_t)(kMaxMissed - 1) * sizeof(s_missedBeatMs[0]));
    s_nMissed = kMaxMissed - 1;
  }
  s_missedBeatMs[s_nMissed++] = beatMs;
}

// After a beat leaves the reticle unconsumed, flash MISS on the time row.
static void rhythmCheckBeatMisses(uint32_t rel, uint32_t now, const bool *down) {
  if (rhythmPlayAssistActive(down))
    return;
  if (!rhythmStreamIsActive())
    return;
  if (!rhythmStreamLaneActive())
    return;
  if (rhythmInSongIntroGrace(rel) || rhythmInSongOutroWindow(rel))
    return;
  if (s_nDisplayBeats <= 0)
    return;
  uint32_t period = rhythmLaneScrollPeriodMs();
  bool haveBass = rhythmStreamHasStrongBassOnsetYet();
  uint32_t lastKick = 0;
  if (haveBass) {
    const uint32_t *bass = nullptr;
    int nBass = 0;
    rhythmStreamGetBassOnsets(&bass, &nBass);
    if (nBass > 0)
      lastKick = bass[nBass - 1];
  }
  uint32_t laneEnd = rhythmLaneBeatEndMs(rel, period, haveBass, lastKick);
  uint32_t hitWindowMs = rhythmReticleHitWindowMs();
  uint32_t fuzz = period / 4u;
  if (fuzz < 40u)
    fuzz = 40u;

  for (int i = 0; i < s_nDisplayBeats; i++) {
    uint32_t b = s_displayBeatsBuf[i];
    if (b > laneEnd)
      continue;
    if (rhythmBeatOutsideScoreEdges(b, rel))
      continue;
    if (rel <= b + hitWindowMs)
      continue;
    if (rhythmBeatWasConsumed(b, fuzz))
      continue;
    if (rhythmBeatWasMarkedMissed(b, fuzz))
      continue;
    rhythmMarkBeatMissed(b);
    s_lastMissWallMs = now;
  }
}

// Lane column for a beat timestamp (matches display.cpp kCpp / kHitColC).
static int rhythmBeatDisplayCol(uint32_t beatMs, uint32_t songRelMs, uint32_t scrollPeriodMs) {
  const int kCpp = 9;
  const int kHitColC = 10;
  if (scrollPeriodMs < 1u)
    scrollPeriodMs = 1u;
  int64_t dt = (int64_t)beatMs - (int64_t)songRelMs;
  return kHitColC + (int)((dt * (int64_t)kCpp) / (int64_t)scrollPeriodMs);
}

// Lane scroll period: lock to detected kick spacing once bass is present, else chart BPM.
static uint32_t rhythmLaneScrollPeriodMs(void) {
  if (s_laneScrollPeriodMs >= 80u)
    return s_laneScrollPeriodMs;
  return rhythmChartBeatPeriodMs();
}

static void rhythmLockLaneFromBass(const uint32_t *bass, int nBass, uint32_t lastBass) {
  if (!bass || nBass < 2 || lastBass == 0)
    return;
  uint32_t est = rhythmEstimateBassPeriod(bass, nBass);
  if (est < 100u || est > 2000u)
    return;
  if (s_laneScrollPeriodMs == 0u) {
    s_laneScrollPeriodMs = est;
    s_chartBpm = (uint16_t)(60000u / est);
  } else {
    int32_t diff = (int32_t)est - (int32_t)s_laneScrollPeriodMs;
    if (diff > 25 || diff < -25)
      s_laneScrollPeriodMs = (uint32_t)((int32_t)s_laneScrollPeriodMs + diff / 8);
  }
}

// Furthest song ms that may spawn a lane heart — tied to audio/kicks, not blind BPM extrapolation.
static uint32_t rhythmLaneBeatEndMs(uint32_t songRel, uint32_t scrollPeriod, bool haveBass, uint32_t lastKickMs) {
  if (scrollPeriod < 1u)
    scrollPeriod = 1u;

  if (!rhythmStreamIsActive())
    return songRel;

  if (!rhythmStreamLaneActive())
    return songRel;

  // One beat ahead of playhead for the approach ribbon; wider only before first kick.
  uint32_t cap = songRel + scrollPeriod + 48u;
  if (!haveBass)
    cap = songRel + scrollPeriod * 2u + 48u;

  if (haveBass && lastKickMs > 0) {
    uint32_t kickCap = lastKickMs + scrollPeriod + scrollPeriod / 4u;
    if (cap > kickCap)
      cap = kickCap;
  }

  if (rhythmInSongOutroWindow(songRel)) {
    uint32_t endMs = s_songDurationMs;
    uint32_t el = rhythmStreamElapsedMs();
    if (el > endMs)
      endMs = el;
    if (cap > endMs)
      cap = endMs;
  }

  return cap;
}

// Build the per-frame display beat list — steady phase-locked grid for chronological lane scroll.
// Bass detection adjusts scroll period only; marker times stay on chart-origin grid lines so
// hearts never teleport when a kick is detected.
static void rhythmBuildDisplayBeats(uint32_t songRel) {
  s_nDisplayBeats = 0;

  const uint32_t *bass = nullptr;
  int nBass = 0;
  if (rhythmStreamHasStrongBassOnsetYet())
    rhythmStreamGetBassOnsets(&bass, &nBass);

  uint32_t scrollPeriod = rhythmLaneScrollPeriodMs();
  static const int kLaneCpp = 9;
  static const int kLaneHitColC = 10;
  static const int kLaneW = 20;
  uint32_t pastMs =
      ((uint32_t)kLaneHitColC * scrollPeriod + (uint32_t)kLaneCpp - 1u) / (uint32_t)kLaneCpp + 40u;
  uint32_t futureMs =
      ((uint32_t)(kLaneW - 1 - kLaneHitColC) * scrollPeriod + (uint32_t)kLaneCpp - 1u) / (uint32_t)kLaneCpp + 40u;
  uint32_t winStart = (pastMs > songRel) ? 0u : (songRel - pastMs);
  uint32_t winEnd   = songRel + futureMs;

  const int kMaxBeats = (int)(sizeof(s_displayBeatsBuf) / sizeof(s_displayBeatsBuf[0]));

  uint32_t lastBass = 0;
  bool haveBass = rhythmStreamHasStrongBassOnsetYet() && nBass > 0;
  if (haveBass) {
    for (int i = nBass - 1; i >= 0; i--) {
      if (bass[i] <= songRel) {
        lastBass = bass[i];
        break;
      }
    }
    if (lastBass == 0)
      lastBass = bass[nBass - 1];
    rhythmLockLaneFromBass(bass, nBass, lastBass);
  }

  scrollPeriod = rhythmLaneScrollPeriodMs();
  uint32_t laneEnd = rhythmLaneBeatEndMs(songRel, scrollPeriod, haveBass, lastBass);
  if (winEnd > laneEnd)
    winEnd = laneEnd;

  uint32_t origin = s_chartOffMs;

  bool allowGrid = !haveBass || songRel <= lastBass + scrollPeriod / 3u;
  if (allowGrid && scrollPeriod > 0) {
    uint32_t t = origin;
    while (t + scrollPeriod <= winStart)
      t += scrollPeriod;
    while (t <= winEnd && s_nDisplayBeats < kMaxBeats) {
      s_displayBeatsBuf[s_nDisplayBeats++] = t;
      t += scrollPeriod;
    }
  }
}

bool rhythmGameDrawLcd(uint32_t now) {
  static uint32_t s_lastLcd = 0;
  if (s_phase == RG_NORMAL)
    return false;

  if (s_phase == RG_PLAYING && !s_playingAfkPrompt && !s_resumeCountdown && !s_holdMeltdownActive && !s_playPaused) {
    if (rhythmStreamIsActive() && rhythmStreamLaneActive())
      rhythmBuildDisplayBeats(rhythmSongPositionMs(now));
  }

  uint32_t lcdThrottleMs = 220;
  if (s_phase == RG_MENU)
    lcdThrottleMs = 200;
  if (s_phase == RG_GET_READY)
    lcdThrottleMs = 80;
  if (s_phase == RG_RESULTS && s_resultsStage == 0)
    lcdThrottleMs = 350;
  if (s_phase == RG_PLAYING && s_resumeCountdown)
    lcdThrottleMs = 80;
  if (s_phase == RG_PLAYING && !s_playingAfkPrompt && !s_resumeCountdown && s_holdMeltdownActive)
    lcdThrottleMs = 130;
  if (s_phase == RG_PLAYING && !s_playingAfkPrompt && !s_resumeCountdown && !s_holdMeltdownActive)
    lcdThrottleMs = 80;
  if (s_phase == RG_PLAYING && lcdRetroPlayingNeedsFastLcd(now, rhythmSongPositionMs(now),
                                                          rhythmLaneScrollPeriodMs(), s_lastHitWallMs,
                                                          s_lastMissWallMs, s_displayBeatsBuf, s_nDisplayBeats,
                                                          s_consumed, s_nConsumed))
    lcdThrottleMs = 35;
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
    lcdRetroGetReady(now, s_grStartMs, st, rgGetReadyScrollMs(s_grSongIdx), RG_GR_COUNT_MS);
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
    if (s_resumeCountdown) {
      lcdRetroResumeCountdown(now, s_resumeCountStartMs, RG_GR_COUNT_MS, s_songs[s_playSongIdx].title, el,
                              s_songDurationMs);
      return true;
    }
    if (s_holdMeltdownActive) {
      lcdRetroHoldMeltdown(now, s_holdMeltdownStartMs);
      return true;
    }
    uint32_t rel = rhythmSongPositionMs(now);
    // Visualisation: hybrid bass-driven ribbon.  Past hearts = real detected kicks
    // (density tracks the music); future hearts = extrapolated from recent bass tempo
    // (gives the Guitar-Hero approach phase).  Falls back to chart grid before any
    // bass is detected so the screen never goes empty.
    uint32_t scrollPeriodMs = rhythmLaneScrollPeriodMs();
    rhythmBuildDisplayBeats(rel);
    int livePct = s_playPaused ? -1 : rhythmGameLiveScorePct(rel);
    float recoverGlitch = 0.f;
    if (s_meltdownRecoverActive && s_meltdownRecoverDurMs > 0u) {
      uint32_t recoverAge = now - s_meltdownRecoverStartMs;
      if (recoverAge < s_meltdownRecoverDurMs)
        recoverGlitch = 1.f - (float)recoverAge / (float)s_meltdownRecoverDurMs;
    }
    lcdRetroPlaying(s_songs[s_playSongIdx].title, el, s_songDurationMs, scrollPeriodMs, now, rel,
                    s_displayBeatsBuf, s_nDisplayBeats,
                    s_lastHitWallMs, s_lastMissWallMs, s_consumed, s_nConsumed,
                    s_playPaused, livePct, recoverGlitch);
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

#ifndef RHYTHM_MP3_H
#define RHYTHM_MP3_H

#include <Arduino.h>
#include <stdint.h>

#define RHYTHM_MAX_SCAN_TRACKS 64
// Full VFS path (e.g. /rhythm/name.mp3). 80 was too small — truncation dropped .mp3 and hid files silently.
#define RHYTHM_PATH_MAX 128
#define RHYTHM_TITLE_MAX 48

// One entry from scanning /rhythm/*.mp3 on SD only (menu list).
typedef struct {
  char path[RHYTHM_PATH_MAX];
  char title[RHYTHM_TITLE_MAX];
  uint32_t approxDurationMs; // heuristic from file size @ nominal kbps (LCD + scoring grid hint)
  uint16_t bpm;             // ID3 / filename, overridden by optional sidecar (same stem: track.rhy)
  uint16_t beatOffsetMs;    // first grid beat at this ms; from sidecar or 0
  uint8_t difficulty;       // 0 = auto (filename d# / BPM heuristic); 1–9 from sidecar overrides
} RhythmTrackInfo;

bool rhythmMp3FsInit();
// Call before SPI.end() when SD shares the bus (buttons.cpp). Flushes unmount so the next SD.begin is clean.
void rhythmSdUnmountIfMounted();
// True after SD mount succeeds (scan / MP3). MISO GPIO cannot double as a button while SD is in use.
bool rhythmMp3SdMounted();
void rhythmMp3Stop();

// Returns true if playback started (I2S handed to MP3 stack). Path is SD path e.g. /rhythm/track.mp3.
bool rhythmMp3TryPlay(const char *sdPath);

// Internal kick-grid writer (not used in rhythm game — rhythm mode is MP3-only).
void rhythmSynthStart(uint16_t bpm, uint16_t beatOffsetMs, uint32_t durationMs);
void rhythmSynthStop();

bool rhythmStreamIsActive();
bool rhythmStreamIsPaused();
void rhythmStreamSetPaused(bool paused);
void rhythmStreamLoop();
// 1.f = full output; multiply MP3/synth PCM (e.g. AFK exit fade). Reset to 1 on stop / new playback.
void rhythmStreamSetFadeMul(float linear01);

// Elapsed play position in ms (for UI). Stops updating when not active.
uint32_t rhythmStreamElapsedMs();
// Sample-accurate song ms — matches bass-onset timestamps from the beat tracker.
uint32_t rhythmStreamAudioPositionMs(void);

// BPM from last opened MP3 (ID3 TBPM / filename) or synth start; 0 if unknown. Used to refresh chart vs scan default.
uint16_t rhythmStreamSourceBpm(void);

// Low-band kick onset times (ms from song start) — primary beats for scoring / LED feedback.
void rhythmStreamGetBassOnsets(const uint32_t **outPtr, int *outCount);

// Total number of oldest bass onsets that have been evicted from the ring since
// playback began. Use to keep "last judged onset" indices valid after sliding.
uint32_t rhythmStreamBassOnsetsDropped(void);

// Mid-band onset times for bonus scoring — filled during MP3 or synth.
void rhythmStreamGetBonusOnsets(const uint32_t **outPtr, int *outCount);

// Audio-reactive levels for LEDs (not used for scoring). Bass ≈ low-band flux / kicks; mid ≈ repetitive transients.
// Pulses decay with sample-accurate song time; levels are smoothed envelope. No-op when stream inactive.
void rhythmStreamGetMusicVis(float *bassPulse, float *midPulse, float *bassLevel, float *midLevel);
// Slower-decay bass/kick envelope for front 16/46 LEDs (0..1); holds through the visible part of each hit.
void rhythmStreamGetBassLedEnvelope(float *envelope01);

// True after decoded audio has fired at least one strong bass onset (kick). False during quiet intros.
bool rhythmStreamHasStrongBassOnsetYet(void);

// Scan /rhythm for .mp3 files on SD only. Sorted by title.
int rhythmScanTrackFolder(RhythmTrackInfo *out, int maxOut);

// Basename tag: ..._d3_... or ...-d9....mp3 → 1–9; 0 = not tagged (use BPM/duration heuristic in game).
uint8_t rhythmDifficultyFromFilename(const char *path);

#endif

#include "rhythm_mp3.h"
#include "audio.h"
#include "buttons.h"
#include "config.h"
#include "rhythm_sd_pins.h"
#include <SD.h>
#include <SPI.h>
#include <ctype.h>
#include <driver/i2s.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#endif
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("AudioGeneratorMP3.h")
#define RHYTHM_HAVE_MP3 1
#include "AudioFileSource.h"
#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#else
#define RHYTHM_HAVE_MP3 0
#endif

// --- Onset tracker (bass flux ≈ "kick band"; mid flux for bonus) ------------
// Buffer holds enough onsets for ~17 min @ 1 kick/sec or ~4 min @ 4 kicks/sec.
// When full we slide the window (drop oldest) so a long/dense song never silently
// stops generating onsets — see lcd hearts and scoring rely on a non-empty list.
static constexpr int kMaxOnsets = 1024;
static uint32_t s_bassOnsetMs[kMaxOnsets];
static int s_bassOnsetCount = 0;
static uint32_t s_midBonusMs[kMaxOnsets];
static int s_midBonusCount = 0;
// Counts of onsets that have rolled off the front of each ring (for external
// index trackers that need to compensate when entries get evicted).
static uint32_t s_bassOnsetDropped = 0;
static uint32_t s_midBonusDropped = 0;
static uint64_t s_monoFrames = 0;
static uint32_t s_playStartMs = 0;
static uint32_t s_streamSr = SR;

static float s_lpBass = 0.f, s_lpMid = 0.f;
static float s_prevBassE = 0.f, s_prevMidE = 0.f;
static float s_fluxEma = 0.f, s_fluxVar = 0.f;
static uint32_t s_lastBassOnsetMs = 0;
static uint32_t s_lastMidOnsetMs = 0;
// LED music viz (decoupled from scoring)
static float s_visBassMagSm = 0.f;
static float s_visMidMagSm = 0.f;
static float s_lastBassHitStr = 1.f;
static float s_lastMidHitStr = 0.9f;
static float s_outLevelSm = 0.f;
static float s_audibleGate01 = 1.f;
static float s_streamFadeMul = 1.f;

static void beatTrackerReset() {
  s_bassOnsetCount = 0;
  s_midBonusCount = 0;
  s_bassOnsetDropped = 0;
  s_midBonusDropped = 0;
  s_monoFrames = 0;
  s_lpBass = s_lpMid = 0.f;
  s_prevBassE = s_prevMidE = 0.f;
  s_fluxEma = s_fluxVar = 0.f;
  s_lastBassOnsetMs = s_lastMidOnsetMs = 0;
  s_visBassMagSm = s_visMidMagSm = 0.f;
  s_lastBassHitStr = 1.f;
  s_lastMidHitStr = 0.9f;
  s_outLevelSm = 0.f;
  s_audibleGate01 = 1.f;
}

static void beatTrackerFeedFrame(int16_t mono) {
  float x = mono * (1.f / 32768.f);
  // Pure sub-bass kick detection — no mid-band bleed so snares/hats/synths
  // don't fire false onsets.  Cutoff ~630 Hz (coeff 0.09).
  s_lpBass = s_lpBass * 0.91f + x * 0.09f;
  s_lpMid  = s_lpMid  * 0.82f + (x - s_lpBass) * 0.18f;
  float kickBody = s_lpBass;   // sub-bass only; mid-band removed to avoid over-firing
  float eb = kickBody * kickBody;
  float em = s_lpMid * s_lpMid;
  float fluxB = eb - s_prevBassE;
  float fluxM = em - s_prevMidE;
  s_prevBassE = eb;
  s_prevMidE = em;

  s_fluxEma = s_fluxEma * 0.995f + fabsf(fluxB) * 0.005f;
  s_fluxVar = s_fluxVar * 0.997f + (fabsf(fluxB) - s_fluxEma) * (fabsf(fluxB) - s_fluxEma) * 0.003f;
  // Raised to 2.2σ (was 1.65) — stricter threshold, only strong kicks pass.
  float thr = s_fluxEma + 2.2f * sqrtf(fmaxf(s_fluxVar, 1e-12f));

  float rmsB = sqrtf(fmaxf(eb, 1e-12f));
  s_outLevelSm = s_outLevelSm * 0.985f + rmsB * 0.015f;
  float level01 = fminf(1.f, s_outLevelSm / 0.048f);
  s_audibleGate01 = s_streamFadeMul * level01;
  // During fade-out / quiet tails: raise onset threshold sharply so weak tails don't spawn beats.
  float detectScale = 1.f + (1.f - s_audibleGate01) * 5.5f;
  thr *= detectScale;

  uint32_t tms = (uint32_t)(s_monoFrames * 1000ull / (uint64_t)s_streamSr);
  s_monoFrames++;

  if ((s_monoFrames & 127u) == 0u) {
    float bm = sqrtf(fmaxf(eb, 1e-15f));
    float mm = sqrtf(fmaxf(em, 1e-15f));
    s_visBassMagSm = s_visBassMagSm * 0.62f + bm * 0.38f;
    s_visMidMagSm = s_visMidMagSm * 0.58f + mm * 0.42f;
  }

  // 220 ms refractory = max ~4.5 kicks/sec; was 105 ms which allowed 9/sec and
  // caused screen-flooding on busy mid-range content.
  if (s_audibleGate01 >= 0.32f && fluxB > thr && (tms - s_lastBassOnsetMs) > 220) {
    float ex = (fluxB - thr) / fmaxf(thr * 0.38f, 1e-5f);
    if (ex > 1.35f)
      ex = 1.35f;
    s_lastBassHitStr = 0.42f + 0.58f * (ex / 1.35f);
    s_lastBassOnsetMs = tms;
    if (s_bassOnsetCount < kMaxOnsets) {
      s_bassOnsetMs[s_bassOnsetCount++] = tms;
    } else {
      // Buffer full: slide window left, drop oldest, append newest. Keeps the
      // most recent kMaxOnsets entries so display + scoring keep working on
      // long/dense songs.
      memmove(s_bassOnsetMs, s_bassOnsetMs + 1, (kMaxOnsets - 1) * sizeof(s_bassOnsetMs[0]));
      s_bassOnsetMs[kMaxOnsets - 1] = tms;
      s_bassOnsetDropped++;
    }
  }
  float mthr = s_fluxEma * 0.92f + 2.1f * sqrtf(fmaxf(s_fluxVar, 1e-12f));
  mthr *= detectScale;
  if (s_audibleGate01 >= 0.32f && fluxM > mthr && (tms - s_lastMidOnsetMs) > 110) {
    float mx = (fluxM - mthr) / fmaxf(mthr * 0.48f, 1e-5f);
    if (mx > 1.4f)
      mx = 1.4f;
    s_lastMidHitStr = 0.35f + 0.65f * (mx / 1.4f);
    s_lastMidOnsetMs = tms;
    if (s_midBonusCount < kMaxOnsets) {
      s_midBonusMs[s_midBonusCount++] = tms;
    } else {
      memmove(s_midBonusMs, s_midBonusMs + 1, (kMaxOnsets - 1) * sizeof(s_midBonusMs[0]));
      s_midBonusMs[kMaxOnsets - 1] = tms;
      s_midBonusDropped++;
    }
  }
}

void rhythmStreamGetBassOnsets(const uint32_t **outPtr, int *outCount) {
  if (outPtr)
    *outPtr = s_bassOnsetMs;
  if (outCount)
    *outCount = s_bassOnsetCount;
}

uint32_t rhythmStreamBassOnsetsDropped(void) { return s_bassOnsetDropped; }

void rhythmStreamGetBonusOnsets(const uint32_t **outPtr, int *outCount) {
  if (outPtr)
    *outPtr = s_midBonusMs;
  if (outCount)
    *outCount = s_midBonusCount;
}

void rhythmStreamSetFadeMul(float linear01) {
  if (linear01 < 0.f)
    linear01 = 0.f;
  else if (linear01 > 1.f)
    linear01 = 1.f;
  s_streamFadeMul = linear01;
}

// --- MP3 path (ESP8266Audio) -------------------------------------------------
#if RHYTHM_HAVE_MP3
static AudioFileSource *s_file = nullptr;     // Buffer (read-ahead) or raw SD
static AudioFileSourceSD *s_fileSd = nullptr; // Underlying SD file when s_file is a buffer
static AudioGeneratorMP3 *s_mp3 = nullptr;

// RAM read-ahead hides SD latency spikes / main-loop jitter from the MP3 bitstream reader.
// 64 KiB stressed heap on some tracks; 32 KiB is usually enough and reduces malloc stalls.
static constexpr uint32_t kMp3SdReadAheadBytes = 32 * 1024;

static int16_t rhythmMp3ClampS16(int32_t v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (int16_t)v;
}

class TappedAudioOutput : public AudioOutputI2S {
public:
  // Match audioInit DMA depth; APLL off — APLL on ESP32-S3 often causes crackly/static I2S.
  TappedAudioOutput() : AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 16, 0) {}

  bool SetRate(int hz) override {
    if (hz >= 8000 && hz <= 48000)
      s_streamSr = (uint32_t)hz;
    return AudioOutputI2S::SetRate(hz);
  }

  bool ConsumeSample(int16_t sample[2]) override {
    float g = s_streamFadeMul;
    int16_t adj[2] = {rhythmMp3ClampS16((int32_t)((float)sample[0] * g)),
                      rhythmMp3ClampS16((int32_t)((float)sample[1] * g))};
    int32_t m = ((int32_t)adj[0] + (int32_t)adj[1]) >> 1;
    beatTrackerFeedFrame((int16_t)m);
    return AudioOutputI2S::ConsumeSample(adj);
  }
};

static TappedAudioOutput *s_out = nullptr;
#endif

enum StreamMode : uint8_t { SM_NONE, SM_MP3, SM_SYNTH };
static StreamMode s_mode = SM_NONE;
static uint16_t s_streamSourceBpm = 0;
static bool s_streamPaused = false;
static uint32_t s_pauseStartedMs = 0;
static uint32_t s_pauseAccumMs = 0;

float rhythmStreamGetFadeMul(void) { return s_streamFadeMul; }

float rhythmStreamAudibleGate(void) { return s_audibleGate01; }

bool rhythmStreamLaneActive(void) { return s_mode != SM_NONE && s_audibleGate01 >= 0.30f; }

void rhythmStreamGetMusicVis(float *bassPulse, float *midPulse, float *bassLevel, float *midLevel) {
  float bp = 0.f, mp = 0.f, bl = 0.f, ml = 0.f;
  if (s_mode != SM_NONE) {
    uint32_t t = (uint32_t)(s_monoFrames * 1000ull / (uint64_t)s_streamSr);
    if (s_lastBassOnsetMs > 0 && t >= s_lastBassOnsetMs)
      bp = s_lastBassHitStr * expf(-(float)(t - s_lastBassOnsetMs) / 88.f);
    if (s_lastMidOnsetMs > 0 && t >= s_lastMidOnsetMs)
      mp = s_lastMidHitStr * expf(-(float)(t - s_lastMidOnsetMs) / 62.f);
    bl = fminf(1.f, s_visBassMagSm * 6.8f);
    ml = fminf(1.f, s_visMidMagSm * 7.5f);
  }
  if (bassPulse)
    *bassPulse = bp;
  if (midPulse)
    *midPulse = mp;
  if (bassLevel)
    *bassLevel = bl;
  if (midLevel)
    *midLevel = ml;
}

void rhythmStreamGetBassLedEnvelope(float *envelope01) {
  float e = 0.f;
  if (s_mode != SM_NONE && s_lastBassOnsetMs > 0) {
    uint32_t t = (uint32_t)(s_monoFrames * 1000ull / (uint64_t)s_streamSr);
    uint32_t since = (t >= s_lastBassOnsetMs) ? (t - s_lastBassOnsetMs) : 0;
    float bp = s_lastBassHitStr * expf(-(float)since / 70.f);
    const uint32_t kHoldMs = 240;
    float hold = 0.f;
    if (since < kHoldMs) {
      float x = 1.f - (float)since / (float)kHoldMs;
      hold = 0.62f + 0.38f * x * x;
    }
    e = fmaxf(bp, hold);
    if (e > 1.f)
      e = 1.f;
  }
  if (envelope01)
    *envelope01 = e;
}

// --- Synthetic “melodic guide” (soft thump + short pentatonic tone, all sines) ---
static uint16_t s_synBpm = 120;
static uint16_t s_synOffMs = 0;
static uint32_t s_synDurMs = 0;
static uint32_t s_synStartWallMs = 0;
static uint32_t s_synSampleIdx = 0;
static float s_kickTheta = 0.f;
static uint32_t s_kickRemain = 0;
static uint32_t s_kickLenSamp = 1;
static float s_melTheta = 0.f;
static uint32_t s_melRemain = 0;
static uint32_t s_melLenSamp = 1;
static float s_melFreqHz = 196.f;
static uint8_t s_melStep = 0;

// C-major pentatonic around a pleasant mid range (not harsh, not sub-only)
static const float kPentHz[5] = {196.00f, 220.00f, 261.63f, 293.66f, 329.63f}; // G3..E4

static void synthTriggerBeat() {
  s_kickTheta = 0.f;
  s_kickLenSamp = (uint32_t)(SR * 0.032f);
  s_kickRemain = s_kickLenSamp;
  s_melTheta = 0.f;
  s_melLenSamp = (uint32_t)(SR * 0.20f);
  s_melRemain = s_melLenSamp;
  s_melFreqHz = kPentHz[s_melStep % 5];
  s_melStep++;
}

static void rhythmSynthWriteBlock() {
  if (!i2s_initialized)
    return;
  int16_t buf[BUF_SAMPLES * 2];
  memset(buf, 0, sizeof(buf));
  float periodSec = 60.f / (float)s_synBpm;
  uint32_t periodSamp = (uint32_t)(periodSec * (float)SR + 0.5f);
  uint32_t offSamp = (uint32_t)((float)s_synOffMs * 0.001f * (float)SR + 0.5f);

  const float TAU = 6.28318530718f;

  for (size_t i = 0; i < BUF_SAMPLES; i++) {
    uint32_t si = s_synSampleIdx + (uint32_t)i;
    if (periodSamp > 0) {
      int64_t rel = (int64_t)si - (int64_t)offSamp;
      if (rel >= 0 && (rel % periodSamp) == 0)
        synthTriggerBeat();
    }
    float v = 0.f;
    if (s_kickRemain > 0) {
      float t = 1.f - (float)s_kickRemain / (float)s_kickLenSamp;
      float env = expf(-7.0f * t);
      s_kickTheta += TAU * 52.f / (float)SR;
      v += sinf(s_kickTheta) * 0.20f * env;
      s_kickRemain--;
    }
    if (s_melRemain > 0) {
      float t = 1.f - (float)s_melRemain / (float)s_melLenSamp;
      float env = expf(-2.8f * t);
      s_melTheta += TAU * s_melFreqHz / (float)SR;
      float m = sinf(s_melTheta) + 0.15f * sinf(2.f * s_melTheta);
      v += m * 0.17f * env;
      s_melRemain--;
    }
    int16_t s = (int16_t)fmaxf(-30000.f, fminf(30000.f, v * 28000.f * s_streamFadeMul));
    buf[2 * i + 0] = s;
    buf[2 * i + 1] = s;
    beatTrackerFeedFrame(s);
  }
  s_synSampleIdx += BUF_SAMPLES;

  size_t w = 0;
  i2s_write(I2S_NUM_0, buf, sizeof(buf), &w, pdMS_TO_TICKS(200));
}

static bool s_sd_mounted = false;

// forceRemount: SD.end + begin — needed for reliable /rhythm scan after SPI was shared; avoid for every MP3 open.
static bool rhythmTryMountSd(bool forceRemount) {
#if !RHYTHM_HAVE_MP3 || !RHYTHM_ENABLE_SD || (RHYTHM_SD_CS_PIN < 0)
  return false;
#else
  SPI.begin(RHYTHM_SD_SCK_PIN, RHYTHM_SD_MISO_PIN, RHYTHM_SD_MOSI_PIN, RHYTHM_SD_CS_PIN);
  yield();
  if (forceRemount && s_sd_mounted) {
    SD.end();
    s_sd_mounted = false;
    delayMicroseconds(500);
  }
  if (!SD.begin(RHYTHM_SD_CS_PIN, SPI, RHYTHM_SD_SPI_HZ)) {
    if (s_sd_mounted) {
      SD.end();
      s_sd_mounted = false;
      delayMicroseconds(2000);
    }
    if (!SD.begin(RHYTHM_SD_CS_PIN, SPI, RHYTHM_SD_SPI_HZ)) {
      Serial.println("[RHYTHM] SD not found or mount failed (FAT32, use Elite pins 10,9,11,12)");
      return false;
    }
  }
  if (!s_sd_mounted)
    Serial.printf("[RHYTHM] SD OK (SCK=%d MISO=%d MOSI=%d CS=%d)\n", RHYTHM_SD_SCK_PIN, RHYTHM_SD_MISO_PIN,
                  RHYTHM_SD_MOSI_PIN, RHYTHM_SD_CS_PIN);
  s_sd_mounted = true;
  return true;
#endif
}

bool rhythmMp3FsInit() {
#if defined(ARDUINO_ARCH_ESP32)
  return rhythmTryMountSd(true);
#else
  return false;
#endif
}

bool rhythmMp3SdMounted() {
#if RHYTHM_HAVE_MP3 && RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
  return s_sd_mounted;
#else
  return false;
#endif
}

void rhythmSdUnmountIfMounted() {
#if !RHYTHM_HAVE_MP3 || !RHYTHM_ENABLE_SD || (RHYTHM_SD_CS_PIN < 0)
  return;
#else
  if (s_mode == SM_MP3)
    return;
  if (!s_sd_mounted)
    return;
  SPI.begin(RHYTHM_SD_SCK_PIN, RHYTHM_SD_MISO_PIN, RHYTHM_SD_MOSI_PIN, RHYTHM_SD_CS_PIN);
  yield();
  SD.end();
  s_sd_mounted = false;
  delayMicroseconds(500);
#endif
}

static void uninstallIdfI2s() {
  if (i2s_initialized) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_initialized = false;
    delay(80);
  }
}

#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
static uint16_t rhythmProbeId3Bpm(File &f);
static uint16_t bpm_from_filename(const char *path);
#endif

uint16_t rhythmStreamSourceBpm(void) { return s_streamSourceBpm; }

void rhythmMp3Stop() {
  StreamMode prev = s_mode;
#if RHYTHM_HAVE_MP3
  if (s_mp3) {
    s_mp3->stop();
    delete s_mp3;
    s_mp3 = nullptr;
  }
  if (s_file) {
    delete s_file;
    s_file = nullptr;
  }
  if (s_fileSd) {
    delete s_fileSd;
    s_fileSd = nullptr;
  }
  if (s_out) {
    s_out->stop();
    delete s_out;
    s_out = nullptr;
  }
#endif
  s_mode = SM_NONE;
  s_streamFadeMul = 1.f;
  s_streamSourceBpm = 0;
  s_streamPaused = false;
  s_pauseStartedMs = 0;
  s_pauseAccumMs = 0;
  if (prev == SM_MP3)
    audioInit();
  beatTrackerReset();
  buttonsRestoreInputPullups();
}

bool rhythmMp3TryPlay(const char *path) {
#if !RHYTHM_HAVE_MP3
  (void)path;
  return false;
#else
  if (!path || !path[0])
    return false;
  rhythmMp3Stop();
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
  yield();
  // Always remount: MENU/GET_READY run buttonsRefreshSdSharedPins() every loop (SPI.end) without a matching SD.end,
  // so s_sd_mounted can lie and light mount skips SD.end → exists/open fail → synth fallback.
  if (!rhythmTryMountSd(true)) {
    Serial.println("[RHYTHM] SD mount failed — cannot open MP3");
    buttonsRestoreInputPullups();
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
  yield();

  // BPM from filename only here — avoids a second SD open before decode (was correlated with bad/static playback).
  uint16_t probedChartBpm = 0;
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
  probedChartBpm = bpm_from_filename(path);
#endif

  AudioFileSourceSD *sdSrc = nullptr;
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
  if (s_sd_mounted && SD.exists(path)) {
    sdSrc = new AudioFileSourceSD(path);
    if (sdSrc && !sdSrc->isOpen()) {
      delete sdSrc;
      sdSrc = nullptr;
    }
  }
#endif
  if (!sdSrc) {
    Serial.printf("[RHYTHM] MP3 not on SD: %s\n", path);
    buttonsRestoreInputPullups();
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
  yield();

  AudioFileSource *src = new AudioFileSourceBuffer(sdSrc, kMp3SdReadAheadBytes);
  if (!src) {
    delete sdSrc;
    Serial.println("[RHYTHM] MP3 read-ahead buffer alloc failed");
    buttonsRestoreInputPullups();
    return false;
  }
  s_fileSd = sdSrc;

  uninstallIdfI2s();
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
  yield();

  s_file = src;
  s_out = new TappedAudioOutput();
  s_out->SetPinout(I2S_BCLK, I2S_LRCK, I2S_DATA);
  s_out->SetRate(44100);
  s_streamSr = 44100;
  s_out->SetChannels(2);
  s_out->SetGain(1.0f);
  if (!s_out->begin()) {
    delete s_out;
    s_out = nullptr;
    delete s_file;
    s_file = nullptr;
    delete s_fileSd;
    s_fileSd = nullptr;
    audioInit();
    buttonsRestoreInputPullups();
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
  yield();
  Serial.printf("[RHYTHM] MP3 decoder starting: %s\n", path);
  s_mp3 = new AudioGeneratorMP3();
  if (!s_mp3->begin(s_file, s_out)) {
    Serial.printf("[RHYTHM] MP3 decoder begin() failed: %s (re-encode as 44.1kHz stereo CBR/VBR mp3)\n", path);
    rhythmMp3Stop();
    return false;
  }
  s_playStartMs = millis();
  beatTrackerReset();
  s_monoFrames = 0;
  s_mode = SM_MP3;
  s_streamSourceBpm = probedChartBpm;
  if (s_streamSourceBpm)
    Serial.printf("[RHYTHM] MP3 playing: %s (BPM %u)\n", path, (unsigned)s_streamSourceBpm);
  else
    Serial.printf("[RHYTHM] MP3 playing: %s\n", path);
  return true;
#endif
}

void rhythmSynthStart(uint16_t bpm, uint16_t beatOffsetMs, uint32_t durationMs) {
  rhythmMp3Stop();
  if (!i2s_initialized)
    audioInit();
  s_streamSr = SR;
  s_synBpm = bpm ? bpm : 120;
  s_streamSourceBpm = s_synBpm;
  s_synOffMs = beatOffsetMs;
  s_synDurMs = durationMs;
  s_synStartWallMs = millis();
  s_synSampleIdx = 0;
  s_kickRemain = 0;
  s_playStartMs = s_synStartWallMs;
  beatTrackerReset();
  s_monoFrames = 0;
  s_mode = SM_SYNTH;
}

void rhythmSynthStop() {
  if (s_mode == SM_SYNTH)
    s_mode = SM_NONE;
  beatTrackerReset();
}

bool rhythmStreamIsActive() { return s_mode != SM_NONE; }

bool rhythmStreamIsPaused() { return s_streamPaused && s_mode != SM_NONE; }

void rhythmStreamSetPaused(bool paused) {
  if (s_mode == SM_NONE)
    return;
  if (paused && !s_streamPaused) {
    s_pauseStartedMs = millis();
    s_streamPaused = true;
  } else if (!paused && s_streamPaused) {
    if (s_pauseStartedMs)
      s_pauseAccumMs += millis() - s_pauseStartedMs;
    s_pauseStartedMs = 0;
    s_streamPaused = false;
  }
}

bool rhythmStreamHasStrongBassOnsetYet(void) {
  return s_mode != SM_NONE && s_lastBassOnsetMs > 0;
}

void rhythmStreamLoop() {
  if (s_streamPaused)
    return;
  if (s_mode == SM_MP3) {
#if RHYTHM_HAVE_MP3
    // SPI is left initialized for SD during MP3 (main skips shared-pin SPI.end); no SPI.begin here.
    // isRunning() can be false for the first few ms after begin(); tearing down here made the game
    // think the song ended instantly (results / emojis / next-track) with no audible playback.
    const uint32_t mp3AgeMs = millis() - s_playStartMs;
    constexpr uint32_t kMp3StartGraceMs = 1500;
    // More loop() calls per pump — keep PCM fed when LCD / lane logic blocks the main loop.
    constexpr int kMp3LoopsPerPump = 28;
    if (s_mp3 && s_mp3->isRunning()) {
      for (int k = 0; k < kMp3LoopsPerPump && s_mp3->isRunning(); k++) {
        if (!s_mp3->loop()) {
          Serial.println("[RHYTHM] MP3 finished");
          rhythmMp3Stop();
          break;
        }
      }
    } else if (s_mp3 && mp3AgeMs < kMp3StartGraceMs) {
      for (int k = 0; k < kMp3LoopsPerPump && s_mp3; k++) {
        if (!s_mp3->loop())
          break;
      }
    } else if (s_mp3) {
      Serial.printf("[RHYTHM] MP3 decoder stopped early (age=%lu ms)\n", (unsigned long)mp3AgeMs);
      rhythmMp3Stop();
    }
#endif
  } else if (s_mode == SM_SYNTH) {
    // duration 0 must not end immediately (millis() - start >= 0 is always true). Let rhythm_game
    // time out via s_songDurationMs + margin, or stop() when leaving PLAYING.
    if (s_synDurMs > 0 && rhythmStreamElapsedMs() >= s_synDurMs) {
      Serial.println("[RHYTHM] Synth song finished");
      rhythmSynthStop();
      return;
    }
    for (int k = 0; k < 3; k++)
      rhythmSynthWriteBlock();
  }
}

uint32_t rhythmStreamElapsedMs() {
  if (s_mode == SM_NONE)
    return 0;
  uint32_t el = (s_mode == SM_SYNTH) ? (millis() - s_synStartWallMs) : (millis() - s_playStartMs);
  if (s_pauseAccumMs >= el)
    return 0;
  el -= s_pauseAccumMs;
  if (s_streamPaused && s_pauseStartedMs) {
    uint32_t cur = millis() - s_pauseStartedMs;
    if (cur >= el)
      return 0;
    return el - cur;
  }
  return el;
}

uint32_t rhythmStreamAudioPositionMs(void) {
  if (s_mode == SM_NONE)
    return 0;
  if (s_streamSr < 8000u)
    return rhythmStreamElapsedMs();
  return (uint32_t)(s_monoFrames * 1000ull / (uint64_t)s_streamSr);
}

#if defined(ARDUINO_ARCH_ESP32)

static const char kRhythmSubdir[] = "/rhythm";

static int ends_mp3_name(const char *name) {
  size_t n = strlen(name);
  if (n < 4)
    return 0;
  const char *e = name + n - 4;
  return (e[0] == '.' && (e[1] == 'm' || e[1] == 'M') && (e[2] == 'p' || e[2] == 'P') && (e[3] == '3'));
}

static void title_from_path(const char *path, char *title, size_t cap) {
  const char *s = strrchr(path, '/');
  s = s ? s + 1 : path;
  strncpy(title, s, cap - 1);
  title[cap - 1] = 0;
  size_t tl = strlen(title);
  if (tl > 4 && ends_mp3_name(title))
    title[tl - 4] = 0;
  for (char *p = title; *p; p++) {
    if (*p == '_' || *p == '-')
      *p = ' ';
  }
}

static uint32_t estimate_ms_from_size(size_t sz) {
  if (sz < 2048)
    return 45000;
  // Duration ≈ file_bits / bitrate. Bitrate must be in bits/s (128 kbps => 128000), not kbps —
  // dividing by 128 alone inflated estimates ~1000× and clamped almost everything to 15:00.
  const uint64_t bits = (uint64_t)sz * 8;
  const unsigned kbps = 192; // closer to typical store/rip defaults than 128; still heuristic for VBR
  const uint64_t bps = (uint64_t)kbps * 1000ull;
  uint64_t ms64 = bits * 1000ull / bps;
  if (ms64 > (uint64_t)UINT32_MAX)
    ms64 = UINT32_MAX;
  uint32_t ms = (uint32_t)ms64;
  if (ms < 30000u)
    ms = 30000u;
  if (ms > 10800000u) // 3 h ceiling (long mixes); avoids absurd values if size is wrong
    ms = 10800000u;
  return ms;
}

static void basename_key(const char *path, char *key, size_t kcap) {
  const char *s = strrchr(path, '/');
  s = s ? s + 1 : path;
  strncpy(key, s, kcap - 1);
  key[kcap - 1] = 0;
  for (size_t i = 0; key[i]; i++)
    key[i] = (char)tolower((unsigned char)key[i]);
}

static int basename_in_existing(const char *path, const RhythmTrackInfo *arr, int n) {
  char k1[RHYTHM_PATH_MAX];
  basename_key(path, k1, sizeof k1);
  if (!k1[0])
    return 0;
  for (int i = 0; i < n; i++) {
    char k2[RHYTHM_PATH_MAX];
    basename_key(arr[i].path, k2, sizeof k2);
    if (strcmp(k1, k2) == 0)
      return 1;
  }
  return 0;
}

uint8_t rhythmDifficultyFromFilename(const char *path) {
  if (!path || !path[0])
    return 0;
#if !RHYTHM_ENABLE_SD || (RHYTHM_SD_CS_PIN < 0)
  return 0;
#else
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char s[RHYTHM_PATH_MAX];
  size_t k;
  for (k = 0; k + 1 < sizeof(s) && base[k]; k++)
    s[k] = (char)tolower((unsigned char)base[k]);
  s[k] = 0;

  for (const char *p = s; *p; p++) {
    if (p[0] != 'd')
      continue;
    if (p[1] < '1' || p[1] > '9')
      continue;
    if (p > s && isalnum((unsigned char)p[-1]))
      continue;
    if (p[2] && isdigit((unsigned char)p[2]))
      continue;
    if (p[2] && isalpha((unsigned char)p[2]))
      continue;
    return (uint8_t)(p[1] - '0');
  }
  return 0;
#endif
}

#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
// --- ID3v2 TBPM / TBP + filename hint (scan-time only) ---------------------
static uint32_t id3_read_u32be(const uint8_t *b) {
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

static uint32_t id3_synchsafe_u32(const uint8_t *b) {
  return (uint32_t)b[0] << 21 | (uint32_t)b[1] << 14 | (uint32_t)b[2] << 7 | (uint32_t)b[3];
}

static uint16_t id3_parse_tbpm_bytes(const uint8_t *data, size_t len) {
  if (len < 2)
    return 0;
  uint8_t enc = data[0];
  const uint8_t *p = data + 1;
  len -= 1;
  char buf[48];
  size_t bi = 0;
  if (enc == 0 || enc == 3) {
    if (len >= sizeof(buf))
      len = sizeof(buf) - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
  } else if (enc == 1 && len >= 4) {
    size_t i = 0;
    if (p[0] == 0xFF && p[1] == 0xFE)
      i = 2;
    for (; i + 1 < len && bi + 1 < sizeof(buf); i += 2) {
      uint16_t ch = (uint16_t)p[i] | ((uint16_t)p[i + 1] << 8);
      if (ch == 0)
        break;
      if (ch >= 32 && ch < 127)
        buf[bi++] = (char)ch;
    }
    buf[bi] = 0;
  } else if (enc == 2 && len >= 2) {
    for (size_t i = 0; i + 1 < len && bi + 1 < sizeof(buf); i += 2) {
      uint16_t ch = ((uint16_t)p[i] << 8) | p[i + 1];
      if (ch == 0)
        break;
      if (ch >= 32 && ch < 127)
        buf[bi++] = (char)ch;
    }
    buf[bi] = 0;
  } else
    return 0;

  char *trim = buf;
  while (*trim == ' ' || *trim == '\t' || *trim == '\r' || *trim == '\n')
    trim++;
  size_t L = strlen(trim);
  while (L > 0 && (trim[L - 1] == ' ' || trim[L - 1] == '\t' || trim[L - 1] == '\r' || trim[L - 1] == '\n')) {
    trim[L - 1] = 0;
    L--;
  }
  char *endp = NULL;
  float f = strtof(trim, &endp);
  (void)endp;
  if (f >= 40.f && f <= 320.f)
    return (uint16_t)(f + 0.5f);
  return 0;
}

static uint16_t bpm_from_filename(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char s[RHYTHM_PATH_MAX];
  size_t k;
  for (k = 0; k + 1 < sizeof(s) && base[k]; k++)
    s[k] = (char)tolower((unsigned char)base[k]);
  s[k] = 0;

  uint16_t best = 0;
  for (const char *p = s; *p; p++) {
    if (p[0] == 'b' && p[1] == 'p' && p[2] == 'm') {
      const char *after = p + 3;
      while (*after == ' ' || *after == '_' || *after == '-' || *after == '.')
        after++;
      if (isdigit((unsigned char)*after)) {
        unsigned v = (unsigned)strtoul(after, NULL, 10);
        if (v >= 40u && v <= 300u && v > best)
          best = (uint16_t)v;
      }
      const char *r = p;
      while (r > s && (r[-1] == ' ' || r[-1] == '_' || r[-1] == '-' || r[-1] == '.'))
        r--;
      const char *end = r;
      while (r > s && isdigit((unsigned char)r[-1]))
        r--;
      size_t nd = (size_t)(end - r);
      if (nd >= 2 && nd <= 3) {
        char tmp[5];
        memcpy(tmp, r, nd);
        tmp[nd] = 0;
        unsigned v = (unsigned)strtoul(tmp, NULL, 10);
        if (v >= 40u && v <= 300u && v > best)
          best = (uint16_t)v;
      }
    }
  }
  return best;
}

// Reads start of file; returns 0 if no usable TBPM/TBP.
static uint16_t rhythmProbeId3Bpm(File &f) {
  if (!f || f.isDirectory())
    return 0;
  f.seek(0);
  uint8_t hdr[10];
  if (f.read(hdr, 10) != 10)
    return 0;
  if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3')
    return 0;
  uint8_t ver_maj = hdr[3];
  uint8_t flags = hdr[5];
  uint32_t tagSize = id3_synchsafe_u32(hdr + 6);
  if (tagSize == 0 || tagSize > 512u * 1024u)
    return 0;

  size_t cap = (size_t)tagSize;
  const size_t kMaxRead = 24u * 1024u;
  if (cap > kMaxRead)
    cap = kMaxRead;
  uint8_t *body = (uint8_t *)malloc(cap);
  if (!body)
    return 0;
  size_t got = f.read(body, cap);
  if (got < 10) {
    free(body);
    return 0;
  }

  uint32_t off = 0;
  if (ver_maj == 3 && (flags & 0x40u)) {
    if (got < 4) {
      free(body);
      return 0;
    }
    uint32_t ext = id3_read_u32be(body);
    if (ext < 4 || ext > got) {
      free(body);
      return 0;
    }
    off = ext;
  } else if (ver_maj == 4 && (flags & 0x40u)) {
    if (got < 6) {
      free(body);
      return 0;
    }
    uint32_t ext = id3_synchsafe_u32(body + 1);
    if (ext < 6 || ext > got) {
      free(body);
      return 0;
    }
    off = ext;
  }
  (void)flags;

  uint16_t out = 0;
  if (ver_maj == 2) {
    while (off + 6 <= got) {
      if (body[off] == 0)
        break;
      char id4[4];
      memcpy(id4, body + off, 3);
      id4[3] = 0;
      uint32_t fr = (uint32_t)body[off + 3] << 16 | (uint32_t)body[off + 4] << 8 | (uint32_t)body[off + 5];
      off += 6;
      if (fr > 256u * 1024u || off + fr > got)
        break;
      if (strcmp(id4, "TBP") == 0) {
        out = id3_parse_tbpm_bytes(body + off, fr);
        if (out)
          break;
      }
      off += fr;
    }
  } else if (ver_maj == 3) {
    while (off + 10 <= got) {
      if (body[off] == 0)
        break;
      char id5[5];
      memcpy(id5, body + off, 4);
      id5[4] = 0;
      uint32_t fr = id3_read_u32be(body + off + 4);
      off += 10;
      if (fr > 256u * 1024u || off + fr > got)
        break;
      if (strcmp(id5, "TBPM") == 0) {
        out = id3_parse_tbpm_bytes(body + off, fr);
        if (out)
          break;
      }
      off += fr;
    }
  } else if (ver_maj == 4) {
    while (off + 10 <= got) {
      if (body[off] == 0)
        break;
      char id5[5];
      memcpy(id5, body + off, 4);
      id5[4] = 0;
      uint32_t fr = id3_synchsafe_u32(body + off + 4);
      off += 10;
      if (fr > 256u * 1024u || off + fr > got)
        break;
      if (strcmp(id5, "TBPM") == 0) {
        out = id3_parse_tbpm_bytes(body + off, fr);
        if (out)
          break;
      }
      off += fr;
    }
  }

  free(body);
  return out;
}
#endif // RHYTHM_ENABLE_SD ID3

#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
// Optional PC-written sidecar: same path as .mp3 but extension .rhy (UTF-8 text, one key per line).
//   bpm 128
//   offset_ms 240
//   difficulty 3
// Keys: bpm (40–320), offset_ms|offset|off (0–120000 ms), d|difficulty (1–9). # starts comment.
// Chart is still one fixed BPM for the whole song — use a reference tempo + offset for live/DJ tracks.
static void mp3_to_sidecar_path(const char *mp3, char *out, size_t cap) {
  strncpy(out, mp3, cap - 1);
  out[cap - 1] = 0;
  char *dot = strrchr(out, '.');
  if (dot && ends_mp3_name(out)) {
    if ((size_t)(dot - out) + 5 < cap)
      strcpy(dot, ".rhy");
  } else if (strlen(out) + 5 < cap)
    strcat(out, ".rhy");
}

static void rhythmParseSidecarLine(char *line, RhythmTrackInfo *t) {
  char *s = line;
  while (*s == ' ' || *s == '\t')
    s++;
  if (!*s || *s == '#')
    return;
  for (char *q = s; *q; q++)
    *q = (char)tolower((unsigned char)*q);
  char key[20];
  unsigned val = 0;
  if (sscanf(s, "%19s %u", key, &val) < 2)
    return;
  if (strcmp(key, "bpm") == 0) {
    if (val >= 40u && val <= 320u)
      t->bpm = (uint16_t)val;
  } else if (strcmp(key, "offset_ms") == 0 || strcmp(key, "offset") == 0 || strcmp(key, "off") == 0) {
    if (val <= 120000u)
      t->beatOffsetMs = (uint16_t)val;
  } else if (strcmp(key, "d") == 0 || strcmp(key, "difficulty") == 0) {
    if (val >= 1u && val <= 9u)
      t->difficulty = (uint8_t)val;
  }
}

static void rhythmMergeSidecar(fs::FS &fs, const char *mp3Path, RhythmTrackInfo *t) {
  char scPath[RHYTHM_PATH_MAX];
  mp3_to_sidecar_path(mp3Path, scPath, sizeof(scPath));
  if (!fs.exists(scPath))
    return;
  File sf = fs.open(scPath, FILE_READ);
  if (!sf || sf.isDirectory()) {
    if (sf)
      sf.close();
    return;
  }
  char line[72];
  size_t li = 0;
  while (sf.available()) {
    int raw = sf.read();
    if (raw < 0)
      break;
    char c = (char)raw;
    if (c == '\r')
      continue;
    if (c == '\n') {
      if (li < sizeof(line) - 1)
        line[li] = 0;
      else
        line[sizeof(line) - 1] = 0;
      li = 0;
      rhythmParseSidecarLine(line, t);
      continue;
    }
    if (li + 1 < sizeof(line))
      line[li++] = c;
  }
  if (li > 0) {
    line[li < sizeof(line) - 1 ? li : sizeof(line) - 1] = 0;
    rhythmParseSidecarLine(line, t);
  }
  sf.close();
}
#endif

// Count .mp3 entries (same rules as scan) for Serial diagnostics — does not open each file.
static void rhythmDirCountMp3(fs::FS &fs, int *nMp3, int *nTooLongPath) {
  *nMp3 = *nTooLongPath = 0;
  File dir = fs.open(kRhythmSubdir);
  if (!dir || !dir.isDirectory())
    return;
  dir.rewindDirectory();
  for (;;) {
    boolean isDir = false;
    String pathStr = dir.getNextFileName(&isDir);
    if (!pathStr.length())
      break;
    if (isDir)
      continue;
    const char *full = pathStr.c_str();
    if (!ends_mp3_name(full))
      continue;
    (*nMp3)++;
    if (strlen(full) >= RHYTHM_PATH_MAX)
      (*nTooLongPath)++;
  }
  dir.close();
}

static void scan_fs_dir(fs::FS &fs, RhythmTrackInfo *out, int *count, int maxOut, bool skipIfDup) {
  File dir = fs.open(kRhythmSubdir);
  if (!dir || !dir.isDirectory())
    return;
  // openNextFile() iteration is flaky on some ESP32-S3 + SD setups; use readdir via getNextFileName().
  dir.rewindDirectory();
  while (*count < maxOut) {
    boolean isDir = false;
    String pathStr = dir.getNextFileName(&isDir);
    if (!pathStr.length())
      break;
    if (isDir)
      continue;
    const char *full = pathStr.c_str();
    if (!ends_mp3_name(full))
      continue;
    size_t fullLen = strlen(full);
    if (fullLen >= RHYTHM_PATH_MAX) {
      Serial.printf("[RHYTHM] Skip (path >= %d chars): %s\n", RHYTHM_PATH_MAX, full);
      continue;
    }
    char p[RHYTHM_PATH_MAX];
    strncpy(p, full, sizeof(p) - 1);
    p[sizeof(p) - 1] = 0;
    if (skipIfDup && basename_in_existing(p, out, *count))
      continue;
    size_t fsz = 0;
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
    uint16_t id3Bpm = 0;
#endif
    File mf = fs.open(full);
    if (mf && !mf.isDirectory()) {
      fsz = mf.size();
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
      id3Bpm = rhythmProbeId3Bpm(mf);
#endif
      mf.close();
    } else if (mf)
      mf.close();
    RhythmTrackInfo *t = &out[*count];
    memset(t, 0, sizeof(*t));
    strncpy(t->path, p, sizeof(t->path) - 1);
    t->path[sizeof(t->path) - 1] = 0;
    title_from_path(p, t->title, sizeof(t->title));
    t->approxDurationMs = estimate_ms_from_size(fsz);
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
    t->bpm = id3Bpm ? id3Bpm : bpm_from_filename(p);
    rhythmMergeSidecar(fs, p, t);
#endif
    (*count)++;
  }
  dir.close();
}

static int cmp_tracks_by_title(const void *a, const void *b) {
  const RhythmTrackInfo *x = (const RhythmTrackInfo *)a;
  const RhythmTrackInfo *y = (const RhythmTrackInfo *)b;
  const char *pa = x->title;
  const char *pb = y->title;
  while (*pa && *pb) {
    int ca = tolower((unsigned char)*pa++);
    int cb = tolower((unsigned char)*pb++);
    if (ca != cb)
      return ca - cb;
  }
  return (unsigned char)*pa - (unsigned char)*pb;
}

int rhythmScanTrackFolder(RhythmTrackInfo *out, int maxOut) {
  if (!out || maxOut <= 0)
    return 0;
  rhythmTryMountSd(true);
  int n = 0;
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0)
  if (s_sd_mounted)
    scan_fs_dir(SD, out, &n, maxOut, false);
  int dirMp3 = 0, tooLong = 0;
  if (s_sd_mounted)
    rhythmDirCountMp3(SD, &dirMp3, &tooLong);
  if (s_sd_mounted && (tooLong > 0 || dirMp3 != n)) {
    Serial.printf("[RHYTHM] /rhythm: %d .mp3 filenames, %d listed", dirMp3, n);
    if (tooLong)
      Serial.printf("; %d skipped (path >= %d chars — shorten name)", tooLong, RHYTHM_PATH_MAX);
    if (n == maxOut && dirMp3 > n)
      Serial.printf("; hit track cap %d (raise RHYTHM_MAX_SCAN_TRACKS)", maxOut);
    Serial.println();
  }
#endif
  if (n > 1)
    qsort(out, (size_t)n, sizeof(RhythmTrackInfo), cmp_tracks_by_title);
  buttonsRestoreInputPullups();
  return n;
}

#else

int rhythmScanTrackFolder(RhythmTrackInfo *out, int maxOut) {
  (void)out;
  (void)maxOut;
  return 0;
}

#endif

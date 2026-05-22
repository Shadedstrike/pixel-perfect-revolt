#include "scales.h"
#include "config.h"

// ===== Scales =====
static float baseAHz = 220.0f;
// Ceiling only (~15% under 220×2^2): pitch-up / high degrees can’t go as shrill; lows unchanged.
static const float kMaxSynthHz = 220.0f * powf(2.0f, 24.0f / 12.0f) * 0.85f;

static inline float clampSynthHz(float hz) {
  if (hz <= 0.f)
    return hz;
  return fminf(hz, kMaxSynthHz);
}

const int8_t MAJ_PENTA[]  = {0,2,4,7,9,12,14,16,19,21,24};
const int8_t MIN_PENTA[]  = {0,3,5,7,10,12,15,17,19,22,24};
const int8_t MAJOR[]      = {0,2,4,5,7,9,11,12,14,16,17,19,21,23,24};
const int8_t NAT_MINOR[]  = {0,2,3,5,7,8,10,12,14,15,17,19,20,22,24};
const int8_t WHOLE[]      = {0,2,4,6,8,10,12,14,16,18,20,22,24};
const int8_t CHROMA[]     = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};

ScaleDef SCALES[] = {
  {"Major Pentatonic", MAJ_PENTA, (uint8_t)(sizeof(MAJ_PENTA)/sizeof(MAJ_PENTA[0]))},
  {"Minor Pentatonic", MIN_PENTA, (uint8_t)(sizeof(MIN_PENTA)/sizeof(MIN_PENTA[0]))},
  {"Major (Ionian)",   MAJOR,     (uint8_t)(sizeof(MAJOR)/sizeof(MAJOR[0]))},
  {"Natural Minor",    NAT_MINOR, (uint8_t)(sizeof(NAT_MINOR)/sizeof(NAT_MINOR[0]))},
  {"Whole Tone",       WHOLE,     (uint8_t)(sizeof(WHOLE)/sizeof(WHOLE[0]))},
  {"Chromatic",        CHROMA,    (uint8_t)(sizeof(CHROMA)/sizeof(CHROMA[0]))},
};

const uint8_t NUM_SCALES = sizeof(SCALES)/sizeof(SCALES[0]);

float   SCALE_HZ[MAX_SCALE_SIZE];
uint8_t SCALE_LEN=0;
uint8_t scaleIndex=0;
static int8_t transpose=0;

void buildScaleHz(){
  ScaleDef &S=SCALES[scaleIndex];
  SCALE_LEN=S.count;
  float tr=semiRatio((float)transpose);
  for (uint8_t i=0;i<S.count;i++)
    SCALE_HZ[i] = clampSynthHz(baseAHz * powf(2.0f, S.steps[i] / 12.0f) * tr);
}

// map 4 side buttons to degree indices - consecutive scale degrees for melodic playability
// Bottom to top: consecutive scale degrees (0, 1, 2, 3) for better melodic flow
int degreeIndexForSlot(int slot /*0..3*/){
  if (SCALE_LEN<=1) return 0;
  // Use consecutive scale degrees: slot 0 = index 0, slot 1 = index 1, etc.
  // This creates a more melodic, playable arrangement
  int idx = slot;
  // If scale is shorter than 4 notes, wrap around
  if (idx >= SCALE_LEN) {
    // Wrap to beginning, but keep ascending pattern
    idx = idx % SCALE_LEN;
  }
  if (idx<0) idx=0; 
  if (idx>=SCALE_LEN) idx=SCALE_LEN-1;
  return idx;
}

float scaleHzAtIdx(int idx){
  // Handle negative indices and indices beyond scale bounds
  // The idx parameter can be negative or beyond SCALE_LEN when offsets are applied
  // We need to calculate the actual semitone offset from the scale definition
  
  extern uint8_t scaleIndex;
  extern ScaleDef SCALES[];
  
  if (idx < 0) {
    // Negative index: calculate frequency going down from first scale note
    // Get the semitone offset of the first note and subtract the negative offset
    ScaleDef &S = SCALES[scaleIndex];
    if (S.count == 0) return baseAHz;
    int firstNoteSemitones = S.steps[0];
    int semitonesDown = -idx; // How many semitones to go down
    int totalSemitones = firstNoteSemitones - semitonesDown;
    return clampSynthHz(baseAHz * powf(2.0f, (float)totalSemitones / 12.0f));
  } else if (idx >= SCALE_LEN) {
    // Beyond scale: calculate frequency going up from last scale note
    ScaleDef &S = SCALES[scaleIndex];
    if (S.count == 0) return baseAHz;
    int lastNoteSemitones = S.steps[SCALE_LEN - 1];
    int semitonesUp = idx - (SCALE_LEN - 1);
    int totalSemitones = lastNoteSemitones + semitonesUp;
    return clampSynthHz(baseAHz * powf(2.0f, (float)totalSemitones / 12.0f));
  }
  return SCALE_HZ[idx];
}

// quick note name (approx, A=baseAHz as tonic-ish)
const char* noteNameFromHz(float hz){
  if (hz<=0) return "--";
  static const char* N[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  float n = 12.0f*log2f(hz/440.0f) + 69.0f; // MIDI-ish
  int midi = (int)lroundf(n);
  int nn = ((midi % 12)+12)%12;
  static char buf[8];
  snprintf(buf,sizeof(buf),"%s%d", N[nn], (midi/12)-1);
  return buf;
}


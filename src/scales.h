#ifndef SCALES_H
#define SCALES_H

#include <Arduino.h>
#include <math.h>

static inline float semiRatio(float s){ return powf(2.0f, s/12.0f); }

struct ScaleDef { 
  const char* name; 
  const int8_t* steps; 
  uint8_t count; 
};

#define MAX_SCALE_SIZE 32

extern const int8_t MAJ_PENTA[];
extern const int8_t MIN_PENTA[];
extern const int8_t MAJOR[];
extern const int8_t NAT_MINOR[];
extern const int8_t WHOLE[];
extern const int8_t CHROMA[];

extern ScaleDef SCALES[];
extern const uint8_t NUM_SCALES;

extern float SCALE_HZ[MAX_SCALE_SIZE];
extern uint8_t SCALE_LEN;
extern uint8_t scaleIndex;

void buildScaleHz();
int degreeIndexForSlot(int slot);
float scaleHzAtIdx(int idx);
const char* noteNameFromHz(float hz);

#endif // SCALES_H


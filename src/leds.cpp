#include "leds.h"
#include <Wire.h>
#include <math.h>

// LED state variables
float pressHue[10] = {0,36,72,108,144,180,216,252,288,324};
const uint32_t PRESS_FADE_MS=1000, IDLE_AFTER_MS=3000;
const float IDLE_V_MIN=0.08f, IDLE_V_MAX=0.28f, IDLE_LFO_HZ=1.0f/3.2f, IDLE_SAT=1.0f;
const float PRESS_V=1.0f, RAINBOW_HZ=0.2f; // 5s sweep

// Flame params
const uint32_t FLAME_UPDATE_MS=75;
bool flipLeft=true, flipRight=true;
float flameL[4]={0,0,0,0}, flameR[4]={0,0,0,0};
uint32_t lastFlameStepMs=0;

// Button/LED state
bool down[10]={0};
bool edgeDownArr[10]={0}, edgeUpArr[10]={0};
uint32_t releaseTs[10]={0}, lastPressMs=0;

uint8_t lastR[10]={0}, lastG[10]={0}, lastB[10]={0};
float idleRateDegPerSec[10], idleBaseHue[10];

uint8_t idleMode=1; // 0=RGB breathe, 1=warm flame, 2=cool flame, 3=RGB+flame, 4=solid colors, 5=rainbow wave, 6=aurora, 7=starlight, 8=gradient flow, 9=matrix rain, 10=lightning strike, 11=plasma swirl
uint32_t bothHoldStart=0; bool holdLatch=false;

float rgbBaseHue[10]; float rgbHueRateDegPerSec[10];
float rainbowStartHue[10];

// PCA9685 drivers
Adafruit_PWMServoDriver pcaA(PCA_A_ADDR, Wire);
Adafruit_PWMServoDriver pcaB(PCA_B_ADDR, Wire);

uint16_t lastPWM_A[16], lastPWM_B[16];

void getPressColorForGPIO(int gpio, uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (gpio) {
    case 38: case 39: r=255; g=200; b=0;   return; // yellow (39 = right top; 11 is SD MOSI only)
    case 12: case 2:  r=0;   g=64;  b=255; return; // blue
    case 5:  case 15: r=0;   g=255; b=0;   return; // green (15 = right key moved off SD MISO)
    case 7:  case 8:  r=255; g=0;   b=0;   return; // red
    default: r=g=b=0; return;
  }
}

static inline uint16_t to12bit(uint8_t v){
  float x=v/255.0f; x=powf(x,GAMMA);
  uint16_t p=(uint16_t)lroundf(x*4095.0f);
  if (LED_INVERT) p=4095-p;
  return p;
}

void pcaSet(uint8_t drv,uint8_t ch,uint8_t v){
  uint16_t pwm=to12bit(v);
  if (drv==0){ if (lastPWM_A[ch]!=pwm){ lastPWM_A[ch]=pwm; pcaA.setPWM(ch,0,pwm);} }
  else       { if (lastPWM_B[ch]!=pwm){ lastPWM_B[ch]=pwm; pcaB.setPWM(ch,0,pwm);} }
}

void setLED_RGB(uint8_t i,uint8_t r,uint8_t g,uint8_t b){
  if (i == IDX_FRONT_L || i == IDX_FRONT_R) {
    r = (uint8_t)lroundf((float)r * FRONT_LED_BRIGHTNESS_SCALE);
    g = (uint8_t)lroundf((float)g * FRONT_LED_BRIGHTNESS_SCALE);
    b = (uint8_t)lroundf((float)b * FRONT_LED_BRIGHTNESS_SCALE);
  }
  pcaSet(MAP[i].r.drv,MAP[i].r.ch,r);
  pcaSet(MAP[i].g.drv,MAP[i].g.ch,g);
  pcaSet(MAP[i].b.drv,MAP[i].b.ch,b);
}

void hsv2rgb(float h,float s,float v,uint8_t &r,uint8_t &g,uint8_t &b){
  while(h<0)h+=360; while(h>=360)h-=360;
  float c=v*s, x=c*(1.0f-fabsf(fmodf(h/60.0f,2.0f)-1.0f)), m=v-c;
  float rf=0,gf=0,bf=0;
  if(h<60){rf=c;gf=x;} else if(h<120){rf=x;gf=c;}
  else if(h<180){gf=c;bf=x;} else if(h<240){gf=x;bf=c;}
  else if(h<300){rf=x;bf=0;} else {rf=c;bf=x;}
  r=(uint8_t)lroundf((rf+m)*255); g=(uint8_t)lroundf((gf+m)*255); b=(uint8_t)lroundf((bf+m)*255);
}

void applyVFloor(uint8_t &r,uint8_t &g,uint8_t &b,float floorV){
  uint8_t maxc=max(r,max(g,b)); uint8_t target=(uint8_t)lroundf(floorV*255);
  if (maxc<target && maxc>0){ float s=target/(float)maxc; r=min(255,(int)lroundf(r*s)); g=min(255,(int)lroundf(g*s)); b=min(255,(int)lroundf(b*s)); }
  else if (maxc==0){ r=target; g=0; b=0; }
}

void warmManualRGB(float v, uint8_t &r,uint8_t &g,uint8_t &b){
  float vv=powf(fmaxf(v,0.0f),1.10f);
  float rF=0.55f+0.45f*vv; float gF=rF*(0.10f+0.45f*vv);
  int Ri=(int)lroundf(rF*255), Gi=(int)lroundf(gF*255);
  if (Gi> (int)lroundf(Ri*0.55f)) Gi=(int)lroundf(Ri*0.55f);
  r=min(255,Ri); g=max(0,Gi); b=0;
}

void flameStep(float f[4]){
  float n[4];
  bool spark=(random(0,100)<26);
  float s=spark? (random(22,90)/100.0f):(random(0,10)/260.0f);
  n[0]=f[0]*0.88f + s + (random(-1,2))/255.0f;
  for(int i=1;i<4;i++){
    float below=f[i-1], here=f[i];
    float carry=below*1.10f, decay=here*0.88f, diff=(below-here)*0.05f;
    float bias=0.05f*below + 0.04f*below*below;
    float damp=fmaxf(here-below,0.0f)*0.90f;
    float jit=(random(-1,2))/255.0f;
    n[i]=decay+carry+diff+bias-damp+jit;
  }
  if (random(0,1000)<6) n[3]+=0.30f;
  for(int i=0;i<4;i++){ n[i]=fminf(1.0f,fmaxf(0.0f,powf(n[i],1.18f))); f[i]=n[i]; }
}

void flameToRGB(float v,bool cool,uint8_t &r,uint8_t &g,uint8_t &b){
  if(!cool){ warmManualRGB(v,r,g,b); }
  else{
    float vv=powf(fmaxf(v,0.0f),1.20f);
    // Cool flame: blue (240°) -> purple (270°) -> pink/magenta (300-330°) -> red (0-20°)
    // Full range covering blue, purple, pink, and red
    // Use range 240° to 360° (wraps to 0°), covering 120° of hue space
    float hue=240.0f+120.0f*vv; // 240° to 360° (wraps to 0°)
    float val=0.08f+0.92f*vv;
    hsv2rgb(hue,1.0f,val,r,g,b);
  }
}

void mixGroupColors(uint32_t now,const int *idxs,int n,float &mr,float &mg,float &mb,float &sumW){
  mr=mg=mb=0; sumW=0;
  for(int k=0;k<n;k++){
    int i=idxs[k]; float w=0, cr=0,cg=0,cb=0;
    if (down[i]){ uint8_t pr,pg,pb; getPressColorForGPIO(BTN_PINS[i],pr,pg,pb);
      cr=pr*PRESS_V; cg=pg*PRESS_V; cb=pb*PRESS_V; w=1.0f;
    } else {
      uint32_t dt = now - releaseTs[i];
      if (dt<PRESS_FADE_MS){ float a=1.0f-(dt/(float)PRESS_FADE_MS);
        cr=lastR[i]*a; cg=lastG[i]*a; cb=lastB[i]*a; w=a; }
    }
    if(w>0){ mr+=w*cr; mg+=w*cg; mb+=w*cb; sumW+=w; }
  }
  if (sumW>0){ mr/=sumW; mg/=sumW; mb/=sumW; }
}

// Boot-up LED animation: green/turquoise pulsing, starting with 38 and 11
void playBootAnimation(){
  const uint32_t ANIMATION_DURATION_MS = 2000; // 2 seconds for gradual build-up
  const uint32_t FULL_COLOR_DURATION_MS = 250; // 0.25 seconds at full color
  const uint32_t TOTAL_DURATION_MS = ANIMATION_DURATION_MS + FULL_COLOR_DURATION_MS;
  
  // Green/turquoise color range: 150-180 degrees (green to cyan/turquoise)
  const float HUE_START = 150.0f; // Green
  const float HUE_END = 180.0f;   // Turquoise/cyan
  const float SATURATION = 1.0f;
  
  // Button order: start with 38 (IDX_0) and 11 (IDX_6), then add others
  // IDX_LEFT: 0,1,2,3 (38,12,5,7)
  // IDX_RIGHT: 6,7,8,9 (39,2,15,8)
  // IDX_FRONT: 4,5 (16,46)
  const uint8_t START_BUTTONS[] = {0, 6}; // 38 and 11
  const uint8_t OTHER_BUTTONS[] = {1, 2, 3, 4, 5, 7, 8, 9}; // Rest
  
  uint32_t startTime = millis();
  const float TAU = 6.28318530718f;
  const float PULSE_RATE = 1.5f; // Pulses per second
  
  while(millis() - startTime < TOTAL_DURATION_MS){
    uint32_t elapsed = millis() - startTime;
    float t = elapsed / 1000.0f;
    
    // Pulsing brightness (sine wave)
    float pulse = 0.3f + 0.7f * (0.5f + 0.5f * sinf(TAU * PULSE_RATE * t));
    
    // Phase hue between green and turquoise
    float huePhase = sinf(TAU * 0.3f * t); // Slow phase shift
    float hue = HUE_START + (HUE_END - HUE_START) * (0.5f + 0.5f * huePhase);
    
    if(elapsed < ANIMATION_DURATION_MS){
      // Gradual build-up phase
      float buildProgress = (float)elapsed / ANIMATION_DURATION_MS;
      
      // Start buttons (38 and 11) always pulsing from the very start (elapsed = 0)
      // Make them immediately visible with minimum brightness
      float startButtonBrightness = pulse;
      if(elapsed < 100){ // First 100ms: fade in quickly
        startButtonBrightness = pulse * (elapsed / 100.0f);
      }
      for(int i = 0; i < 2; i++){
        uint8_t idx = START_BUTTONS[i];
        uint8_t r, g, b;
        hsv2rgb(hue, SATURATION, startButtonBrightness, r, g, b);
        setLED_RGB(idx, r, g, b);
      }
      
      // Add other buttons gradually
      int numOtherButtons = sizeof(OTHER_BUTTONS) / sizeof(OTHER_BUTTONS[0]);
      for(int i = 0; i < numOtherButtons; i++){
        uint8_t idx = OTHER_BUTTONS[i];
        // Each button starts pulsing at different times
        float buttonStart = (float)i / numOtherButtons;
        if(buildProgress >= buttonStart){
          float buttonProgress = (buildProgress - buttonStart) / (1.0f - buttonStart);
          float buttonBrightness = pulse * buttonProgress;
          uint8_t r, g, b;
          hsv2rgb(hue, SATURATION, buttonBrightness, r, g, b);
          setLED_RGB(idx, r, g, b);
        } else {
          setLED_RGB(idx, 0, 0, 0); // Off
        }
      }
    } else {
      // Full color phase (0.25s)
      uint8_t r, g, b;
      hsv2rgb(hue, SATURATION, 1.0f, r, g, b); // Full brightness
      for(int i = 0; i < 10; i++){
        setLED_RGB(i, r, g, b);
      }
    }
    
    delay(10); // Update every 10ms for smooth animation
  }
  
  // Turn all lights off
  for(int i = 0; i < 10; i++){
    setLED_RGB(i, 0, 0, 0);
  }
}


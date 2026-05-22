#include "audio.h"
#include "config.h"
#include "scales.h"
#include "leds.h"
#include <math.h>
#include <string.h>

// Synth state
static AudioWaveShape s_waveShape = AUDIO_WAVE_SINE;

float audioWaveShapeSample(AudioWaveShape shape, float phaseRad) {
  const float TAU = 6.28318530718f;
  float ph = phaseRad;
  while (ph >= TAU)
    ph -= TAU;
  while (ph < 0.f)
    ph += TAU;

  switch (shape) {
  case AUDIO_WAVE_SINE:
    return sinf(ph);
  case AUDIO_WAVE_TRIANGLE: {
    float n = ph * (1.f / TAU);
    return n < 0.5f ? (4.f * n - 1.f) : (3.f - 4.f * n);
  }
  case AUDIO_WAVE_SOFT_SQUARE:
    return tanhf(2.8f * sinf(ph));
  case AUDIO_WAVE_RICH: {
    float s = sinf(ph) + 0.42f * sinf(2.f * ph) + 0.20f * sinf(3.f * ph) + 0.10f * sinf(4.f * ph);
    return s * 0.58f;
  }
  default:
    return sinf(ph);
  }
}

static float audioOscSample(float phaseRad) {
  return audioWaveShapeSample(s_waveShape, phaseRad);
}

void audioCycleWaveShape() {
  uint8_t n = (uint8_t)s_waveShape + 1;
  if (n >= (uint8_t)AUDIO_WAVE_COUNT)
    n = 0;
  s_waveShape = (AudioWaveShape)n;
}

AudioWaveShape audioGetWaveShape() { return s_waveShape; }

const char *audioWaveShapeName(AudioWaveShape w) {
  switch (w) {
  case AUDIO_WAVE_SINE:
    return "SINE";
  case AUDIO_WAVE_TRIANGLE:
    return "TRI";
  case AUDIO_WAVE_SOFT_SQUARE:
    return "SOFT";
  case AUDIO_WAVE_RICH:
    return "RICH";
  default:
    return "SINE";
  }
}

float freqL=0, freqR=0, curFreqL=0, curFreqR=0;
float vibPhaseL=0,vibPhaseR=0, phaseL=0, phaseR=0, ampL=0, ampR=0;
float curFreqCompL=1.0f, curFreqCompR=1.0f; // Smoothed frequency compensation to prevent clicks
uint32_t lastGoodI2S=0;
int i2s_consec_errors=0;
bool i2s_initialized = false;

void audioInit(){
  // Uninstall any existing driver first
  if(i2s_initialized){
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_initialized = false;
    delay(50);
  }
  
  i2s_config_t cfg = {
    .mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX),
    .sample_rate=SR, .bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT,
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 4)
    .communication_format=I2S_COMM_FORMAT_STAND_I2S,
#else
    .communication_format=(i2s_comm_format_t)I2S_COMM_FORMAT_I2S,
#endif
    .intr_alloc_flags=0, .dma_buf_count=DMA_COUNT, .dma_buf_len=BUF_SAMPLES,
    .use_apll=false, .tx_desc_auto_clear=true, .fixed_mclk=0
  };
  
  Serial.printf("[I2S] Installing I2S driver...\n");
  Serial.printf("[I2S] Config: SR=%d, bits=16, channels=stereo, DMA=%dx%d\n", SR, DMA_COUNT, BUF_SAMPLES);
  esp_err_t ret = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if(ret != ESP_OK){
    Serial.printf("[I2S] ERROR: I2S driver install failed: %d (0x%x)\n", ret, ret);
    Serial.printf("[I2S] ERROR: Check if I2S_NUM_0 is available\n");
    i2s_initialized = false;
    return;
  }
  Serial.printf("[I2S] I2S driver installed OK\n");
  
  // Don't manually set pinMode for I2S pins - the driver handles it
  // pinMode calls can interfere with I2S operation
  delay(10);
  
  i2s_pin_config_t pins;
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_LRCK;
  pins.data_out_num = I2S_DATA;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  
  Serial.printf("[I2S] Setting I2S pins: BCLK=%d, LRCLK=%d, DATA=%d\n", I2S_BCLK, I2S_LRCK, I2S_DATA);
  ret = i2s_set_pin(I2S_NUM_0, &pins);
  if(ret != ESP_OK){
    Serial.printf("[I2S] ERROR: I2S set pin failed with error code: %d (0x%x)\n", ret, ret);
    Serial.printf("[I2S] ERROR: Pin %d (LRCLK) may not be valid for I2S on ESP32-S3\n", I2S_LRCK);
    Serial.printf("[I2S] ERROR: Try different pins or check pin capabilities\n");
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_initialized = false;
    return;
  }
  Serial.printf("[I2S] I2S pins set OK\n");
  
  i2s_set_sample_rates(I2S_NUM_0, SR);
  i2s_set_clk(I2S_NUM_0, SR, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_start(I2S_NUM_0);
  lastGoodI2S=millis();
  i2s_initialized = true;
  
  Serial.printf("[I2S] I2S initialized successfully!\n");
  Serial.printf("[I2S] Pins: BCLK=%d, LRCLK=%d, DATA=%d\n", I2S_BCLK, I2S_LRCK, I2S_DATA);
  Serial.printf("[I2S] Sample rate: %d Hz, Format: 16-bit stereo I2S\n", SR);
  Serial.printf("[I2S] Channel format: RIGHT_LEFT (check if DAC expects LEFT_RIGHT)\n");
  Serial.printf("[I2S] Communication format: STAND_I2S\n");
  Serial.printf("[I2S] If no audio output, try:\n");
  Serial.printf("[I2S]   1. Verify DAC power (3.3V or 5V)\n");
  Serial.printf("[I2S]   2. Check pin connections match config\n");
  Serial.printf("[I2S]   3. Verify amplifier/speaker connections\n");
  Serial.printf("[I2S]   4. Test with buttons pressed to generate audio\n");
  delay(10);
}

void playTestTone(float freqHz, uint32_t durationMs){
  if(!i2s_initialized){
    Serial.println("playTestTone: I2S not initialized, skipping");
    return;
  }
  
  const float TAU = 6.28318530718f;
  float phase = 0.0f;
  float phaseInc = TAU * freqHz / SR;
  uint32_t samplesNeeded = (uint32_t)((SR * durationMs) / 1000);
  uint32_t samplesPlayed = 0;
  int16_t buf[BUF_SAMPLES*2];
  
  // Split duration: first half on left, second half on right
  uint32_t samplesPerChannel = samplesNeeded / 2;
  
  while(samplesPlayed < samplesNeeded){
    size_t samplesThisBlock = (samplesNeeded - samplesPlayed < BUF_SAMPLES) ? 
                              (samplesNeeded - samplesPlayed) : BUF_SAMPLES;
    
    for(size_t i=0; i<samplesThisBlock; i++){
      float sample = sinf(phase) * 0.4f; // 50% of previous 0.8f volume
      int16_t s16 = (int16_t)(sample * 16383.0f);
      
      // First half: left channel only, second half: right channel only
      if(samplesPlayed + i < samplesPerChannel){
        buf[2*i+0] = s16; // Left channel
        buf[2*i+1] = 0;   // Right channel silent
      } else {
        buf[2*i+0] = 0;   // Left channel silent
        buf[2*i+1] = s16; // Right channel
      }
      
      phase += phaseInc;
      if(phase >= TAU) phase -= TAU;
    }
    
    for(size_t i=samplesThisBlock; i<BUF_SAMPLES; i++){
      buf[2*i+0] = 0;
      buf[2*i+1] = 0;
    }
    
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    if(err == ESP_OK && written > 0){
      samplesPlayed += samplesThisBlock;
      if(samplesPlayed % (SR/10) == 0){ // Print progress every 100ms
        Serial.printf("[I2S] Test tone: %lu/%lu samples (%.1f%%)\n", 
                      samplesPlayed, samplesNeeded, (100.0f * samplesPlayed / samplesNeeded));
      }
    } else {
      Serial.printf("[I2S] ERROR: i2s_write failed: err=%d, written=%d\n", err, written);
      break;
    }
    delay(1);
  }
  delay(50);
  Serial.printf("[I2S] Test tone finished. Check serial output above for any errors.\n");
}

// Randomized geiger counter noise - left then right, quiet
void playGeigerCounter(){
  if(!i2s_initialized){
    Serial.println("playGeigerCounter: I2S not initialized, skipping");
    return;
  }
  
  const uint32_t DURATION_MS = 800; // 0.4s per channel
  const float CLICK_VOLUME = 0.15f; // Quiet volume
  const float CLICK_FREQ_MIN = 800.0f; // Hz
  const float CLICK_FREQ_MAX = 2000.0f; // Hz
  const uint32_t CLICK_DURATION_SAMPLES = (uint32_t)(SR * 0.003f); // 3ms clicks
  const uint32_t MIN_CLICK_INTERVAL = (uint32_t)(SR * 0.02f); // 20ms minimum between clicks
  const uint32_t MAX_CLICK_INTERVAL = (uint32_t)(SR * 0.15f); // 150ms maximum between clicks
  
  uint32_t samplesNeeded = (uint32_t)((SR * DURATION_MS) / 1000);
  uint32_t samplesPerChannel = samplesNeeded / 2;
  uint32_t samplesPlayed = 0;
  int16_t buf[BUF_SAMPLES*2];
  
  // Track click timing for each channel (relative to channel start)
  uint32_t nextClickL = random(0, MAX_CLICK_INTERVAL / 2);
  uint32_t nextClickR = random(0, MAX_CLICK_INTERVAL / 2);
  uint32_t clickStartL = 0;
  uint32_t clickStartR = 0;
  bool inClickL = false;
  bool inClickR = false;
  
  while(samplesPlayed < samplesNeeded){
    size_t samplesThisBlock = (samplesNeeded - samplesPlayed < BUF_SAMPLES) ? 
                              (samplesNeeded - samplesPlayed) : BUF_SAMPLES;
    
    // Clear buffer
    for(size_t i=0; i<BUF_SAMPLES; i++){
      buf[2*i+0] = 0;
      buf[2*i+1] = 0;
    }
    
    // Generate clicks
    for(size_t i=0; i<samplesThisBlock; i++){
      uint32_t currentSample = samplesPlayed + i;
      
      // Left channel clicks (first half)
      if(currentSample < samplesPerChannel){
        if(!inClickL && currentSample >= nextClickL){
          // Start new click
          inClickL = true;
          clickStartL = currentSample;
        }
        if(inClickL){
          uint32_t clickAge = currentSample - clickStartL;
          if(clickAge < CLICK_DURATION_SAMPLES){
            // Generate click with random frequency
            float clickFreq = CLICK_FREQ_MIN + (CLICK_FREQ_MAX - CLICK_FREQ_MIN) * (random(0, 1000) / 1000.0f);
            float phase = 6.28318530718f * clickFreq * clickAge / SR;
            float envelope = 1.0f - ((float)clickAge / CLICK_DURATION_SAMPLES); // Decay
            float sample = sinf(phase) * envelope * CLICK_VOLUME;
            buf[2*i+0] = (int16_t)(sample * 16383.0f);
          } else {
            // Click finished, schedule next one
            inClickL = false;
            nextClickL = currentSample + MIN_CLICK_INTERVAL + random(0, MAX_CLICK_INTERVAL - MIN_CLICK_INTERVAL);
          }
        }
      }
      
      // Right channel clicks (second half)
      if(currentSample >= samplesPerChannel){
        uint32_t rightSample = currentSample - samplesPerChannel;
        if(!inClickR && rightSample >= nextClickR){
          // Start new click
          inClickR = true;
          clickStartR = rightSample;
        }
        if(inClickR){
          uint32_t clickAge = rightSample - clickStartR;
          if(clickAge < CLICK_DURATION_SAMPLES){
            float clickFreq = CLICK_FREQ_MIN + (CLICK_FREQ_MAX - CLICK_FREQ_MIN) * (random(0, 1000) / 1000.0f);
            float phase = 6.28318530718f * clickFreq * clickAge / SR;
            float envelope = 1.0f - ((float)clickAge / CLICK_DURATION_SAMPLES);
            float sample = sinf(phase) * envelope * CLICK_VOLUME;
            buf[2*i+1] = (int16_t)(sample * 16383.0f);
          } else {
            // Click finished, schedule next one
            inClickR = false;
            nextClickR = rightSample + MIN_CLICK_INTERVAL + random(0, MAX_CLICK_INTERVAL - MIN_CLICK_INTERVAL);
          }
        }
      }
    }
    
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    if(err == ESP_OK && written > 0){
      samplesPlayed += samplesThisBlock;
    } else {
      Serial.printf("[I2S] ERROR: i2s_write failed: err=%d, written=%d\n", err, written);
      break;
    }
    delay(1);
  }
}

// Chill power-up sound with rising tones (falling shepard's tone effect)
void playPowerUpSound(){
  if(!i2s_initialized){
    Serial.println("playPowerUpSound: I2S not initialized, skipping");
    return;
  }
  
  const uint32_t DURATION_MS = 750; // ~0.75 seconds
  const float VOLUME = 0.25f; // Chill, non-abrasive volume
  const float BASE_FREQ = 220.0f; // A3
  const float TOP_FREQ = 880.0f; // A5 (2 octaves up)
  
  uint32_t samplesNeeded = (uint32_t)((SR * DURATION_MS) / 1000);
  uint32_t samplesPlayed = 0;
  int16_t buf[BUF_SAMPLES*2];
  
  const float TAU = 6.28318530718f;
  
  while(samplesPlayed < samplesNeeded){
    size_t samplesThisBlock = (samplesNeeded - samplesPlayed < BUF_SAMPLES) ? 
                              (samplesNeeded - samplesPlayed) : BUF_SAMPLES;
    
    for(size_t i=0; i<samplesThisBlock; i++){
      float progress = (float)(samplesPlayed + i) / samplesNeeded;
      
      // Rising frequency with smooth envelope
      float freq = BASE_FREQ + (TOP_FREQ - BASE_FREQ) * progress;
      
      // Smooth envelope: fade in, then fade out
      float envelope;
      if(progress < 0.3f){
        envelope = progress / 0.3f; // Fade in
      } else if(progress > 0.7f){
        envelope = (1.0f - progress) / 0.3f; // Fade out
      } else {
        envelope = 1.0f; // Full volume in middle
      }
      
      // Add multiple harmonics for richer sound (but keep it chill)
      float phase = TAU * freq * (samplesPlayed + i) / SR;
      float sample = sinf(phase) * 0.7f; // Fundamental
      sample += sinf(phase * 2.0f) * 0.2f; // Octave
      sample += sinf(phase * 3.0f) * 0.1f; // Fifth
      sample *= envelope * VOLUME;
      
      int16_t s16 = (int16_t)(sample * 16383.0f);
      buf[2*i+0] = s16; // Left channel
      buf[2*i+1] = s16; // Right channel (stereo)
    }
    
    for(size_t i=samplesThisBlock; i<BUF_SAMPLES; i++){
      buf[2*i+0] = 0;
      buf[2*i+1] = 0;
    }
    
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    if(err == ESP_OK && written > 0){
      samplesPlayed += samplesThisBlock;
    } else {
      Serial.printf("[I2S] ERROR: i2s_write failed: err=%d, written=%d\n", err, written);
      break;
    }
    delay(1);
  }
}

// Combined wakeup sequence: organ power-up with crunchy TV static building up
void playWakeupSequence(uint32_t animStartTime){
  if(!i2s_initialized){
    Serial.println("playWakeupSequence: I2S not initialized, skipping");
    return;
  }
  
  const uint32_t POWERUP_DURATION_MS = 1000; // Organ power-up: 1 second
  const uint32_t STATIC_START_MS = 500; // Static starts 0.5s into organ
  const uint32_t STATIC_BUILD_DURATION_MS = 500; // Static builds up over 0.5s (until powerup ends)
  const uint32_t STATIC_FADE_OUT_MS = 500; // Static fades out quickly after powerup
  const uint32_t STATIC_ALONE_MS = 0; // No static alone time - fades out immediately
  const uint32_t TOTAL_DURATION_MS = POWERUP_DURATION_MS + STATIC_FADE_OUT_MS; // Total duration
  
  // Animation parameters (if animStartTime > 0, we'll update LEDs during audio)
  const bool updateLEDs = (animStartTime > 0);
  const uint32_t PRE_AUDIO_ANIM_MS = 500;
  const uint32_t ANIMATION_DURATION_MS = 2000;
  const uint32_t FULL_COLOR_DURATION_MS = 250;
  const uint32_t TOTAL_ANIMATION_MS = ANIMATION_DURATION_MS + FULL_COLOR_DURATION_MS;
  const float TAU = 6.28318530718f;
  const float PULSE_RATE = 1.5f;
  const float HUE_START = 150.0f;
  const float HUE_END = 180.0f;
  const float SATURATION = 1.0f;
  const uint8_t START_BUTTONS[] = {0, 6}; // 38 and 11
  const uint8_t OTHER_BUTTONS[] = {1, 2, 3, 4, 5, 7, 8, 9};
  uint32_t lastLEDUpdate = 0;
  
  const float ORGAN_VOLUME = 0.35f;
  const float STATIC_VOLUME_MAX = 0.05f; // Max static volume (0.4 of previous 0.125f)
  const float BASE_FREQ = 55.0f; // Very low: A1 (deep organ)
  const float TOP_FREQ = 165.0f; // E3 (still low, organ-like)
  
  uint32_t samplesNeeded = (uint32_t)((SR * TOTAL_DURATION_MS) / 1000);
  uint32_t staticStartSamples = (uint32_t)((SR * STATIC_START_MS) / 1000);
  uint32_t powerupEndSamples = (uint32_t)((SR * POWERUP_DURATION_MS) / 1000);
  uint32_t staticBuildEndSamples = staticStartSamples + (uint32_t)((SR * STATIC_BUILD_DURATION_MS) / 1000);
  uint32_t samplesPlayed = 0;
  int16_t buf[BUF_SAMPLES*2];
  
  // Static noise state (persistent filter state)
  static float lowPassL = 0.0f;
  static float lowPassR = 0.0f;
  
  while(samplesPlayed < samplesNeeded){
    size_t samplesThisBlock = (samplesNeeded - samplesPlayed < BUF_SAMPLES) ? 
                              (samplesNeeded - samplesPlayed) : BUF_SAMPLES;
    
    // Clear buffer
    for(size_t i=0; i<BUF_SAMPLES; i++){
      buf[2*i+0] = 0;
      buf[2*i+1] = 0;
    }
    
    for(size_t i=0; i<samplesThisBlock; i++){
      uint32_t currentSample = samplesPlayed + i;
      
      // Organ power-up sound (more organ-like, sustained)
      if(currentSample < powerupEndSamples){
        float progress = (float)currentSample / powerupEndSamples;
        float freq = BASE_FREQ + (TOP_FREQ - BASE_FREQ) * progress;
        
        // Smooth envelope: fade in, sustain, fade out
        float envelope;
        if(progress < 0.2f){
          envelope = progress / 0.2f; // Fade in over first 20%
        } else if(progress > 0.8f){
          envelope = (1.0f - progress) / 0.2f; // Fade out in last 20%
        } else {
          envelope = 1.0f; // Full volume in middle
        }
        
        // Organ sound: rich harmonic series with strong fundamentals
        // Organs have strong even and odd harmonics, with emphasis on fundamentals
        float phase = TAU * freq * currentSample / SR;
        float sample = sinf(phase) * 0.5f;        // Strong fundamental
        sample += sinf(phase * 2.0f) * 0.35f;     // Octave (very strong in organs)
        sample += sinf(phase * 3.0f) * 0.25f;     // 5th (3rd harmonic)
        sample += sinf(phase * 4.0f) * 0.15f;     // 4th harmonic
        sample += sinf(phase * 5.0f) * 0.1f;      // 5th harmonic
        sample += sinf(phase * 6.0f) * 0.08f;     // 6th harmonic
        sample += sinf(phase * 8.0f) * 0.05f;     // 8th harmonic
        
        // Add sub-bass for deep organ feel
        float rumbleFreq = freq * 0.5f; // Sub-octave
        float rumblePhase = TAU * rumbleFreq * currentSample / SR;
        sample += sinf(rumblePhase) * 0.15f; // Sub-bass
        
        // Apply envelope
        sample *= envelope * ORGAN_VOLUME;
        
        // Stereo: both channels
        int16_t s16 = (int16_t)(sample * 16383.0f);
        buf[2*i+0] += s16;
        buf[2*i+1] += s16;
      }
      
      // Crunchy TV static: starts 0.5s into organ, builds up, then fades out quickly after powerup
      if(currentSample >= staticStartSamples){
        bool inBuildPhase = (currentSample < powerupEndSamples);
        bool inFadeOutPhase = (currentSample >= powerupEndSamples);
        
        // Calculate static volume: builds up during build phase, then fades out quickly after powerup
        float staticVol;
        if(inBuildPhase){
          float buildProgress = (float)(currentSample - staticStartSamples) / (powerupEndSamples - staticStartSamples);
          staticVol = STATIC_VOLUME_MAX * buildProgress * buildProgress; // Quadratic build-up
        } else if(inFadeOutPhase){
          // Fade out quickly after powerup ends
          uint32_t fadeOutStartSamples = powerupEndSamples;
          uint32_t fadeOutEndSamples = fadeOutStartSamples + (uint32_t)((SR * STATIC_FADE_OUT_MS) / 1000);
          if(currentSample < fadeOutEndSamples){
            float fadeOutProgress = (float)(currentSample - fadeOutStartSamples) / (fadeOutEndSamples - fadeOutStartSamples);
            staticVol = STATIC_VOLUME_MAX * (1.0f - fadeOutProgress); // Linear fade out
          } else {
            staticVol = 0.0f; // Fully faded out
          }
        } else {
          staticVol = STATIC_VOLUME_MAX;
        }
        
        // Generate crunchy TV static noise
        // Use multiple noise sources with filtering for that old TV feel
        float noise1 = ((float)(esp_random() % 65536) / 32768.0f) - 1.0f; // White noise
        float noise2 = ((float)(esp_random() % 65536) / 32768.0f) - 1.0f;
        
        // Mix noise sources for texture
        float staticSample = (noise1 * 0.6f + noise2 * 0.4f);
        
        // Apply high-pass filtering effect (old TVs had limited low-end)
        // Simple approximation: subtract low-frequency component
        lowPassL = lowPassL * 0.95f + staticSample * 0.05f; // Low-pass filter
        staticSample = staticSample - lowPassL * 0.3f; // Subtract low-end
        
        // Add distortion for crunchiness
        // Soft clipping distortion
        if(staticSample > 0.7f) staticSample = 0.7f + (staticSample - 0.7f) * 0.3f;
        if(staticSample < -0.7f) staticSample = -0.7f + (staticSample + 0.7f) * 0.3f;
        
        // Hard clipping for extra crunch
        if(staticSample > 1.0f) staticSample = 1.0f;
        if(staticSample < -1.0f) staticSample = -1.0f;
        
        // Apply volume
        staticSample *= staticVol;
        
        // Stereo: same on both channels (mono static)
        int16_t s16Static = (int16_t)(staticSample * 16383.0f);
        buf[2*i+0] += s16Static;
        buf[2*i+1] += s16Static;
      }
    }
    
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    if(err == ESP_OK && written > 0){
      samplesPlayed += samplesThisBlock;
    } else {
      Serial.printf("[I2S] ERROR: i2s_write failed: err=%d, written=%d\n", err, written);
      break;
    }
    
    // Update LEDs during audio generation (every ~10ms)
    if(updateLEDs && (millis() - lastLEDUpdate >= 10)){
      lastLEDUpdate = millis();
      uint32_t elapsed = millis() - animStartTime;
      float t = elapsed / 1000.0f;
      
      // Calculate animation progress (relative to when animation should be running)
      uint32_t animElapsed = (elapsed > PRE_AUDIO_ANIM_MS) ? (elapsed - PRE_AUDIO_ANIM_MS) : 0;
      
      // Pulsing brightness
      float pulse = 0.3f + 0.7f * (0.5f + 0.5f * sinf(TAU * PULSE_RATE * t));
      
      // Phase hue between green and turquoise
      float huePhase = sinf(TAU * 0.3f * t);
      float hue = HUE_START + (HUE_END - HUE_START) * (0.5f + 0.5f * huePhase);
      
      if(animElapsed < ANIMATION_DURATION_MS){
        // Gradual build-up phase
        float buildProgress = (float)animElapsed / ANIMATION_DURATION_MS;
        
        // Start buttons (38 and 11) always pulsing from the very start
        float startButtonBrightness = pulse;
        if(animElapsed < 100 && elapsed >= PRE_AUDIO_ANIM_MS){
          startButtonBrightness = pulse * (animElapsed / 100.0f);
        } else if(elapsed < PRE_AUDIO_ANIM_MS){
          // Before audio starts, fade in start buttons
          startButtonBrightness = pulse * (elapsed / (float)PRE_AUDIO_ANIM_MS);
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
          float buttonStart = (float)i / numOtherButtons;
          if(buildProgress >= buttonStart){
            float buttonProgress = (buildProgress - buttonStart) / (1.0f - buttonStart);
            float buttonBrightness = pulse * buttonProgress;
            uint8_t r, g, b;
            hsv2rgb(hue, SATURATION, buttonBrightness, r, g, b);
            setLED_RGB(idx, r, g, b);
          } else {
            setLED_RGB(idx, 0, 0, 0);
          }
        }
      } else if(animElapsed < TOTAL_ANIMATION_MS){
        // Full color phase (0.25s)
        uint8_t r, g, b;
        hsv2rgb(hue, SATURATION, 1.0f, r, g, b);
        for(int i = 0; i < 10; i++){
          setLED_RGB(i, r, g, b);
        }
      } else {
        // Animation complete - turn off LEDs
        for(int i = 0; i < 10; i++){
          setLED_RGB(i, 0, 0, 0);
        }
      }
    }
    
    delay(1);
  }
}

// Falling Shepard's tone - creates illusion of continuously falling pitch
void playFallingShepardTone(uint32_t durationMs) {
  if(!i2s_initialized){
    Serial.println("playFallingShepardTone: I2S not initialized, skipping");
    return;
  }
  
  const float TAU = 6.28318530718f;
  const float VOLUME = 0.3f; // Moderate volume
  const float BASE_FREQ = 220.0f; // A3
  const float OCTAVE_RATIO = 2.0f; // Each octave doubles frequency
  
  // Shepard's tone uses multiple octaves with amplitude envelopes
  // As lower octaves fade out, higher octaves fade in, creating continuous falling illusion
  const int NUM_OCTAVES = 4; // Use 4 octaves for smooth illusion
  
  uint32_t samplesNeeded = (uint32_t)((SR * durationMs) / 1000);
  uint32_t samplesPlayed = 0;
  int16_t buf[BUF_SAMPLES*2];
  
  while(samplesPlayed < samplesNeeded){
    size_t samplesThisBlock = (samplesNeeded - samplesPlayed < BUF_SAMPLES) ? 
                              (samplesNeeded - samplesPlayed) : BUF_SAMPLES;
    
    // Clear buffer
    for(size_t i=0; i<BUF_SAMPLES; i++){
      buf[2*i+0] = 0;
      buf[2*i+1] = 0;
    }
    
    for(size_t i=0; i<samplesThisBlock; i++){
      uint32_t currentSample = samplesPlayed + i;
      float progress = (float)currentSample / samplesNeeded;
      
      float sampleL = 0.0f;
      float sampleR = 0.0f;
      
      // Generate multiple octaves with amplitude envelopes
      for(int octave = 0; octave < NUM_OCTAVES; octave++){
        // Calculate frequency for this octave (falling)
        float octaveFreq = BASE_FREQ * powf(OCTAVE_RATIO, (float)octave);
        
        // Phase advances, creating falling pitch illusion
        // The phase wraps around, but amplitude envelopes create the illusion
        float phaseOffset = progress * TAU * NUM_OCTAVES; // Continuous phase shift
        float phase = TAU * octaveFreq * currentSample / SR + phaseOffset;
        
        // Amplitude envelope: bell curve centered on each octave
        // Lower octaves fade out as we progress, higher octaves fade in
        float envelopePos = fmodf(progress * NUM_OCTAVES + octave, NUM_OCTAVES);
        float envelope;
        if(envelopePos < 1.0f){
          // Fade in
          envelope = envelopePos;
        } else if(envelopePos < 2.0f){
          // Full volume
          envelope = 1.0f;
        } else {
          // Fade out
          envelope = 1.0f - (envelopePos - 2.0f);
        }
        if(envelope < 0.0f) envelope = 0.0f;
        if(envelope > 1.0f) envelope = 1.0f;
        
        // Apply bell curve for smoother transitions
        envelope = sinf(envelope * M_PI); // Sine envelope for smooth bell curve
        
        // Generate tone with harmonics for richer sound
        float tone = sinf(phase) * 0.6f;
        tone += sinf(phase * 2.0f) * 0.3f; // Octave harmonic
        tone += sinf(phase * 3.0f) * 0.1f; // Fifth harmonic
        
        tone *= envelope * VOLUME / NUM_OCTAVES; // Normalize by number of octaves
        
        sampleL += tone;
        sampleR += tone;
      }
      
      // Apply smooth envelope to overall output
      float overallEnvelope = 1.0f;
      if(progress < 0.1f){
        overallEnvelope = progress / 0.1f; // Fade in
      } else if(progress > 0.9f){
        overallEnvelope = (1.0f - progress) / 0.1f; // Fade out
      }
      
      sampleL *= overallEnvelope;
      sampleR *= overallEnvelope;
      
      int16_t s16L = (int16_t)(sampleL * 16383.0f);
      int16_t s16R = (int16_t)(sampleR * 16383.0f);
      buf[2*i+0] = s16L;
      buf[2*i+1] = s16R;
    }
    
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    if(err == ESP_OK && written > 0){
      samplesPlayed += samplesThisBlock;
    } else {
      Serial.printf("[I2S] ERROR: i2s_write failed: err=%d, written=%d\n", err, written);
      break;
    }
    delay(1);
  }
}

// Diagnostic: Send continuous test pattern to verify I2S is working
void audioDiagnosticTest(){
  if(!i2s_initialized){
    Serial.println("[I2S] ERROR: Cannot run diagnostic - I2S not initialized");
    return;
  }
  
  Serial.println("[I2S] ===== DIAGNOSTIC TEST MODE =====");
  Serial.println("[I2S] Sending continuous 440Hz sine wave...");
  Serial.println("[I2S] Press reset to exit");
  
  const float TAU = 6.28318530718f;
  float phase = 0.0f;
  float phaseInc = TAU * 440.0f / SR; // 440Hz tone
  int16_t buf[BUF_SAMPLES*2];
  
  uint32_t startTime = millis();
  uint32_t lastStatus = 0;
  
  while(true){
    // Generate sine wave
    for(size_t i=0; i<BUF_SAMPLES; i++){
      float sample = sinf(phase) * 0.8f;
      int16_t s16 = (int16_t)(sample * 16383.0f);
      buf[2*i+0] = s16; // Left channel
      buf[2*i+1] = s16; // Right channel
      phase += phaseInc;
      if(phase >= TAU) phase -= TAU;
    }
    
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    if(err != ESP_OK || written != sizeof(buf)){
      Serial.printf("[I2S] ERROR: Write failed: err=%d, written=%d/%d\n", err, written, sizeof(buf));
    }
    
    // Status update every 2 seconds
    uint32_t now = millis();
    if(now - lastStatus > 2000){
      lastStatus = now;
      Serial.printf("[I2S] Diagnostic running: %lu seconds, writes: OK\n", (now - startTime) / 1000);
    }
    
    delay(1);
  }
}

// Frequency-dependent gain compensation for equal loudness
// Human hearing is more sensitive to mid frequencies (1-4 kHz), so we boost lows and reduce mids slightly
static float freqCompensation(float freqHz){
  if(freqHz <= 0) return 1.0f;
  // Do not apply sub-bass loudness boost to glide tails / near-DC carriers (was +40% and read as random volume jumps).
  const float fApply = fmaxf(freqHz, 55.f);

  // Simplified equal-loudness compensation curve
  // Boost low frequencies (< 500 Hz), reduce mid frequencies (1-4 kHz), keep highs similar
  if(fApply < 200.0f){
    // Very low: boost significantly
    return 1.0f + (200.0f - fApply) / 200.0f * 0.4f; // Up to +40% boost at very low frequencies
  } else if(fApply < 500.0f){
    // Low: moderate boost
    return 1.0f + (500.0f - fApply) / 300.0f * 0.25f; // Up to +25% boost
  } else if(fApply < 1000.0f){
    // Low-mid: slight boost
    return 1.0f + (1000.0f - fApply) / 500.0f * 0.1f; // Up to +10% boost
  } else if(fApply < 3000.0f){
    // Mid: reduce slightly (most sensitive range)
    return 1.0f - (fApply - 1000.0f) / 2000.0f * 0.15f; // Up to -15% reduction
  } else if(fApply < 5000.0f){
    // High-mid: slight reduction
    return 0.85f + (5000.0f - fApply) / 2000.0f * 0.1f; // Gradual return to normal
  } else {
    // High: keep similar
    return 1.0f;
  }
}

void audioRender(float wantL, float wantR){
  float blockTime=(float)BUF_SAMPLES/(float)SR;
  float a=expf(-blockTime/FREQ_GLIDE_TAU_S), keep=a, add=1.f-a;
  const float TAU=6.28318530718f;
  float vibInc = TAU*VIB_RATE_HZ/SR;
  int16_t buf[BUF_SAMPLES*2];
  
  // Clear buffer to prevent clicks from leftover data
  memset(buf, 0, sizeof(buf));
  
  static float prevWantL = 0.0f, prevWantR = 0.0f;
  static float smoothedAmpL = 0.0f, smoothedAmpR = 0.0f;
  static float dcBlockL = 0.0f, dcBlockR = 0.0f; // DC blocking filter state

  // New note: follow requested frequency, not gliding curFreq (curFreq can sit >0 for a long exponential tail).
  if (prevWantL <= 0.f && wantL > 0.f) {
    phaseL = 0.0f;
    smoothedAmpL = 0.0f;
  }
  if (prevWantR <= 0.f && wantR > 0.f) {
    phaseR = 0.0f;
    smoothedAmpR = 0.0f;
  }
  prevWantL = wantL;
  prevWantR = wantR;

  for (size_t i=0;i<BUF_SAMPLES;i++){
    curFreqL = curFreqL*keep + wantL*add;
    curFreqR = curFreqR*keep + wantR*add;
    
    float tL = (wantL > 0.f) ? 1.f : 0.f;
    float tR = (wantR > 0.f) ? 1.f : 0.f;
    ampL += (tL-ampL)*(tL>ampL? AMP_ATTACK:AMP_RELEASE);
    ampR += (tR-ampR)*(tR>ampR? AMP_ATTACK:AMP_RELEASE);
    
    smoothedAmpL = smoothedAmpL*0.90f + ampL*0.10f;
    smoothedAmpR = smoothedAmpR*0.90f + ampR*0.10f;

    if (wantL <= 0.f && ampL < 0.002f) {
      ampL = 0.0f;
      smoothedAmpL = 0.0f;
      phaseL = 0.0f;
      dcBlockL = 0.0f;
    }
    if (wantR <= 0.f && ampR < 0.002f) {
      ampR = 0.0f;
      smoothedAmpR = 0.0f;
      phaseR = 0.0f;
      dcBlockR = 0.0f;
    }
    
    // Keep osc running through envelope release (want=0) while amp decays; only key target follows want.
    const float kAudible = 0.002f;
    bool soundL = (wantL > 0.f) || (curFreqL > 0.f && (ampL > kAudible || smoothedAmpL > kAudible));
    bool soundR = (wantR > 0.f) || (curFreqR > 0.f && (ampR > kAudible || smoothedAmpR > kAudible));

    float rL = (soundL && curFreqL > 0.f) ? semiRatio(VIB_DEPTH_SEMITONES*sinf(vibPhaseL)) : 1.f;
    float rR = (soundR && curFreqR > 0.f) ? semiRatio(VIB_DEPTH_SEMITONES*sinf(vibPhaseR)) : 1.f;
    float dphiL = (soundL && curFreqL > 0.f) ? TAU*(curFreqL*rL)/SR : 0.f;
    float dphiR = (soundR && curFreqR > 0.f) ? TAU*(curFreqR*rR)/SR : 0.f;
    
    // Apply frequency-dependent gain compensation for equal loudness
    // Smooth the compensation to prevent clicks when frequency changes
    float compFreqL = soundL ? fmaxf(curFreqL, 55.f) : 0.f;
    float compFreqR = soundR ? fmaxf(curFreqR, 55.f) : 0.f;
    float targetFreqCompL = freqCompensation(compFreqL);
    float targetFreqCompR = freqCompensation(compFreqR);
    // Faster smoothing for more responsive compensation (reduces clicks)
    curFreqCompL = curFreqCompL*0.85f + targetFreqCompL*0.15f; // Even faster transition
    curFreqCompR = curFreqCompR*0.85f + targetFreqCompR*0.15f;
    
    float sL = (soundL && curFreqL > 0.f ? audioOscSample(phaseL) : 0.f) * (32767.f * MASTER_VOL * GAIN_L * smoothedAmpL * curFreqCompL);
    float sR = (soundR && curFreqR > 0.f ? audioOscSample(phaseR) : 0.f) * (32767.f * MASTER_VOL * GAIN_R * smoothedAmpR * curFreqCompR);
    
    // DC blocking filter to prevent clicks from DC offset
    // Simple high-pass filter: y[n] = x[n] - x[n-1] + 0.995*y[n-1]
    float prevSL = sL;
    float prevSR = sR;
    sL = sL - dcBlockL;
    sR = sR - dcBlockR;
    dcBlockL = dcBlockL * 0.995f + prevSL * 0.005f;
    dcBlockR = dcBlockR * 0.995f + prevSR * 0.005f;
    
    if (wantL <= 0.f && smoothedAmpL < 0.002f)
      dcBlockL = 0.0f;
    if (wantR <= 0.f && smoothedAmpR < 0.002f)
      dcBlockR = 0.0f;

#if SWAP_I2S_LR
    buf[2*i+0] = (int16_t)fmaxf(-32767.f,fminf(32767.f,sL));
    buf[2*i+1] = (int16_t)fmaxf(-32767.f,fminf(32767.f,sR));
#else
    buf[2*i+0] = (int16_t)fmaxf(-32767.f,fminf(32767.f,sR));
    buf[2*i+1] = (int16_t)fmaxf(-32767.f,fminf(32767.f,sL));
#endif
    phaseL += dphiL; if (phaseL>TAU) phaseL-=TAU;
    phaseR += dphiR; if (phaseR>TAU) phaseR-=TAU;
    vibPhaseL += vibInc; if (vibPhaseL>TAU) vibPhaseL-=TAU;
    vibPhaseR += vibInc; if (vibPhaseR>TAU) vibPhaseR-=TAU;
  }

  if(i2s_initialized){
    size_t w=0; 
    esp_err_t err=i2s_write(I2S_NUM_0, buf, sizeof(buf), &w, pdMS_TO_TICKS(100));
    if (err!=ESP_OK || w!=sizeof(buf)){
      if (++i2s_consec_errors>=3 || (millis()-lastGoodI2S>1000)){
        Serial.printf("[I2S ERROR] err=%d, written=%d/%d, consec_errors=%d\n", err, w, sizeof(buf), i2s_consec_errors);
        i2s_consec_errors=0; 
        i2s_stop(I2S_NUM_0); 
        delay(10);
        i2s_zero_dma_buffer(I2S_NUM_0); 
        i2s_start(I2S_NUM_0);
        lastGoodI2S=millis();
      }
    } else { 
      i2s_consec_errors=0; 
      lastGoodI2S=millis(); 
    }
  } else {
    memset(buf, 0, sizeof(buf));
  }
}

// Print I2S diagnostic information
void audioPrintDiagnostics(){
  Serial.println("\n========== I2S DIAGNOSTICS ==========");
  Serial.printf("Initialized: %s\n", i2s_initialized ? "YES" : "NO");
  
  if(i2s_initialized){
    Serial.printf("Pins: BCLK=GPIO%d, LRCLK=GPIO%d, DATA=GPIO%d\n", I2S_BCLK, I2S_LRCK, I2S_DATA);
    Serial.printf("Sample Rate: %d Hz\n", SR);
    Serial.printf("Buffer: %d samples, %d DMA buffers\n", BUF_SAMPLES, DMA_COUNT);
    Serial.printf("Buffer Size: %d bytes per write\n", BUF_SAMPLES * 2 * sizeof(int16_t));
    
    // Error statistics
    uint32_t now = millis();
    uint32_t timeSinceLastGood = (lastGoodI2S > 0) ? (now - lastGoodI2S) : 0;
    Serial.printf("Consecutive Errors: %d\n", i2s_consec_errors);
    Serial.printf("Time since last good write: %lu ms\n", timeSinceLastGood);
    
    if(timeSinceLastGood > 2000){
      Serial.printf("⚠ WARNING: No successful writes in %lu ms!\n", timeSinceLastGood);
      Serial.printf("  This indicates I2S may not be working properly.\n");
      Serial.printf("  Check: pin connections, DAC power, and pin configuration.\n");
    } else if(timeSinceLastGood < 100){
      Serial.printf("✓ I2S appears to be working (writes happening regularly)\n");
    }
    
    // Calculate expected write rate
    float expectedWriteInterval = ((float)BUF_SAMPLES / (float)SR) * 1000.0f;
    Serial.printf("Expected write interval: %.1f ms\n", expectedWriteInterval);
    
    // Check if we're actually generating audio
    Serial.printf("Current frequencies: L=%.1f Hz, R=%.1f Hz\n", curFreqL, curFreqR);
    Serial.printf("Current amplitudes: L=%.3f, R=%.3f\n", ampL, ampR);
    
    if(curFreqL == 0 && curFreqR == 0){
      Serial.printf("⚠ No audio being generated - press buttons to generate tones\n");
    }
    
    Serial.println("=====================================\n");
  } else {
    Serial.println("I2S NOT INITIALIZED - Check initialization errors above");
    Serial.println("Common issues:");
    Serial.println("  - Pin configuration error");
    Serial.println("  - GPIO pin not suitable for I2S");
    Serial.println("  - Driver installation failed");
    Serial.println("=====================================\n");
  }
}


#include "debug.h"
#include "config.h"
#include <esp_random.h>

DebugWatchVars dbgWatch;

void testButtonGPIOs(){
  Serial.println("\n=== Button GPIO Testing Mode ===");
  Serial.println("Pull button GPIOs to GND to test them");
  Serial.println("Button GPIOs: 38,12,5,7,16,46,39,2,15,8");
  Serial.println("Press reset to exit this mode\n");
  delay(500);
  
  Serial.println("Initializing button GPIOs...");
  for(int i=0; i<10; i++){
    int pin = BTN_PINS[i];
    Serial.printf("  Setting GPIO %d to INPUT_PULLUP... ", pin);
    pinMode(pin, INPUT_PULLUP);
    delay(10);
    int val = digitalRead(pin);
    Serial.printf("read=%s\n", val==HIGH?"HIGH":"LOW");
  }
  Serial.println("Button GPIOs initialized.\n");
  delay(500);
  
  bool lastState[50] = {0};
  uint32_t lastStatePrint = 0;
  
  Serial.println("Reading initial state of all GPIOs:");
  for(int i=0; i<10; i++){
    int pin = BTN_PINS[i];
    lastState[pin] = digitalRead(pin);
    Serial.printf("  GPIO %d: %s\n", pin, lastState[pin]==HIGH?"HIGH":"LOW");
  }
  Serial.println("\n=== Starting continuous monitoring ===");
  Serial.println("Pull GPIOs to GND to test them. Current state will be shown every second.\n");
  
  uint32_t loopCount = 0;
  while(true){
    loopCount++;
    uint32_t now = millis();
    
    if(now - lastStatePrint > 1000){
      lastStatePrint = now;
      Serial.print("State: ");
      for(int i=0; i<10; i++){
        int pin = BTN_PINS[i];
        int val = digitalRead(pin);
        Serial.printf("GPIO%d=%s ", pin, val==HIGH?"H":"L");
      }
      Serial.println();
    }
    
    for(int i=0; i<10; i++){
      int pin = BTN_PINS[i];
      int currentState = digitalRead(pin);
      
      if(lastState[pin] == HIGH && currentState == LOW){
        Serial.printf("\n>>> GPIO %d (index %d) WENT LOW! (pulled to GND) <<<\n", pin, i);
        lastState[pin] = LOW;
      }
      else if(lastState[pin] == LOW && currentState == HIGH){
        Serial.printf(">>> GPIO %d (index %d) WENT HIGH! (released from GND) <<<\n\n", pin, i);
        lastState[pin] = HIGH;
      }
    }
    delay(50);
  }
}

void identifyBreakoutPins(){
  Serial.println("\n=== Breakout Pin Identification Mode ===");
  Serial.println("Set up: Touch breakout pins (PCMfs/PCMdIn) to GND with a wire");
  Serial.println("The serial output will show which GPIO number each pin is");
  Serial.println("Press reset to exit this mode\n");
  delay(100);
  
  // Pins we try to configure as INPUT_PULLUP and monitor. (Many S3 GPIOs are intentionally
  // NOT listed here — see skip block below: strapping, USB, flash/PSRAM bus, etc.)
  //
  // Note: The old list included 22–34 etc. but those were all skipped during init, so only
  // ~7 pins ever monitored. This list is trimmed to pins that actually init, plus input-only
  // 35–42 (ok for "short to GND" identification on a running chip).
  // 12 is included on purpose: it is the old SD CS / left blue key, and 21 is the
  // new SD CS. Identifying both terminals is what the CS rewire needs. Nothing
  // else runs in this mode, so probing 12 as an input is safe here.
  int testPins[] = {
      4,  6,  9,  10, 12, 13, 14, 21,
      35, 36, 37, 40, 41, 42, // 39 = button — skip below
  };
  int numPins = sizeof(testPins) / sizeof(testPins[0]);
  
  Serial.printf("Testing %d GPIO candidates for breakout mapping: ", numPins);
  for(int i=0; i<numPins; i++){
    Serial.printf("%d ", testPins[i]);
  }
  Serial.println("\n");
  delay(100);
  
  Serial.println("Initializing pins...");
  Serial.println("Note: Skips I2C (17,18), current BTN_PINS, strapping, USB, flash/PSRAM block");
  
  // Track which pins successfully initialized
  bool pinInitialized[50] = {false};
  
  for(int i=0; i<numPins; i++){
    int pin = testPins[i];
    
    Serial.printf("  Checking GPIO %d... ", pin);
    
    // Skip I2C and button pins
    if(pin == 17 || pin == 18 || // I2C
       pin == 38 || pin == 5 || pin == 7 || pin == 16 || pin == 46 ||
       pin == 39 || pin == 2 || pin == 15 || pin == 8){ // Buttons (12 probed on purpose)
      Serial.printf("SKIP (I2C or button)\n");
      continue;
    }
    
    // Skip known problematic pins FIRST (before any pinMode call)
    // ESP32-S3 restrictions:
    // - GPIO 0, 1, 3, 45, 46: Strapping pins
    // - GPIO 19-20: USB/JTAG
    // - GPIO 22, 26-34: SPI flash/PSRAM / octal SPI (do not mux in identification mode)
    // - GPIO 23, 25, 26: often special / overlap with above
    // - GPIO 43-48: Strapping / chip-specific (skip for safety)
    // 35-42: full input+output on ESP32-S3 (input-only 34-39 is an ESP32-CLASSIC
  // property and does not apply here). 35-37 are still reserved on octal-PSRAM
  // modules; 40/41/42 are free and are where SD CS now lives.
    if(pin == 0 || pin == 1 || pin == 3 || // Strapping pins
       pin == 19 || pin == 20 || // USB/JTAG
       pin == 22 || // Problematic
       pin == 23 || pin == 25 || pin == 26 || // I2S pins (may be strapping)
       (pin >= 26 && pin <= 34) || // SPI flash/PSRAM range (26-32) + problematic (33-34)
       pin == 45 || pin == 46 || // Strapping pins
       (pin >= 43 && pin <= 48)){ // Strapping pins
      Serial.printf("SKIP (strapping/USB/SPI-flash/problematic)\n");
      Serial.flush();
      yield();
      continue;
    }
    
    // Validate pin number is in valid range
    if(pin < 0 || pin > 48){
      Serial.printf("SKIP (out of range)\n");
      yield();
      continue;
    }
    
    // Try to initialize
    Serial.printf("INIT... ");
    Serial.flush();
    yield();
    pinMode(pin, INPUT_PULLUP);
    delay(10);
    yield();
    Serial.printf("OK\n");
    Serial.flush();
    pinInitialized[pin] = true; // Mark as successfully initialized
    yield();
  }
  
  Serial.flush();
  delay(50);
  Serial.println("Pins initialized. Starting monitoring...");
  Serial.flush();
  delay(50);
  Serial.println("Touch PCMfs or PCMdin pins on the breakout board to GND to identify them.");
  Serial.println("The GPIO number will be displayed when detected.");
  Serial.flush();
  delay(200);
  
  bool lastState[50];
  uint32_t lastPrintTime[50];
  for(int i=0; i<50; i++){
    lastState[i] = true;
    lastPrintTime[i] = 0;
  }
  
  Serial.println("Monitoring pins... Touch a pin to GND to identify it:");
  Serial.println("---------------------------------------------------");
  Serial.flush();
  delay(200);
  
  uint32_t loopCount = 0;
  while(true){
    loopCount++;
    uint32_t now = millis();
    
    // Feed watchdog frequently to prevent timeouts
    yield(); // Always yield to feed watchdog and give other tasks time
    
    // Periodic status message
    if(loopCount % 500 == 0){
      Serial.println("(Still monitoring... touch pins to GND)");
      Serial.flush();
      yield(); // Feed watchdog after serial output
    }
    
    // Read pins with error handling - only read pins that successfully initialized
    for(int i=0; i<numPins; i++){
      int pin = testPins[i];
      
      // Only monitor pins that were successfully initialized
      if(!pinInitialized[pin]){
        continue;
      }
      
      // Read pin state
      bool currentState = digitalRead(pin);
      
      // Check for state changes
      if(lastState[pin] == HIGH && currentState == LOW){
        if(now - lastPrintTime[pin] > 200){
          Serial.printf(">>> GPIO %d detected! (touched to GND) <<<\n", pin);
          Serial.printf("    This pin is currently LOW\n");
          Serial.printf("    Release it to see it go HIGH again\n\n");
          Serial.flush();
          lastPrintTime[pin] = now;
          yield(); // Feed watchdog after detection
        }
      }
      else if(lastState[pin] == LOW && currentState == HIGH){
        if(now - lastPrintTime[pin] > 200){
          Serial.printf("GPIO %d released (now HIGH)\n", pin);
          Serial.flush();
          lastPrintTime[pin] = now;
          yield(); // Feed watchdog after release message
        }
      }
      
      lastState[pin] = currentState;
      
      // Feed watchdog every few pins to prevent timeout
      if(i % 3 == 0){
        yield();
      }
    }
    
    delay(10);
    yield(); // Feed watchdog after each loop iteration
  }
}


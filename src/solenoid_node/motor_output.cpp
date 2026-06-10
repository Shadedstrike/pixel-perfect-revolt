#include "motor_output.h"
#include "motor_config.h"

#include <Arduino.h>
#include <driver/gpio.h>

struct MotorPair {
  ActuatorColor color;
  uint8_t in1;
  uint8_t in2;
};

static const MotorPair kMotors[] = {
    {ACTUATOR_COLOR_RED, (uint8_t)MOTOR_RED_IN1, (uint8_t)MOTOR_RED_IN2},
    {ACTUATOR_COLOR_GREEN, (uint8_t)MOTOR_GREEN_IN1, (uint8_t)MOTOR_GREEN_IN2},
    {ACTUATOR_COLOR_YELLOW, (uint8_t)MOTOR_YELLOW_IN1, (uint8_t)MOTOR_YELLOW_IN2},
};

static const uint8_t kAllDirectionPins[] = {
    MOTOR_RED_IN1,    MOTOR_RED_IN2,    MOTOR_GREEN_IN1, MOTOR_GREEN_IN2,
    MOTOR_YELLOW_IN1, MOTOR_YELLOW_IN2,
};

static bool s_stbyEnabled = false;

static void forcePinOutput(uint8_t pin, int level) {
  gpio_reset_pin((gpio_num_t)pin);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)pin, level);
  gpio_pullup_dis((gpio_num_t)pin);
  gpio_pulldown_dis((gpio_num_t)pin);
}

static void forcePinLow(uint8_t pin) { forcePinOutput(pin, 0); }

static void stbyDrive(bool enable) {
#if MOTOR_STBY_PIN >= 0
  forcePinOutput((uint8_t)MOTOR_STBY_PIN, enable ? 1 : 0);
  s_stbyEnabled = enable;
#endif
}

static int stbyReadback() {
#if MOTOR_STBY_PIN >= 0
  return gpio_get_level((gpio_num_t)MOTOR_STBY_PIN);
#else
  return -1;
#endif
}

static void drivePair(uint8_t in1, uint8_t in2, bool on) {
  if (on) {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, LOW);
  } else {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, HIGH);
  }
}

static const MotorPair *findMotor(ActuatorColor color) {
  for (const MotorPair &m : kMotors) {
    if (m.color == color)
      return &m;
  }
  return nullptr;
}

void motorOutputEarlyInit() {
#if MOTOR_STBY_PIN >= 0
  stbyDrive(false);
#endif
  for (uint8_t pin : kAllDirectionPins)
    forcePinLow(pin);
}

bool motorOutputBegin() {
  motorOutputEarlyInit();

  for (const MotorPair &m : kMotors) {
    pinMode(m.in1, OUTPUT);
    pinMode(m.in2, OUTPUT);
    drivePair(m.in1, m.in2, false);
  }

#if MOTOR_STBY_PIN >= 0
  stbyDrive(true);
  Serial.printf("[MOT] STBY=GPIO%d  boot:0V->enable  readback=%d (meter vs GND: expect ~3.3V now)\n",
                (int)MOTOR_STBY_PIN, stbyReadback());
#else
  Serial.println("[MOT] ERROR: MOTOR_STBY_PIN not set — use GPIO 13, 15, or 46");
#endif
  Serial.printf("[MOT] direction pins ready  red=%d/%d green=%d/%d yellow=%d/%d\n", (int)MOTOR_RED_IN1,
                (int)MOTOR_RED_IN2, (int)MOTOR_GREEN_IN1, (int)MOTOR_GREEN_IN2, (int)MOTOR_YELLOW_IN1,
                (int)MOTOR_YELLOW_IN2);
  return true;
}

bool motorOutputHasMotor(ActuatorColor color) { return findMotor(color) != nullptr; }

void motorOutputSetColor(ActuatorColor color, bool on) {
  const MotorPair *m = findMotor(color);
  if (!m)
    return;
#if MOTOR_STBY_PIN >= 0
  if (!s_stbyEnabled)
    stbyDrive(true);
#endif
  drivePair(m->in1, m->in2, on);
}

void motorOutputAllOff() {
  for (const MotorPair &m : kMotors)
    drivePair(m.in1, m.in2, false);
}

bool motorOutputStbyEnabled() { return s_stbyEnabled; }

int motorOutputStbyLevel() { return stbyReadback(); }

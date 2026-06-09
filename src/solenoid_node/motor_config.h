#ifndef SOLENOID_NODE_MOTOR_CONFIG_H
#define SOLENOID_NODE_MOTOR_CONFIG_H

// Telesyn / TB6612-style dual H-bridge motor drivers.
//
// Controller 1: red=AIN(42,11)  green=BIN(40,41)
// Controller 2: yellow=BIN(7,10)
//
// PWM: tie PWMA + PWMB on both boards to 3.3V (full speed).
// STBY: wire BOTH driver STBY pins to MOTOR_STBY_PIN (NOT 3.3V).
//   LOW at boot = drivers off. HIGH after init = drivers on.
//
// Safe STBY GPIO choices on actuator node: 13 (default), 15, 46
// Do NOT use 4 or 5 — those are DMX UART (see dmx_config.h).
// Override: -DMOTOR_STBY_PIN=15 in platformio.ini build_flags.

#ifndef MOTOR_STBY_PIN
#define MOTOR_STBY_PIN 13
#endif

#ifndef MOTOR_RED_IN1
#define MOTOR_RED_IN1 42
#endif
#ifndef MOTOR_RED_IN2
#define MOTOR_RED_IN2 11
#endif

#ifndef MOTOR_GREEN_IN1
#define MOTOR_GREEN_IN1 40
#endif
#ifndef MOTOR_GREEN_IN2
#define MOTOR_GREEN_IN2 41
#endif

#ifndef MOTOR_YELLOW_IN1
#define MOTOR_YELLOW_IN1 7
#endif
#ifndef MOTOR_YELLOW_IN2
#define MOTOR_YELLOW_IN2 10
#endif

#endif

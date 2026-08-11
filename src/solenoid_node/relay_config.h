#ifndef SOLENOID_NODE_RELAY_CONFIG_H
#define SOLENOID_NODE_RELAY_CONFIG_H

// 5V opto-isolated relay module, driven by any color button hold.
//
//   IN   -> RELAY_PIN (default GPIO 16)
//   VCC  -> 5V   (module side, NOT the ESP32 3.3V rail)
//   GND  -> common ground with the ESP32
//
// ALWAYS drive push-pull (RELAY_OPEN_DRAIN=0). On this board the input side is
// 5V ──[R]──►|LED──── IN, so IN is pulled to the module's 5V rail whenever it is
// released. High-Z would therefore put 5V on the GPIO, and ESP32-S3 pads clamp
// at VDD+0.3 (~3.6V) — they are not 5V tolerant. Actively holding the pin at
// 0V/3.3V means it is never released and never sees 5V.
//
// Jumper on H, RELAY_ACTIVE_LOW=0. H triggers ~1.5V above ground, so 3.3V logic
// has margin, and a floating pin (boot, reset, ESP unpowered) reads LOW = relay
// de-energized, which is the safe state.
//
// HARD REQUIREMENT: the module's DC- must be bonded to ESP GND by its own short
// wire. The module compares IN against its OWN DC-, so without a solid shared
// reference the GPIO level means nothing at the module — and solenoid current
// through a shared ground return shifts that reference enough to fire the relay
// on its own. That was a real fault here: the relay tracked solenoid switching
// instead of GPIO16, and probing GND-to-DC- with a meter fired it.
//
// Keep the module's ground return OFF the solenoid return path (star ground).
// Ground bounce from the solenoids can otherwise energize the relay uncommanded.
//
// Note: earlier bench notes claimed H could not source enough current at 3.3V.
// Those measurements were taken before the ground fault was found and are not
// reliable. These modules are routinely driven from 3.3V (e.g. WLED).
//
// Free GPIO on this node (everything else is DMX 4/5/21, I2C 17/18,
// motors 7/10/11/40/41/42, STBY 13, USB 19/20, UART0 43/44, flash 26-32):
//   1, 2, 6, 8, 9, 12, 14, 15, 16, 39, 47
// Do NOT use 0/3/45/46 — strapping pins, wrong level at boot fires the relay.
// GPIO 15 is reserved as the documented MOTOR_STBY_PIN fallback.
//
// Boot: pin is parked inactive before it is switched to an output, so the relay
// never clicks during reset. Override: -DRELAY_PIN=14 in platformio.ini.

#ifndef RELAY_PIN
#define RELAY_PIN 16
#endif

// 0 = drive HIGH to close, matching the module jumper on H (the wired config).
// Set to 1 only if the jumper moves to L. Adding an NPN/FET between the GPIO and
// IN also needs 0, since that stage inverts.
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 0
#endif

// Leave at 0. See the 5V-tolerance note above before ever setting this to 1.
#ifndef RELAY_OPEN_DRAIN
#define RELAY_OPEN_DRAIN 0
#endif

// Boot self-test: drives the relay directly, before ESP-NOW/solenoids/DMX exist.
// Isolates the GPIO -> module path from everything else. 0 disables.
#ifndef RELAY_SELFTEST_CYCLES
#define RELAY_SELFTEST_CYCLES 3
#endif
#ifndef RELAY_SELFTEST_MS
#define RELAY_SELFTEST_MS 700
#endif

#endif

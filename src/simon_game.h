#ifndef SIMON_GAME_H
#define SIMON_GAME_H

#include <Arduino.h>

// "Follow the Leader" — Simon-says memory game.
//
// Enter: both BLUE keys (GPIO 42 + 2) held 5 s with both yellows UP. The yellow-up
// requirement matters: the blue pair is a subset of the rhythm-mode enter gesture
// (38 + 42 + 2 + 39), so without it both countdowns would race.
//
// Key pool grows with the round — see simonGamePoolSize():
//   rounds 1-3   4 keys  left side only
//   rounds 4-7   8 keys  both sides
//   rounds 8+   10 keys  both sides + both front keys
//
// During playback the actuator fires for each step (relay, solenoid, DMX), so the
// machine performs the sequence rather than just blinking it. Player presses fire
// the actuator through the normal inputFastPoll path.

void simonGameInit();

// Drive from loop(). Consumes latched edges; safe to call every iteration.
void simonGameLoop(uint32_t now, const bool *down, const bool *edgeDown);

bool simonGameIsActive();
// True while active OR while the enter countdown is showing — main.cpp uses this to
// keep the synth UI and normal LED render off the panel.
bool simonGameSuppressNormalUi();

// Returns true if it drew; leaves the LCD alone otherwise.
bool simonGameDrawLcd(uint32_t now);
// Owns all 10 LEDs while active.
void simonGameRenderLeds(uint32_t now);
// Overrides the synth targets so sequence steps are audible.
void simonGameAudioTargets(float &wantL, float &wantR);

bool simonGameEnterCountdownActive();
uint32_t simonGameEnterHeldMs();
uint32_t simonGameEnterTotalMs();

#endif // SIMON_GAME_H

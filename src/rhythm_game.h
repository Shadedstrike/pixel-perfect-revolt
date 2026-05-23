#ifndef RHYTHM_GAME_H
#define RHYTHM_GAME_H

#include <Arduino.h>

void rhythmGameSetup();
// Call after debounced button scan each frame.
void rhythmGameLoop(uint32_t now, const bool *down, const bool *edgeDown);
// Any side/front button edge-down while playing registers a tap.
void rhythmGameOnButtonEdge(uint32_t now);

bool rhythmGameIsActive();
// True in all rhythm phases — main-loop button synth (audioRender) must stay off.
bool rhythmGameShouldSilenceSynth();
// When true, skip scale-switch / idle-cycle / normal LCD stack (game draws LCD).
bool rhythmGameSuppressNormalUi();

// Draw retro UI; returns true if game owns the LCD this frame.
bool rhythmGameDrawLcd(uint32_t now);

// MP3 decode pump — call instead of audioRender for the whole rhythm game (menu/play/results).
void rhythmGameAudioPump();
// True during all rhythm phases: blocks main synthesizer mode (audioRender) — rhythm uses MP3 only.
bool rhythmGameOwnsAudioOutput();
// Live beat/score hue on 16/46 — not during results/menu (use results RGB there).
bool rhythmGameFrontBeatLedsActive();
bool rhythmGameIsPaused();

// Live 0–100 from last ~4s of beats only (red=poor … green=great); full song % at results.
int rhythmGameLiveScorePct(uint32_t songRelMs);
// GPIO 16 / 46: hue = rolling 4s score; brightness pulses sharply on each beat.
void rhythmGameGetFrontPlayingLeds(uint32_t now, uint8_t &rL, uint8_t &gL, uint8_t &bL, uint8_t &rR, uint8_t &gR, uint8_t &bR);

// Left/right columns (4 keys each): audio-reactive viz from MP3 analysis; cosmetic only vs scoring grid.
void rhythmGameGetSideColumnMusicLeds(uint32_t wallMs, uint8_t musicL[4][3], uint8_t musicR[4][3]);

// After a song: score-based ambient (pulsing red..yellow → steady green when excellent).
bool rhythmGameResultsAmbientLedsActive();
void rhythmGameGetResultsMoodRgb(uint32_t now, uint8_t &r, uint8_t &g, uint8_t &b);
// Front 16/46 on results: same score hue as mood strip but steady (no sine pulse — avoids "flashing yellow").
void rhythmGameGetResultsFrontRgb(uint8_t &r, uint8_t &g, uint8_t &b);

#endif

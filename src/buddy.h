#pragma once
#include <stdint.h>
// TFT_eSPI is a LovyanGFX alias in this port — `class TFT_eSPI;` forward
// declarations clash with the typedef, so pull in the shim header
// directly. Cheap because every .cpp that uses buddy.h includes it anyway.
#include <M5StickCPlus.h>

// Multi-species ASCII buddy renderer. Each species lives in its own
// src/buddies/<name>.cpp file and exposes 7 state functions matching
// the PersonaState enum order: sleep, idle, busy, attention, celebrate,
// dizzy, heart.
void buddyInit();
void buddyTick(uint8_t personaState);
void buddyInvalidate();
void buddyRenderTo(TFT_eSPI* tgt, uint8_t personaState);
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
void buddySetPeek(bool peek);
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

// Shift every subsequent buddy draw by (dx, dy) px. Reset by calling
// with (0, 0). Used by the settings-menu mini preview to render the
// ASCII species inside the thumbnail box without touching the species'
// hardcoded BUDDY_X_CENTER / BUDDY_Y_BASE coordinates.
void buddyShift(int dx, int dy);

// Per-species state function: takes the global tickCount and renders
// the buddy + any overlays for the current state into the shared sprite.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // index by PersonaState (0=sleep .. 6=heart)
};

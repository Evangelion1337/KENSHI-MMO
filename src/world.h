#pragma once

#include <cstdint>

namespace kmmo {
struct Discovery;
struct GameOffsets;

struct Vec3 {
    float x, y, z;
};

namespace game {

// Find the address of the live GameWorld object by dereferencing the
// discovered singleton global (validates it's still a heap object).
uintptr_t GetGameWorld(const Discovery& d);

// Find the player's controlled character pointer (or any valid player char).
// Writes found ptr to outPtr. Returns true if found.
bool FindFirstCharacter(uintptr_t world, uintptr_t& outPtr);

// Find any loaded character whose name matches (track remote players).
bool FindCharacterByName(uintptr_t world, const char* name, uintptr_t& outPtr);

// Set an additional player-name to accept beyond the default "Nameless_0"
// (used when the server tells us which squad this account owns).
void SetPreferredPlayerName(const char* name);

// Name currently accepted as the player character (override or default).
const char* PlayerName();

// Walk the character list; returns the array pointer (or 0).
uintptr_t CharacterListArray(const GameOffsets& o);

// Read a character's cached world position.
bool GetCharacterPosition(uintptr_t charPtr, Vec3& pos);

// Read/write the Ogre quaternion at char+0x58 (w,x,y,z floats).
bool GetCharacterRotation(uintptr_t charPtr, float rot[4]);
bool WriteCharacterRotation(uintptr_t charPtr, const float rot[4]);

// Read the character's display name ('' on failure).
bool GetCharacterName(uintptr_t charPtr, char* out, int cap);

// Write a character's position using guarded memory writes only
// (cached field + heuristic physics chain). Does NOT call game code.
bool WriteCharacterPosition(uintptr_t charPtr, const Vec3& pos);

// Enumerate characters from the live world; returns count scanned.
int CountCharacters(uintptr_t world);

// Fill `out` (up to `cap`) with live character pointers from the character
// pool, ordered by pool slot. Returns the number filled. Used by the session
// to broadcast the whole visible world (shared world startup: both clients
// load the same save, so every character exists on both sides).
int ListCharacters(uintptr_t world, uintptr_t* out, int cap);

// Read a single body-part health float via the Kenshi-Online CE-verified
// chain (char+0x2B8 -> +0x5F8 -> +0x40 + stride*part), Head=0/Chest=1.
// Guarded reads; returns false if the chain isn't readable yet.
bool GetCharacterHealth(uintptr_t charPtr, int part, float& out);

// Force a body part's health downward to `value` (only if currently higher).
// Used to apply remote combat events (KO/death) without calling game code.
bool WriteCharacterHealth(uintptr_t charPtr, int part, float value);

// Coarse combat state: 0=alive, 1=down/KO, 2=dead (head/chest <= -100).
int CharacterCombatState(uintptr_t charPtr);

} // namespace game
} // namespace kmmo
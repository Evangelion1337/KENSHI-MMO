#include "world.h"

#include "mem.h"
#include "discovery.h"
#include "log.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cstddef>
#include <initializer_list>

namespace kmmo {
namespace game {

namespace {

const int kPosOffset = 0x48;
const int kNameOffset = 0x18;
const int kMaxChars = 50000;
const int kSentinelRunStop = 4096; // stop after this many empty/sentinel slots

// RVA of the *character* pool global (observed live on GOG 1.0.68).
// Its .data address is: read -> heap array of Character* (the loaded world
// population). The player character is the 'Nameless_0' entry (their squad
// name in the default Wanderer start).
const uintptr_t kCharPoolGlobalRva = 0x1FFDCB0;
// Character vtable RVA, observed live in-game on this build (0x141717868).
const uintptr_t kCharVtableRva = 0x1717868;

// Legacy (wrong-target) world-object pool global: holds *item/container*
// inventories of the loaded town, NOT characters. Kept only as fallback.
const uintptr_t kItemPoolGlobalRva = 0x2133FE8;

// Search range for .data (to rediscover the pool if the RVA shifts).
const uintptr_t kDataRva = 0x1BBE000;
const uintptr_t kDataSize = 0x58E000;

bool IsHeapLike(uintptr_t val, uintptr_t modBase, uintptr_t modEnd) {
    if (val < 0x10000 || val >= 0x00007FFFFFFFFFFFULL) return false;
    if (val >= modBase && val < modEnd) return false;
    return val % 8 == 0;
}

bool HasVtable(uintptr_t obj, uintptr_t modBase, uintptr_t modEnd) {
    uintptr_t vt = 0;
    if (!mem::Read(obj, vt)) return false;
    if (vt < modBase + 0x1000 || vt >= modEnd) return false;
    return true;
}

// Read the (possibly inline) std::string at obj+nameOff.
void ReadName(uintptr_t obj, char* out, int cap) {
    out[0] = '\0';
    if (!obj) return;
    uintptr_t hdr = 0;
    if (!mem::Read(obj + kNameOffset, hdr)) return;
    size_t size = 0;
    mem::Read(obj + kNameOffset + 0x10, size);
    if (size < 16) {
        // SSO: buffer inline at obj+nameOffset
        for (size_t i = 0; i < cap - 1 && i < 16; i++) {
            unsigned char c = 0;
            if (!mem::Read(obj + kNameOffset + i, c)) break;
            out[i] = (char)c;
            if (c == 0) break;
        }
        return;
    }
    // heap-backed string: first 8 bytes are the buffer pointer
    if (hdr < 0x10000 || hdr >= 0x00007FFFFFFFFFFFULL) return;
    for (size_t i = 0; i < cap - 1 && i < 256; i++) {
        unsigned char c = 0;
        if (!mem::Read(hdr + i, c)) break;
        out[i] = (char)c;
        if (c == 0) break;
    }
}

// A plausible loaded character: heap object, module vtable, readable name
// that's not the unloaded sentinel, and a sane world position.
bool IsPrintableName(const char* s) {
    int letters = 0;
    int len = 0;
    for (; len < 96 && s[len]; len++) {
        unsigned char c = (unsigned char)s[len];
        if (c >= 0x80) return false;          // non-ASCII -> garbage
        if (!isprint(c) && c != ' ') return false;
        if (isalpha((unsigned char)c)) letters++;
    }
    if (len < 2 || len > 64) return false;
    return letters >= 2;
}

uintptr_t Rva(uintptr_t base, uintptr_t rva) { return base + rva; }

char g_playerNameOverride[96] = {};
bool g_playerNameOverrideSet = false;

} // namespace

void SetPreferredPlayerName(const char* name) {
    if (!name) {
        g_playerNameOverrideSet = false;
        g_playerNameOverride[0] = '\0';
        return;
    }
    strncpy(g_playerNameOverride, name, sizeof(g_playerNameOverride) - 1);
    g_playerNameOverrideSet = true;
    log::Info("world: preferred player name set to '%s'", g_playerNameOverride);
}

const char* PlayerName() {
    return g_playerNameOverrideSet ? g_playerNameOverride : "Nameless_0";
}

uintptr_t GetGameWorld(const Discovery& d) {
    if (!d.Ready()) return 0;
    const GameOffsets& o = d.Result();
    uintptr_t modEnd = o.base + 0x232D000; // SizeOfImage observed at runtime
    if (!o.GameWorldSingleton) return 0;
    uintptr_t world = 0;
    if (!mem::Read(o.GameWorldSingleton, world)) return 0;
    if (!IsHeapLike(world, o.base, modEnd)) return 0;
    if (!HasVtable(world, o.base, modEnd)) return 0;
    return world;
}

int CountCharacters(uintptr_t world) {
    if (!world) return 0;
    const GameOffsets& o = Discovery::Get().Result();
    uintptr_t modEnd = o.base + 0x232D000;

    const int charListOff = 0x888;
    int count = 0;
    uintptr_t arrayPtr = 0;

    if (mem::Read(world + charListOff + 0x00, count) &&
        mem::Read(world + charListOff + 0x08, arrayPtr) &&
        count > 0 && count < kMaxChars &&
        IsHeapLike(arrayPtr, o.base, modEnd) && mem::IsReadable(arrayPtr, count * 8)) {
        return count;
    }
    if (mem::Read(world + charListOff + 0x00, arrayPtr) &&
        mem::Read(world + charListOff + 0x08, count) &&
        count > 0 && count < kMaxChars &&
        IsHeapLike(arrayPtr, o.base, modEnd) && mem::IsReadable(arrayPtr, count * 8)) {
        return count;
    }
    return 0;
}

// True if `name` matches the player's own squad/character name in the default
// Wanderer start (every MMO account creates one of these). An override set
// from the server (account-owned squad) takes priority.
bool IsPlayerName(const char* nm) {
    if (!nm[0]) return false;
    if (g_playerNameOverrideSet) return strcmp(nm, g_playerNameOverride) == 0;
    return strcmp(nm, "Nameless_0") == 0;
}

// Locate the character pool array. Prefers the observed RVA global; falls
// back to scanning .data for a global whose value is a heap array holding
// objects with the character vtable. The success log fires once.
uintptr_t FindCharacterPoolArray() {
    const GameOffsets& o = Discovery::Get().Result();
    if (!o.base) return 0;
    uintptr_t modEnd = o.base + 0x232D000;

    // Preferred: observed global
    uintptr_t poolGlobal = Rva(o.base, kCharPoolGlobalRva);
    uintptr_t arr = 0;
    static bool loggedArr = false;
    if (mem::Read(poolGlobal, arr) && IsHeapLike(arr, o.base, modEnd) &&
        mem::IsReadable(arr, 64)) {
        if (!loggedArr) {
            loggedArr = true;
            log::Info("world: charPool RVA global=0x%llX arr=0x%llX",
                      (unsigned long long)poolGlobal, (unsigned long long)arr);
        }
        return arr;
    }
    loggedArr = false;

    // Fallback: scan .data for a global->heap array of character objects
    for (uintptr_t g = Rva(o.base, kDataRva) + 8; g < Rva(o.base, kDataRva) + kDataSize; g += 8) {
        if (!mem::Read(g, arr) || !IsHeapLike(arr, o.base, modEnd)) continue;
        // cheap pre-check: first two readable vtable'd entries
        uintptr_t c0 = 0, c1 = 0, vt0 = 0, vt1 = 0;
        if (mem::Read(arr, c0) && mem::Read(arr + 8, c1) &&
            mem::Read(c0, vt0) && mem::Read(c1, vt1) &&
            vt0 == o.base + kCharVtableRva && vt1 == o.base + kCharVtableRva) {
            log::Info("world: char pool rediscovered at global 0x%llX arr 0x%llX",
                      (unsigned long long)g, (unsigned long long)arr);
            return arr;
        }
    }
    return 0;
}

bool FindFirstCharacter(uintptr_t world, uintptr_t& outPtr) {
    const GameOffsets& o = Discovery::Get().Result();
    uintptr_t modEnd = o.base + 0x232D000;
    (void)world;

    // Walk the *character* pool. We only accept the player's own wanderer
    // ('Nameless_0' - the default squad/member name of a vanilla Wanderer
    // start, which is the only start the server session expects). Any other
    // match would be an NPC or a manager object, so we deliberately return
    // false and let the caller keep polling until the world is actually
    // populated. The pool stays empty while the game is in the main menu.
    uintptr_t arr = FindCharacterPoolArray();
    if (!arr) return false;

    int emptyRun = 0;
    int chars = 0;
    for (int i = 0; i < kMaxChars; i++) {
        uintptr_t c = 0;
        if (!mem::Read(arr + (size_t)i * 8, c) || c == 0) { emptyRun++; continue; }
        if (!IsHeapLike(c, o.base, modEnd)) { emptyRun++; continue; }
        uintptr_t vt = 0;
        if (!mem::Read(c, vt) || vt != o.base + kCharVtableRva) {
            if (++emptyRun >= kSentinelRunStop) break;
            continue;
        }
        emptyRun = 0;
        chars++;
        char nm[96] = {};
        ReadName(c, nm, sizeof(nm));
        if (!IsPrintableName(nm) || !nm[0]) continue;
        if (IsPlayerName(nm)) {
            outPtr = c;
            log::Info("world: player char found via charPool[%d] = 0x%llX ('%s')",
                      i, (unsigned long long)c, nm);
            return true;
        }
    }
    if (chars > 0) {
        log::Info("world: charPool populated (%d chars) but no '%s' player yet",
                  chars, PlayerName());
    }
    return false;
}

bool FindCharacterByName(uintptr_t world, const char* name, uintptr_t& outPtr) {
    if (!world || !name || !name[0]) return false;
    const GameOffsets& o = Discovery::Get().Result();
    if (!o.base) return false;
    uintptr_t modEnd = o.base + 0x232D000;

    uintptr_t arr = FindCharacterPoolArray();
    if (!arr) return false;

    for (int emptyRun = 0, i = 0; i < kMaxChars; i++) {
        uintptr_t c = 0;
        if (!mem::Read(arr + (size_t)i * 8, c) || c == 0) { emptyRun++; continue; }
        if (!IsHeapLike(c, o.base, modEnd)) { emptyRun++; continue; }
        uintptr_t vt = 0;
        if (!mem::Read(c, vt) || vt != o.base + kCharVtableRva) {
            if (++emptyRun >= kSentinelRunStop) break;
            continue;
        }
        emptyRun = 0;
        char nm[96] = {};
        ReadName(c, nm, sizeof(nm));
        if (!IsPrintableName(nm)) continue;
        if (_stricmp(nm, name) == 0) {
            outPtr = c;
            return true;
        }
    }
    return false;
}

int ListCharacters(uintptr_t world, uintptr_t* out, int cap) {
    if (!world || !out || cap <= 0) return 0;
    const GameOffsets& o = Discovery::Get().Result();
    if (!o.base) return 0;
    uintptr_t modEnd = o.base + 0x232D000;

    uintptr_t arr = FindCharacterPoolArray();
    if (!arr) return 0;

    int written = 0;
    for (int emptyRun = 0, i = 0; i < kMaxChars && written < cap; i++) {
        uintptr_t c = 0;
        if (!mem::Read(arr + (size_t)i * 8, c) || c == 0) { emptyRun++; continue; }
        if (!IsHeapLike(c, o.base, modEnd)) { emptyRun++; continue; }
        uintptr_t vt = 0;
        if (!mem::Read(c, vt) || vt != o.base + kCharVtableRva) {
            if (++emptyRun >= kSentinelRunStop) break;
            continue;
        }
        emptyRun = 0;
        char nm[96] = {};
        ReadName(c, nm, sizeof(nm));
        if (!IsPrintableName(nm) || !nm[0]) continue;
        out[written++] = c;
    }
    return written;
}

bool GetCharacterPosition(uintptr_t charPtr, Vec3& pos) {
    pos = { 0, 0, 0 };
    if (!charPtr) return false;
    if (!mem::Read(charPtr + kPosOffset, pos.x)) return false;
    if (!mem::Read(charPtr + kPosOffset + 4, pos.y)) return false;
    if (!mem::Read(charPtr + kPosOffset + 8, pos.z)) return false;
    return true;
}

// Ogre quaternion at char+0x58 (w,x,y,z consecutive floats). CE-verified by
// Kenshi-Online on the same build (GameOffsets.rotation = 0x58).
bool GetCharacterRotation(uintptr_t charPtr, float rot[4]) {
    if (!charPtr) return false;
    for (int i = 0; i < 4; i++) {
        if (!mem::Read(charPtr + 0x58 + i * 4, rot[i])) return false;
    }
    return true;
}

bool WriteCharacterRotation(uintptr_t charPtr, const float rot[4]) {
    if (!charPtr) return false;
    for (int i = 0; i < 4; i++) {
        if (!mem::Write(charPtr + 0x58 + i * 4, &rot[i], 4)) return false;
    }
    return true;
}

bool GetCharacterName(uintptr_t charPtr, char* out, int cap) {
    if (!charPtr || cap <= 0) return false;
    ReadName(charPtr, out, cap);
    return out[0] != '\0' && IsPrintableName(out);
}

bool WriteCharacterPosition(uintptr_t charPtr, const Vec3& pos) {
    if (!charPtr) return false;

    // Guess the physics-domain position: some builds mirror the char body
    // at +kPosOffset (cached) and a live position behind an anim container.
    // We prefer the game's own cached field (safe) then probe the chain.
    bool wroteCached =
        mem::Write(charPtr + kPosOffset, &pos.x, 4) &&
        mem::Write(charPtr + kPosOffset + 4, &pos.y, 4) &&
        mem::Write(charPtr + kPosOffset + 8, &pos.z, 4);

    // Heuristic physics-chain write: scan +0x60..+0x200 for a pointer whose
    // deref chain at ~+0x48 matches a Vec3 close to the cached position.
    int probeOff = -1;
    uintptr_t writablePos = 0;
    for (int off = 0x60; off <= 0x200; off += 8) {
        uintptr_t candidate = 0;
        if (!mem::Read(charPtr + off, candidate)) continue;
        if (!IsHeapLike(candidate, Discovery::Get().Result().base,
                        Discovery::Get().Result().base + 0x232D000)) {
            continue;
        }
        for (int sub : { 0x48, 0x40, 0x58 }) {
            uintptr_t chainPos = candidate + sub;
            float ax = 0, ay = 0, az = 0;
            if (!mem::Read(chainPos, ax) || !mem::Read(chainPos + 4, ay) ||
                !mem::Read(chainPos + 8, az)) {
                continue;
            }
            if (std::fabs(ax - pos.x) < 2.0f && std::fabs(ay - pos.y) < 2.0f &&
                std::fabs(az - pos.z) < 2.0f) {
                writablePos = chainPos;
                probeOff = off;
                break;
            }
        }
        if (probeOff >= 0) break;
    }

    bool wroteChain = false;
    if (writablePos) {
        wroteChain =
            mem::Write(writablePos, &pos.x, 4) &&
            mem::Write(writablePos + 4, &pos.y, 4) &&
            mem::Write(writablePos + 8, &pos.z, 4);
    }

    log::Info("world: WritePosition char=0x%llX cached=%d chain=(0x%X)=%d ",
              (unsigned long long)charPtr, wroteCached ? 1 : 0,
              probeOff, wroteChain ? 1 : 0);
    return wroteCached || wroteChain;
}

// Kenshi-Online CE-verified health chain (v1.0.68): character+0x2B8 ->
// +0x5F8 -> +0x40 + stride*part. Body parts: Head=0, Chest=1, ... stride 8
// (health float + stun float per part). All reads/writes are guarded.
const int kHealthChain1 = 0x2B8;
const int kHealthChain2 = 0x5F8;
const int kHealthBase = 0x40;
const int kHealthStride = 8;
const int kHeadPart = 0;
const int kChestPart = 1;

uintptr_t HealthBlock(uintptr_t charPtr) {
    if (!charPtr) return 0;
    uintptr_t p1 = 0;
    if (!mem::Read(charPtr + kHealthChain1, p1) || !p1) return 0;
    uintptr_t p2 = 0;
    if (!mem::Read(p1 + kHealthChain2, p2) || !p2) return 0;
    return p2;
}

bool GetCharacterHealth(uintptr_t charPtr, int part, float& out) {
    if (part < 0 || part > 6) return false;
    uintptr_t hp = HealthBlock(charPtr);
    if (!hp) return false;
    return mem::Read(hp + kHealthBase + part * kHealthStride, out);
}

bool WriteCharacterHealth(uintptr_t charPtr, int part, float value) {
    if (part < 0 || part > 6) return false;
    uintptr_t hp = HealthBlock(charPtr);
    if (!hp) return false;
    float cur = 0;
    if (!mem::Read(hp + kHealthBase + part * kHealthStride, cur)) return false;
    if (cur <= value) return true; // already as bad or worse: leave it alone
    return mem::Write(hp + kHealthBase + part * kHealthStride, &value, 4);
}

// Combat state of a character (0=alive, 1=down/KO, 2=dead) using the
// Kenshi-Online IsAlive fallback: death when head or chest <= -100. A char
// reading <= 0 on head or chest is treated as down but not dead.
int CharacterCombatState(uintptr_t charPtr) {
    float head = 0, chest = 0;
    GetCharacterHealth(charPtr, kHeadPart, head);
    GetCharacterHealth(charPtr, kChestPart, chest);
    if (head <= -100.f || chest <= -100.f) return 2;
    if (head <= 0.f || chest <= 0.f) return 1;
    return 0;
}

} // namespace game
} // namespace kmmo
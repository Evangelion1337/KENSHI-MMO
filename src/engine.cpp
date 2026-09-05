#include "engine.h"

#include "log.h"
#include "mem.h"

#include <windows.h>
#include <cstdint>

namespace kmmo {
namespace engine {

namespace {

HMODULE g_lib = nullptr;
uintptr_t g_ouAddr = 0;   // address of KenshiLib's exported `ou` (GameWorld*)

// KenshiLib reverse-engineering anchors, valid for the sim we drive
// (game speed field float @ GameWorld+0x700, paused bool @ +0x8B9).
const uintptr_t kSpeedOff = 0x700;
const uintptr_t kPauseOff = 0x8B9;

const char* kOuName = "?ou@@3PEAVGameWorld@@EA";

} // namespace

bool Init() {
    if (g_lib) return true;
    g_lib = LoadLibraryA("KenshiLib.dll");
    if (!g_lib) {
        log::Warn("engine: KenshiLib.dll not loaded (err=%lu)",
                  (unsigned long)GetLastError());
        return false;
    }
    g_ouAddr = reinterpret_cast<uintptr_t>(
        GetProcAddress(g_lib, kOuName));
    if (!g_ouAddr) {
        log::Warn("engine: export '%s' not found", kOuName);
        return false;
    }
    log::Info("engine: KenshiLib.dll ready, ou global=0x%llX",
              (unsigned long long)g_ouAddr);
    return true;
}

bool Ready() {
    return g_lib && g_ouAddr;
}

uintptr_t GetGameWorld() {
    if (!Ready()) return 0;
    uintptr_t world = 0;
    if (!mem::Read(g_ouAddr, world)) return 0;
    if (!world) return 0;
    return world;
}

bool ReadSpeed(uintptr_t gw, float& mult, bool& paused) {
    if (!gw) return false;
    if (!mem::Read(gw + kSpeedOff, mult)) return false;
    uint8_t p = 0xFF;
    if (!mem::Read(gw + kPauseOff, p)) return false;
    if (p != 0 && p != 1) return false;
    paused = (p != 0);
    return true;
}

bool WriteSpeed(uintptr_t gw, float mult, bool paused) {
    if (!gw) return false;
    const uint8_t p = paused ? 1 : 0;
    return mem::Write(gw + kSpeedOff, &mult, 4) &&
           mem::Write(gw + kPauseOff, &p, 1);
}

} // namespace engine
} // namespace kmmo
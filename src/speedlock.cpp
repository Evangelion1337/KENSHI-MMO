#include "speedlock.h"

#include "log.h"
#include "engine.h"

#include <windows.h>
#include <cstdint>

namespace kmmo {
namespace speedlock {

namespace {

const float kForced = 1.0f;

volatile LONG g_started = 0;

bool LooksLikeSpeed(float v) {
    return v == 0.0f || v == 1.0f || v == 3.0f || v == 5.0f;
}

DWORD WINAPI LockThread(LPVOID) {
    const DWORD begin = GetTickCount();
    bool armed = false;   // only start writing once the offsets are sane
    bool logged = false;
    uintptr_t anchored = 0;

    while (true) {
        if (!armed && GetTickCount() - begin > 120000) {
            log::Info("speedlock: giving up (no validated GameWorld in 120s)");
            return 0;
        }

        uintptr_t gw = engine::GetGameWorld();
        if (!gw) { Sleep(250); continue; }

        if (!armed) {
            // The field must read as a real speed value and the pause flag as
            // a bool against the KenshiLib-anchored world before we write.
            float mult = -1.0f;
            bool paused = false;
            if (!engine::ReadSpeed(gw, mult, paused)) {
                Sleep(250);
                continue;
            }
            if (!LooksLikeSpeed(mult)) {
                log::Info("speedlock: GameWorld+0x700 doesn't read as a speed "
                          "on this build (mult=%.2f); not arming",
                          (double)mult);
                Sleep(1000);
                continue;
            }
            if (gw != anchored) {
                log::Info("speedlock: arming on KenshiLib `ou` world 0x%llX "
                          "(was mult=%.1f paused=%d)",
                          (unsigned long long)gw, (double)mult, paused ? 1 : 0);
                anchored = gw;
            }
            armed = true;
        }

        engine::WriteSpeed(gw, kForced, false);
        if (!logged) {
            logged = true;
            log::Info("speedlock: enforced x1 at every tick");
        }
        Sleep(80);
    }
}

} // namespace

void Begin() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    CreateThread(nullptr, 0, LockThread, nullptr, 0, nullptr);
    log::Info("speedlock: thread spawned");
}

} // namespace speedlock
} // namespace kmmo
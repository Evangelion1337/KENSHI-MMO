#include "discovery.h"

#include "mem.h"
#include "scan.h"
#include "log.h"

#include <windows.h>
#include <cstring>

namespace kmmo {

namespace {
const size_t kModuleSize = 0; // unused placeholder removed below if needed

uintptr_t ResolveByString(const mem::Module& mod, const scan::Sections& sec,
                          const char* str, size_t len) {
    uintptr_t s = scan::FindString(sec, str, len);
    if (!s) return 0;
    uintptr_t xref = scan::FindStringXref(sec, s);
    if (!xref) return 0;
    return scan::FindFunctionStart(mod, xref, 16384);
}

bool IsHeapLike(uintptr_t val, uintptr_t modBase, uintptr_t modEnd) {
    if (val < 0x10000 || val >= 0x00007FFFFFFFFFFFULL) return false;
    if (val >= modBase && val < modEnd) return false;
    return true;
}

bool HasVtableInText(uintptr_t obj, uintptr_t modBase, uintptr_t modEnd) {
    uintptr_t vtable = 0;
    if (!mem::Read(obj, vtable)) return false;
    if (vtable < modBase + 0x1000 || vtable >= modEnd) return false;
    return true;
}
} // namespace

Discovery& Discovery::Get() {
    static Discovery instance;
    return instance;
}

void Discovery::Start() {
    if (m_running) return;
    m_running = true;
    m_thread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        static_cast<Discovery*>(param)->Poll();
        return 0;
    }, this, 0, nullptr);
}

void Discovery::Stop() {
    m_running = false;
    if (m_thread) {
        WaitForSingleObject(m_thread, 3000);
        m_thread = nullptr;
    }
}

void Discovery::Poll() {
    mem::Module mod;
    while (m_running && !m_ready) {
        if (mem::FindModule("kenshi_x64.exe", mod)) {
            break;
        }
        Sleep(250);
    }
    if (!m_running) return;
    m_offsets.base = mod.base;

    scan::Sections sec;
    if (!scan::EnumerateSections(mod, sec)) {
        log::Error("Discovery: failed to enumerate sections");
        return;
    }
    log::Info("Discovery: module base=0x%llX size=0x%llX text=0x%llX rdata=0x%llX data=0x%llX",
              (unsigned long long)mod.base, (unsigned long long)mod.size,
              (unsigned long long)sec.text, (unsigned long long)sec.rdata,
              (unsigned long long)sec.data);

    // Resolve core functions by string xref (the proven ABI anchors).
    struct Anchor {
        const char* str;
        size_t len;
        uintptr_t& slot;
    };
    Anchor anchors[] = {
        { "[RootObjectFactory::process] Character", 38, m_offsets.CharacterSpawn },
        { "NodeList::destroyNodesByBuilding", 32, m_offsets.CharacterDestroy },
        { "[RootObjectFactory::createRandomSquad] Missing squad leader", 59, m_offsets.CreateRandomSquad },
        { "[Character::serialise] Character '", 34, m_offsets.CharacterSerialise },
        { "Attack damage effect", 20, m_offsets.ApplyDamage },
        { "Cutting damage", 14, m_offsets.StartAttack },
        { "Kenshi 1.0.", 11, m_offsets.GameFrameUpdate },
        { "timeScale", 9, m_offsets.TimeUpdate },
        { "quicksave", 9, m_offsets.SaveGame },
        { "[SaveManager::loadGame] No towns loaded.", 40, m_offsets.LoadGame },
        { "zone.%d.%d.zone", 15, m_offsets.ZoneLoad },
        { "Reset squad positions", 21, m_offsets.SquadCreate },
    };
    for (auto& a : anchors) {
        a.slot = ResolveByString(mod, sec, a.str, a.len);
        if (a.slot) {
            log::Info("Discovery: %s = 0x%llX", a.str,
                      (unsigned long long)a.slot);
        } else {
            log::Warn("Discovery: string not resolved: %s", a.str);
        }
    }

    uintptr_t modEnd = mod.base + mod.size;

    // Find a global in .data whose value points to a heap object with a
    // vtable in .text. That is the PlayerBase semantic signature.
    auto tryGlobal = [&](uintptr_t fnAddr, const char* label) -> uintptr_t {
        for (int n = 0; n < 24; n++) {
            uintptr_t g = scan::FindGlobalLoad(mod, sec, fnAddr, n);
            if (!g) break;
            uintptr_t val = 0;
            if (!mem::Read(g, val)) continue;
            if (!IsHeapLike(val, mod.base, modEnd)) continue;
            if (!HasVtableInText(val, mod.base, modEnd)) continue;
            log::Info("Discovery: %s = 0x%llX (val 0x%llX)",
                      label, (unsigned long long)g, (unsigned long long)val);
            return g;
        }
        return 0;
    };

    uintptr_t candidates[] = {
        m_offsets.CharacterSpawn,
        m_offsets.SaveGame,
        m_offsets.LoadGame,
        m_offsets.GameFrameUpdate,
        m_offsets.TimeUpdate,
        m_offsets.ZoneLoad,
    };

    while (m_running && !m_ready) {
        // Game may not be fully loaded on first pass; re-poll until PlayerBase
        // holds a valid value (two-pass discovery).
        if (m_offsets.PlayerBase == 0) {
            for (uintptr_t fn : candidates) {
                if (!fn) continue;
                m_offsets.PlayerBase = tryGlobal(fn, "PlayerBase");
                if (m_offsets.PlayerBase) break;
            }
        }
        if (m_offsets.GameWorldSingleton == 0) {
            for (uintptr_t fn : candidates) {
                if (!fn) continue;
                m_offsets.GameWorldSingleton = tryGlobal(fn, "GameWorldSingleton");
                if (m_offsets.GameWorldSingleton) break;
            }
        }
        if (m_offsets.PlayerBase && m_offsets.GameWorldSingleton) {
            m_ready = true;
            log::Info("Discovery: READY PlayerBase=0x%llX GameWorld=0x%llX",
                      (unsigned long long)m_offsets.PlayerBase,
                      (unsigned long long)m_offsets.GameWorldSingleton);
            break;
        }
        Sleep(1000);
    }
}

} // namespace kmmo
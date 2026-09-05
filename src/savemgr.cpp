#include "savemgr.h"

#include "mem.h"
#include "log.h"

#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>

namespace kmmo {
namespace savemgr {

namespace {

// Engine anchors (RVAs inside kenshi_x64.exe, verified on Steam 1.0.65):
const uintptr_t kFrameFuncRva = 0x1239D0;  // GameFrameUpdate (12-byte prologue)
const uintptr_t kNewGameRva   = 0x47A930;  // SaveManager::newGame(name)
const uintptr_t kLoadSaveRva  = 0x47AC10;  // SaveManager::load(name)
const uintptr_t kSingletonRva = 0x212DC08; // SaveManager* global

// SaveManager member offsets (derived from the same disassembly):
const uintptr_t kSignalOff = 0xA0;  // 1 SAVE, 2 LOAD, 3 IMPORT, 4 NEWGAME
const uintptr_t kBusyOff   = 0x1E0; // async op counter (>0 while loading/saving)

const int kPrologueLen = 12;

uintptr_t g_base = 0;
uintptr_t g_frameFunc = 0;
uintptr_t g_singletonAddr = 0;
unsigned char g_origPro[kPrologueLen] = {};
bool g_hooked = false;

// Trampoline: original prologue + absolute jump back into the function.
uintptr_t g_trampoline = 0;
typedef void (*FrameFn)(void* rcx);
FrameFn g_frameOrig = nullptr;

// Engine-ABI-compatible read-only string (MSVC std::string layout):
// +0x00 union { char* ptr; char buf[16]; }, +0x10 size, +0x18 capacity.
struct MsStr {
    union { char* ptr; char buf[16]; } x;
    size_t size;
    size_t cap;
};

// Pending-request ring (max one outstanding op at a time).
CRITICAL_SECTION g_lock;
bool g_lockInit = false;
bool g_requestPending = false;
int g_requestOp = 0;
char g_requestName[128] = {};
int g_requestId = 0; // highest id ever queued
long g_dispatchedId = 0; // id the engine pump last accepted

typedef void (*EngineStrFn)(uintptr_t mgr, const MsStr* name);

void EnsureLock() {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
}

// Emit `mov rax, imm64; jmp rax` (12 bytes) for a distance-free indirect jump.
size_t EmitMovRaxJmp(unsigned char* out, uintptr_t dst) {
    out[0] = 0x48;
    out[1] = 0xB8;
    memcpy(out + 2, &dst, sizeof(dst));
    out[10] = 0xFF;
    out[11] = 0xE0;
    return 12;
}

// Call the engine's newGame/load with our own MSVC-layout string. Returns true
// once the engine has accepted the request (signal field set).
bool EngineAccepted(int op, const char* name) {
    uintptr_t mgr = 0;
    if (!mem::Read(g_singletonAddr, mgr)) return false;
    if (!mgr || !mem::IsReadable(mgr, kBusyOff + 16)) return false;

    uint32_t signal = 0;
    if (!mem::Read(mgr + kSignalOff, signal)) return false;
    if (signal != 0) return false; // another transition is queued/running

    MsStr s;
    size_t len = strlen(name);
    s.size = len;
    if (len < 16) {
        memcpy(s.x.buf, name, len);
        s.x.buf[len] = '\0';
        s.cap = 15;               // SSO threshold (<16)
    } else {
        char* heap = static_cast<char*>(malloc(len + 1));
        if (!heap) return false;
        memcpy(heap, name, len);
        heap[len] = '\0';
        s.x.ptr = heap;
        s.cap = len;
    }

    EngineStrFn fn = nullptr;
    if (op == 1) {
        fn = reinterpret_cast<EngineStrFn>(g_base + kNewGameRva);
    } else if (op == 2) {
        fn = reinterpret_cast<EngineStrFn>(g_base + kLoadSaveRva);
    }
    if (!fn) {
        if (s.cap >= 16) free(s.x.ptr);
        return false;
    }
    fn(mgr, &s);
    if (s.cap >= 16) free(s.x.ptr);

    log::Info("savemgr: engine accepted op=%d name='%s'", op, name);
    return true;
}

// Runs on the game's main thread every frame.
void Pump() {
    if (!g_hooked) return;

    int op = 0;
    char name[128] = {};
    bool pending = false;
    {
        EnsureLock();
        EnterCriticalSection(&g_lock);
        if (g_requestPending) {
            op = g_requestOp;
            memcpy(name, g_requestName, sizeof(name));
            pending = true;
        }
        LeaveCriticalSection(&g_lock);
    }
    if (!pending) return;

    if (EngineAccepted(op, name)) {
        EnsureLock();
        EnterCriticalSection(&g_lock);
        g_requestPending = false;
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_dispatchedId, g_requestId);
    }
    // else: engine not ready (singleton null or transition in flight); the
    // request stays pending and this pump retries on the next frame.
}

void FrameHook(void* rcx) {
    if (g_frameOrig) g_frameOrig(rcx);
    Pump();
}

} // namespace

bool Init() {
    if (g_hooked) return true;
    EnsureLock();

    mem::Module mod;
    if (!mem::FindModule("kenshi_x64.exe", mod)) {
        log::Warn("savemgr: kenshi_x64.exe not loaded");
        return false;
    }
    g_base = mod.base;
    g_frameFunc = mod.base + kFrameFuncRva;
    g_singletonAddr = mod.base + kSingletonRva;

    if (!mem::IsReadable(g_frameFunc, kPrologueLen)) {
        log::Error("savemgr: frame function not readable");
        return false;
    }
    unsigned char probe[kPrologueLen];
    memcpy(probe, reinterpret_cast<const void*>(g_frameFunc), kPrologueLen);
    const unsigned char wish[3] = { 0x48, 0x8B, 0xC4 };
    if (memcmp(probe, wish, 3) != 0) {
        log::Error("savemgr: unexpected GameFrameUpdate prologue "
                   "(%02X %02X %02X)", probe[0], probe[1], probe[2]);
        return false;
    }
    memcpy(g_origPro, probe, kPrologueLen);

    unsigned char* tp = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!tp) {
        log::Error("savemgr: trampoline allocation failed");
        return false;
    }
    size_t off = 0;
    memcpy(tp + off, probe, kPrologueLen);
    off += kPrologueLen;
    off += EmitMovRaxJmp(tp + off, g_frameFunc + kPrologueLen);
    g_trampoline = reinterpret_cast<uintptr_t>(tp);
    g_frameOrig = reinterpret_cast<FrameFn>(g_trampoline);

    unsigned char patch[kPrologueLen];
    EmitMovRaxJmp(patch, reinterpret_cast<uintptr_t>(&FrameHook));
    if (!mem::Write(g_frameFunc, patch, kPrologueLen)) {
        log::Error("savemgr: hook patch failed");
        return false;
    }

    g_hooked = true;
    log::Info("savemgr: GameFrameUpdate hooked (base 0x%llX, fn 0x%llX)",
              (unsigned long long)g_base, (unsigned long long)g_frameFunc);
    return true;
}

void Shutdown() {
    if (!g_hooked) return;
    mem::Write(g_frameFunc, g_origPro, kPrologueLen);
    if (g_trampoline) {
        VirtualFree(reinterpret_cast<void*>(g_trampoline), 0, MEM_RELEASE);
        g_trampoline = 0;
    }
    g_frameOrig = nullptr;
    g_hooked = false;
    log::Info("savemgr: GameFrameUpdate hook removed");
}

int Queue(int op, const char* slotName) {
    if (!g_hooked) return 0;
    if (!slotName || !slotName[0] || strlen(slotName) > 96) return 0;
    EnsureLock();
    EnterCriticalSection(&g_lock);
    g_requestPending = true;
    g_requestOp = op;
    strncpy(g_requestName, slotName, sizeof(g_requestName) - 1);
    g_requestName[sizeof(g_requestName) - 1] = '\0';
    int id = ++g_requestId;
    LeaveCriticalSection(&g_lock);
    log::Info("savemgr: queued op=%d name='%s' id=%d", op, slotName, id);
    return id;
}

int RequestNewGame(const char* slotName) {
    return Queue(1, slotName);
}

int RequestLoad(const char* slotName) {
    return Queue(2, slotName);
}

int DispatchCount() {
    return static_cast<int>(InterlockedCompareExchange(&g_dispatchedId, 0, 0));
}

bool TransitionSettled() {
    if (!g_hooked) return false;
    uintptr_t mgr = 0;
    if (!mem::Read(g_singletonAddr, mgr) || !mgr) return false;
    uint32_t signal = 1;
    if (!mem::Read(mgr + kSignalOff, signal)) return false;
    long long busy = 1;
    if (!mem::Read(mgr + kBusyOff, busy)) return false;
    return signal == 0 && busy <= 0;
}

} // namespace savemgr
} // namespace kmmo
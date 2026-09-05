#include "session.h"

#include "net.h"
#include "log.h"
#include "world.h"
#include "discovery.h"
#include "savesync.h"
#include "savemgr.h"
#include "speedlock.h"
#include "version.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace kmmo {
namespace session {

namespace {

struct Session {
    CRITICAL_SECTION lock;
    char status[256] = "Not logged in.";
    char username[128] = {};
    char host[128] = KMMO_SERVER_HOST;
    int port = 25565;
    volatile LONG epoch = 0;       // bumped on every login/logout
    HANDLE worker = nullptr;
};

Session g_s;
bool g_lockInit = false;

// Remote-character registry: positions the server relays from other clients.
// Each tick the worker moves the matching character (if present in this
// world) to the reported position so co-op partners become visible. Both
// clients load the SAME shared world save, so every character exists in both
// worlds and the whole world can be synced by name.
const int kMaxRemote = 256;
struct RemotePos {
    char name[128];
    float x, y, z;
    DWORD lastMs;
    int active;
};
RemotePos g_remotes[kMaxRemote];
CRITICAL_SECTION g_remoteLock;
bool g_remoteLockInit = false;

// Cached view of the live character pool for whole-world broadcast.
const int kMaxCache = 1024;
uintptr_t g_cache[kMaxCache];
Vec3 g_cachePos[kMaxCache];
DWORD g_cacheSentMs[kMaxCache];
int g_cacheCount = 0;

// Latest server time-of-day (seconds) and online roster for the status line.
volatile DWORD g_clockSec = 0;
char g_onlineNames[512] = {};
int g_onlineCount = 0;

void RemoteEnsureLock() {
    if (g_remoteLockInit) return;
    InitializeCriticalSection(&g_remoteLock);
    g_remoteLockInit = true;
}

void RemoteSet(const char* name, float x, float y, float z) {
    RemoteEnsureLock();
    EnterCriticalSection(&g_remoteLock);
    int freeSlot = -1;
    for (int i = 0; i < kMaxRemote; i++) {
        if (g_remotes[i].active) {
            if (_stricmp(g_remotes[i].name, name) == 0) {
                g_remotes[i].x = x;
                g_remotes[i].y = y;
                g_remotes[i].z = z;
                g_remotes[i].lastMs = GetTickCount();
                LeaveCriticalSection(&g_remoteLock);
                return;
            }
            continue;
        }
        if (freeSlot < 0) freeSlot = i;
    }
    if (freeSlot >= 0) {
        RemotePos& r = g_remotes[freeSlot];
        strncpy(r.name, name, sizeof(r.name) - 1);
        r.x = x;
        r.y = y;
        r.z = z;
        r.lastMs = GetTickCount();
        r.active = 1;
    }
    LeaveCriticalSection(&g_remoteLock);
}

// Move every tracked remote character to its reported position. Names equal
// to the local player's character are skipped so broadcast echoes from the
// partner (who also simulates our character) never fight the local sim.
void ApplyRemotePositions(const char* ownName) {
    uintptr_t world = game::GetGameWorld(Discovery::Get());
    if (!world) return;
    RemoteEnsureLock();
    EnterCriticalSection(&g_remoteLock);
    const DWORD now = GetTickCount();
    for (int i = 0; i < kMaxRemote; i++) {
        if (!g_remotes[i].active) continue;
        if (now - g_remotes[i].lastMs > 20000) continue;
        if (ownName[0] && _stricmp(g_remotes[i].name, ownName) == 0) continue;
        uintptr_t ch = 0;
        if (game::FindCharacterByName(world, g_remotes[i].name, ch)) {
            Vec3 p = { g_remotes[i].x, g_remotes[i].y, g_remotes[i].z };
            if (game::WriteCharacterPosition(ch, p)) {
                log::Info("session: moved remote '%s' to (%.0f %.0f %.0f)",
                          g_remotes[i].name, p.x, p.y, p.z);
            }
        }
    }
    LeaveCriticalSection(&g_remoteLock);
}

void RemotePrune() {
    RemoteEnsureLock();
    EnterCriticalSection(&g_remoteLock);
    const DWORD now = GetTickCount();
    for (int i = 0; i < kMaxRemote; i++) {
        if (g_remotes[i].active && now - g_remotes[i].lastMs > 60000) {
            g_remotes[i].active = 0;
            g_remotes[i].name[0] = '\0';
        }
    }
    LeaveCriticalSection(&g_remoteLock);
}

// Refresh the cached view of the live character pool. Runs on a slow cadence:
// the pool only changes on zone streaming / world loads, and the cache drives
// per-char cooldowns so a refresh can never flood the network.
void RefreshCharCache() {
    uintptr_t world = game::GetGameWorld(Discovery::Get());
    if (!world) return;
    uintptr_t next[kMaxCache];
    int n = game::ListCharacters(world, next, kMaxCache);
    const DWORD now = GetTickCount();
    for (int i = 0; i < n; i++) {
        if (g_cache[i] != next[i]) {
            g_cache[i] = next[i];
            g_cacheSentMs[i] = now; // new char: wait before first send
        }
    }
    for (int i = n; i < kMaxCache; i++) {
        g_cache[i] = 0;
        g_cacheSentMs[i] = 0;
    }
    g_cacheCount = n;
    log::Info("session: world camera sees %d characters in this zone", n);
}

// Broadcast live positions for every cached world character, keyed by in-game
// name. Both clients sim the same shared save, so the peer can find each char
// by name and snap it into place. Throttled per-char and capped per tick so
// NPC drift (below the 2-unit threshold) never saturates the relay.
int BroadcastWorldPositions(net::Session* s) {
    const DWORD now = GetTickCount();
    int sent = 0;
    for (int i = 0; i < g_cacheCount && sent < 16; i++) {
        uintptr_t ch = g_cache[i];
        if (!ch) continue;
        if (now - g_cacheSentMs[i] < 800) continue;
        Vec3 cur = {};
        if (!game::GetCharacterPosition(ch, cur)) continue;
        if (g_cacheSentMs[i] != 0 &&
            std::fabsf(cur.x - g_cachePos[i].x) < 2.0f &&
            std::fabsf(cur.y - g_cachePos[i].y) < 2.0f &&
            std::fabsf(cur.z - g_cachePos[i].z) < 2.0f) {
            continue;
        }
        char nm[96] = {};
        if (!game::GetCharacterName(ch, nm, sizeof(nm))) continue;
        char rp[200];
        snprintf(rp, sizeof(rp), "RPOS %s %d %d %d\r\n",
                 nm, (int)cur.x, (int)cur.y, (int)cur.z);
        if (net::SessionSendLine(s, rp, 5000) == net::Result::Ok) {
            g_cachePos[i] = cur;
            g_cacheSentMs[i] = now;
            sent++;
        }
    }
    return sent;
}

// Parse an "ONLINE <n> <name> [<name> ...]" roster (count token skipped)
// into the status globals.
void HandleOnlineLine(const char* p) {
    RemoteEnsureLock();
    g_onlineCount = 0;
    g_onlineNames[0] = '\0';
    // Skip the count token ("ONLINE 3" -> names start after it).
    while (*p && *p == ' ') p++;
    while (*p && *p != ' ') p++;
    size_t off = 0;
    while (*p && g_onlineCount < 24 && off < sizeof(g_onlineNames) - 1) {
        if (*p == ' ') { p++; continue; }
        const char* e = p;
        while (*e && *e != ' ') e++;
        int len = (int)(e - p);
        if (len > 0) {
            int n = snprintf(g_onlineNames + off, sizeof(g_onlineNames) - off,
                             "%s%.*s", g_onlineCount ? ", " : "", len, p);
            if (n > 0) off += (size_t)n;
            g_onlineCount++;
        }
        p = e;
    }
    log::Info("session: online roster (%d): %s", g_onlineCount, g_onlineNames);
}

void EnsureLockInit() {
    if (g_lockInit) return;
    InitializeCriticalSection(&g_s.lock);
    g_lockInit = true;
}

void SetStatus(const char* fmt, ...) {
    EnsureLockInit();
    EnterCriticalSection(&g_s.lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_s.status, sizeof(g_s.status), fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_s.lock);
}

// Trigger the game's native quicksave (F5) so the whole squad is written to
// the local autosave. The game processes the key on its own thread, which is
// far safer than calling the discovered quicksave function from a worker.
void TriggerQuicksave() {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_F5;
    in[0].ki.dwFlags = 0;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_F5;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(2, in, sizeof(INPUT));
    log::Info("session: quicksave key injected (SendInput=%u)", sent);
}

// ---- server-side shared world blob transfer ---------------------------------

const size_t kBlobChunk = 256 * 1024;

// All clients upload/download through this single key: the server world blob.
// Login stays per-account, but the WORLD is shared so every client loads the
// same authority save and co-op partners are guaranteed to coexist in it.
const char* kSharedBlob = "shared";

// SAVE_DOWNLOAD_BEGIN .. SAVE_DOWNLOAD .. SAVE_DOWNLOAD_END. Returns a malloc'd
// blob (exactly `size` bytes) or nullptr.
unsigned char* DownloadBlob(net::Session* s, const char* user, long long size) {
    if (size <= 0 || size > (long long)(512 << 20)) return nullptr;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SAVE_DOWNLOAD_BEGIN %s\r\n", user);
    net::Reply r = net::SessionRequest(s, cmd, 8000);
    if (r.result != net::Result::Ok || strncmp(r.line, "OK READY", 8) != 0) {
        log::Warn("session: download begin failed: %s", r.line);
        return nullptr;
    }

    unsigned char* blob = static_cast<unsigned char*>(malloc((size_t)size));
    if (!blob) return nullptr;

    long long off = 0;
    while (off < size) {
        long long want = size - off;
        if (want > (long long)kBlobChunk) want = (long long)kBlobChunk;
        snprintf(cmd, sizeof(cmd), "SAVE_DOWNLOAD %s %lld %lld\r\n",
                 user, (long long)off, want);
        if (net::SessionSendLine(s, cmd, 5000) != net::Result::Ok) {
            log::Warn("session: download send failed at %lld", (long long)off);
            free(blob);
            return nullptr;
        }
        net::Reply r2 = net::SessionRecvLine(s, 90000);
        long long got = 0;
        if (r2.result != net::Result::Ok ||
            sscanf(r2.line, "OK CHUNK %lld", &got) != 1 || got <= 0) {
            log::Warn("session: download chunk failed: %s", r2.line);
            free(blob);
            return nullptr;
        }
        if (net::SessionRecvBytes(s, blob + off, (size_t)got, 90000) !=
            net::Result::Ok) {
            log::Warn("session: download bytes failed at %lld", (long long)off);
            free(blob);
            return nullptr;
        }
        off += got;
    }
    snprintf(cmd, sizeof(cmd), "SAVE_DOWNLOAD_END\r\n");
    net::SessionRequest(s, cmd, 5000);
    log::Info("session: downloaded %lld-byte save blob for '%s'",
              (long long)size, user);
    return blob;
}

// SAVE_UPLOAD_BEGIN .. SAVE_UPLOAD+bytes .. SAVE_UPLOAD_END. True on full store.
bool UploadBlob(net::Session* s, const char* user,
                const unsigned char* blob, size_t len) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SAVE_UPLOAD_BEGIN %s\r\n", user);
    net::Reply r = net::SessionRequest(s, cmd, 8000);
    if (r.result != net::Result::Ok || strncmp(r.line, "OK READY", 8) != 0) {
        log::Warn("session: upload begin failed: %s", r.line);
        return false;
    }

    size_t off = 0;
    while (off < len) {
        size_t want = len - off;
        if (want > kBlobChunk) want = kBlobChunk;
        snprintf(cmd, sizeof(cmd), "SAVE_UPLOAD %s %zu %zu\r\n",
                 user, off, want);
        if (net::SessionSendLine(s, cmd, 5000) != net::Result::Ok ||
            net::SessionSendBytes(s, blob + off, want) != net::Result::Ok) {
            log::Warn("session: upload chunk failed at %zu", off);
            return false;
        }
        net::Reply r2 = net::SessionRecvLine(s, 20000);
        if (r2.result != net::Result::Ok || strncmp(r2.line, "OK CHUNK", 8) != 0) {
            log::Warn("session: upload ack failed: %s", r2.line);
            return false;
        }
        off += want;
    }
    snprintf(cmd, sizeof(cmd), "SAVE_UPLOAD_END %s %zu\r\n", user, len);
    net::Reply re = net::SessionRequest(s, cmd, 8000);
    bool ok = re.result == net::Result::Ok && strncmp(re.line, "OK SAVED", 8) == 0;
    log::Info("session: uploaded %zu-byte save blob for '%s' (%s)",
              len, user, ok ? "saved" : "failed");
    return ok;
}

// ---- world-restore gate ------------------------------------------------------

// After the engine accepts a load the world tears down and rebuilds. Wait
// until the SaveManager is idle AND a safety floor elapsed, so a character
// wait that follows never mistakes the pre-restore wanderer for the restore
// result. Bounded: if the engine never accepts the op (e.g. transitions stay
// busy), give up so the session can still serve the live-character poll.
// Returns true when the transition may be considered complete.
bool WaitRestoreSettled(int reqId) {
    const DWORD t0 = GetTickCount();
    const DWORD kFloorMs = 15000;
    const DWORD kCapMs = 90000;
    for (;;) {
        DWORD elapsed = GetTickCount() - t0;
        if (savemgr::DispatchCount() < reqId) {
            if (elapsed > kCapMs) return savemgr::TransitionSettled();
            Sleep(50);
            continue;
        }
        if (elapsed >= kFloorMs && savemgr::TransitionSettled()) return true;
        if (elapsed > kCapMs) return savemgr::TransitionSettled();
        Sleep(250);
    }
}

// EnterWorldWorker restores the account's world the only legal way: the server
// owns the save. Connection -> SAVE_INFO decides fresh play vs full restore:
//  - no server save: adopt an existing local game if one is present, otherwise
//    just watch for the player's own new game (no engine op is queued; a queued
//    new-game can hang the gate if the engine never switches worlds itself)
//  - server save:    download the folder blob, unpack it locally, then native
//                    load that slot on the game's main thread (savemgr hook)
// afterwards the same spawn/keep-alive machinery runs as before.
DWORD WINAPI EnterWorldWorker(LPVOID param) {
    const LONG myEpoch = reinterpret_cast<LONG_PTR>(param);

    char userName[128] = {};
    {
        EnterCriticalSection(&g_s.lock);
        if (g_s.epoch != myEpoch) {
            LeaveCriticalSection(&g_s.lock);
            return 0; // superseded before we even started
        }
        strncpy(userName, g_s.username, sizeof(userName) - 1);
        LeaveCriticalSection(&g_s.lock);
    }

    log::Info("session: enter-world worker started (epoch=%ld)", (long)myEpoch);

    // Persistent session with the server (restore needs this connection).
    const size_t kConnTimeout = 5000;
    net::Session* s = net::SessionStart(g_s.host, g_s.port, kConnTimeout);
    if (!s) {
        SetStatus("Could not reach world server.");
        return 0;
    }
    net::SessionDrain(s, 5000); // consume greeting

    char infoCmd[256];
    snprintf(infoCmd, sizeof(infoCmd), "SAVE_INFO %s\r\n", kSharedBlob);
    net::Reply info = net::SessionRequest(s, infoCmd, 8000);

    long long saveSize = 0;
    bool haveSave = false;
    if (info.result == net::Result::Ok && strncmp(info.line, "OK SAVESIZE", 11) == 0) {
        saveSize = atoll(info.line + 11);
        haveSave = saveSize > 0;
        SetStatus("Restoring your world from the server...");
    } else if (info.result == net::Result::Ok && strncmp(info.line, "OK NOSAVE", 9) == 0) {
        SetStatus("Starting a new Wanderer...");
    } else {
        SetStatus("Server save query failed (%s).", info.line);
        log::Warn("session: SAVE_INFO failed: %s", info.line);
        net::SessionStop(s);
        return 0;
    }

    int reqId = 0;
    if (haveSave) {
        unsigned char* blob = DownloadBlob(s, kSharedBlob, saveSize);
        if (!blob) {
            SetStatus("Could not download your world save.");
            net::SessionStop(s);
            return 0;
        }
        // The server save overrides any local files: clear the folder so the
        // unpack below lands clean, then unpack the authoritative save.
        savesync::WipeLocalSaves();
        char slotName[128] = {};
        int written = savesync::UnpackSaveBlob(blob, (size_t)saveSize,
                                               slotName, sizeof(slotName));
        free(blob);
        if (written <= 0 || !slotName[0]) {
            SetStatus("Could not unpack your world save (corrupt?).");
            net::SessionStop(s);
            return 0;
        }
        log::Info("session: restored '%s' (%d files); loading slot '%s'",
                  userName, written, slotName);
        reqId = savemgr::RequestLoad(slotName);
    } else {
        // No server save yet. A previous session may have left a local game
        // behind (first play whose quicksave upload never ran). Adopt it:
        // upload the local folder as this account's authoritative save, then
        // load it so the player continues instead of starting over.
        const char* slot = savesync::LatestSaveFolder();
        if (slot && slot[0]) {
            size_t blobLen = 0;
            unsigned char* blob = savesync::PackSaveFolder(slot, &blobLen);
            if (blob && blobLen > 0) {
                if (UploadBlob(s, kSharedBlob, blob, blobLen)) {
                    log::Info("session: adopted local save '%s' (%zu bytes)",
                              slot, blobLen);
                    reqId = savemgr::RequestLoad(slot);
                } else {
                    log::Warn("session: could not adopt local save '%s'", slot);
                }
                free(blob);
            } else {
                log::Warn("session: could not pack local save '%s'", slot);
            }
        }
        // No local game either: the player starts a fresh game themselves.
        // No engine op is queued here (a queued new-game can stall the settle
        // gate when the engine never switches worlds on its own).
    }

    if (reqId != 0) {
        if (!WaitRestoreSettled(reqId)) {
            SetStatus("World transition pending...");
            log::Warn("session: world transition did not settle; continuing");
        }
        log::Info("session: world transition ready (id=%d)", reqId);
    } else {
        log::Info("session: no pre-load queued; watching for a live character");
    }

    // Fall back to officially spawning from the server-stored position.
    char enterCmd[256];
    snprintf(enterCmd, sizeof(enterCmd), "ENTER_WORLD %s\r\n", userName);
    net::Reply ent = net::SessionRequest(s, enterCmd, 8000);

    // Wait for the restored world to have a live player character. The one
    // persistent session connection stays open from this point until the
    // player quits the game (epoch bump) or the server becomes unreachable;
    // there is no timeout-driven disconnect during character creation.
    uintptr_t charPtr = 0;
    char charName[96] = {};
    bool live = false;
    bool spawnApplied = false;
    unsigned long polls = 0;
    Vec3 lastSentPos = {};
    bool posSeeded = false;
    int periodic = 0;
    int nextRefresh = 30;

    // Force the game to run at exactly x1 forever (server-synced world).
    speedlock::Begin();
    for (;;) {
        polls++;
        {
            EnterCriticalSection(&g_s.lock);
            bool stillCurrent = (g_s.epoch == myEpoch);
            LeaveCriticalSection(&g_s.lock);
            if (!stillCurrent) {
                log::Info("session: persistent session ended (epoch=%ld)",
                          (long)myEpoch);
                break;
            }
        }

        if (polls % 30 == 0) {
            log::Info("session: alive tick (polls=%lu, live=%s)",
                      polls, live ? "yes" : "no");
        }

        // Never bulk-wipe local saves on a timer: between quicksave uploads the
        // local folder is the only copy of the player's world. It is cleaned
        // only after a successful upload, or before restoring a server save.

        // Service the persistent socket with a short poll so the loop below
        // also keeps watching for the live character during character creation.
        net::Reply line = net::SessionRecvLine(s, 1000);
        if (line.result == net::Result::Closed) {
            log::Warn("session: server session closed; reconnecting (RESUME)");
            net::SessionStop(s);
            s = net::SessionStart(g_s.host, g_s.port, kConnTimeout);
            if (!s) {
                SetStatus("Lost connection to world server.");
                break;
            }
            net::SessionDrain(s, 5000);
            char resCmd[256];
            snprintf(resCmd, sizeof(resCmd), "RESUME %s\r\n", userName);
            net::Reply rr = net::SessionRequest(s, resCmd, 8000);
            log::Info("session: resumed connection: %s", rr.line);
            continue;
        }
        if (line.result == net::Result::Ok && strcmp(line.line, "QUICKSAVE") == 0) {
            TriggerQuicksave();
            Sleep(3000); // let the engine finish writing the folder

            const char* folder = savesync::LatestSaveFolder();
            if (folder && savesync::FolderHasWorld(folder)) {
                size_t blobLen = 0;
                unsigned char* blob = savesync::PackSaveFolder(folder, &blobLen);
                if (blob && blobLen > 0) {
                    bool uploaded = UploadBlob(s, kSharedBlob, blob, blobLen);
                    free(blob);
                    if (uploaded) {
                        log::Info("session: uploaded live save slot '%s' (%zu bytes)",
                                  folder, blobLen);
                        // Keep only the save the server now holds.
                        savesync::WipeOtherSlots(folder);
                    } else {
                        log::Warn("session: upload failed; keeping local save");
                    }
                } else {
                    log::Warn("session: could not pack live save slot '%s'", folder);
                }

                // Position sync stays best-effort (only when the character
                // object is discoverable in memory).
                Vec3 nowPos = {};
                uintptr_t pc = charPtr;
                if (!pc) {
                    uintptr_t world = game::GetGameWorld(Discovery::Get());
                    game::FindFirstCharacter(world, pc);
                }
                if (pc && game::GetCharacterPosition(pc, nowPos)) {
                    char spCmd[256];
                    snprintf(spCmd, sizeof(spCmd), "SAVE_POS %s %d %d %d\r\n",
                             userName, (int)nowPos.x, (int)nowPos.y, (int)nowPos.z);
                    net::Reply rp = net::SessionRequest(s, spCmd, 5000);
                    log::Info("session: synced position to server: %s", rp.line);
                }
            } else {
                log::Info("session: no live save slot to upload (menu/creation); skipped");
            }
            continue;
        }
        if (line.result == net::Result::Ok && strncmp(line.line, "POS ", 4) == 0) {
            // Position relay: "<user> x y z" tracking a remote player.
            float px = 0, py = 0, pz = 0;
            char rn[128] = {};
            if (sscanf(line.line + 4, "%127s %f %f %f", rn, &px, &py, &pz) >= 4) {
                RemoteSet(rn, px, py, pz);
                log::Info("session: remote '%s' @ (%.0f %.0f %.0f)", rn, px, py, pz);
            }
            continue;
        }
        if (line.result == net::Result::Ok && strncmp(line.line, "CLOCK ", 6) == 0) {
            g_clockSec = (DWORD)atol(line.line + 6);
            log::Info("session: world clock now %lu", (unsigned long)g_clockSec);
            continue;
        }
        if (line.result == net::Result::Ok && strncmp(line.line, "ONLINE ", 7) == 0) {
            HandleOnlineLine(line.line + 7);
            continue;
        }
        if (line.result == net::Result::Ok) {
            log::Info("session: server said: %s", line.line);
            continue;
        }
        // Timeout: nothing arrived from the server this window. Fall through
        // to the character poll; the socket stays open.

        // Poll for a live character. Covers character creation, load screens,
        // and the restored-world handoff. Deliberately unbounded: the session
        // must stay connected while the player makes their character.
        if (!live) {
            if (!Discovery::Get().Ready()) {
                if (polls % 100 == 0) log::Info("session: waiting for discovery...");
                Sleep(250);
                continue;
            }
            uintptr_t world = game::GetGameWorld(Discovery::Get());
            if (!world) {
                if (polls % 100 == 0) log::Info("session: waiting for GameWorld...");
                Sleep(250);
                continue;
            }
            uintptr_t cand = 0;
            if (!game::FindFirstCharacter(world, cand)) {
                if (polls % 100 == 0) log::Info("session: waiting for a live character...");
                Sleep(250);
                continue;
            }
            charPtr = cand;
            game::GetCharacterName(charPtr, charName, sizeof(charName));
            live = true;
            log::Info("session: live character found: 0x%llX ('%s')",
                      (unsigned long long)charPtr, charName);
            continue;
        }

        // A live character exists; apply the server SPAWN exactly once.
        if (!spawnApplied) {
            spawnApplied = true;
            if (ent.result == net::Result::Ok && strncmp(ent.line, "OK SPAWN", 8) == 0) {
                float x = 0, y = 0, z = 0;
                char sqBuf[96] = {};
                int sqc = 0;
                if (sscanf(ent.line + 9, "%f %f %f", &x, &y, &z) >= 3) {
                    // Optional "SQUAD <name>" token lets us target this account's squad.
                    char* sqTok = strstr(ent.line, "SQUAD ");
                    if (sqTok) {
                        strncpy(sqBuf, sqTok + 6, sizeof(sqBuf) - 1);
                        sqBuf[sizeof(sqBuf) - 1] = '\0';
                        char* sp = strpbrk(sqBuf, " \r\n");
                        if (sp) *sp = '\0';
                        sqc = (int)strlen(sqBuf);
                    }
                    if (sqc > 0) {
                        game::SetPreferredPlayerName(sqBuf);
                        uintptr_t cand2 = 0;
                        uintptr_t world = game::GetGameWorld(Discovery::Get());
                        if (world && game::FindFirstCharacter(world, cand2)) {
                            charPtr = cand2;
                            game::GetCharacterName(charPtr, charName, sizeof(charName));
                            log::Info("session: matched owned squad '%s' -> %s",
                                      sqBuf, charName);
                        } else {
                            log::Info("session: owned squad '%s' not in world yet; "
                                      "keeping current character", sqBuf);
                        }
                    }
                    Vec3 pos = { x, y, z };
                    bool ok = game::WriteCharacterPosition(charPtr, pos);
                    if (ok) {
                        char saveCmd[256];
                        snprintf(saveCmd, sizeof(saveCmd), "SAVE_POS %s %d %d %d\r\n",
                                 userName, (int)pos.x, (int)pos.y, (int)pos.z);
                        net::Reply r2 = net::SessionRequest(s, saveCmd, 5000);
                        log::Info("session: recorded server load back: %s", r2.line);
                    }
                    SetStatus(ok ? "Spawned at %.0f, %.0f, %.0f (server load)" :
                                    "Spawned (position write failed).", x, y, z);
                    log::Info("session: spawn OK at (%.1f %.1f %.1f) write=%d squad='%s'",
                              x, y, z, ok ? 1 : 0, sqc > 0 ? sqBuf : "(none)");
                } else {
                    SetStatus("Server SPAWN reply unparseable.");
                    log::Warn("session: bad SPAWN reply '%s'", ent.line);
                }
            } else {
                SetStatus("Server rejected load (%s).", ent.line);
                log::Warn("session: server load failed: %s", ent.line);
            }
            continue;
        }

        // Periodic world sync on the ~1s poll cadence: checkpoint our own
        // position, broadcast the whole visible world to the relay, snap
        // remote characters into place, refresh roster and clock.
        periodic++;
        if (live) {
            Vec3 cur = {};
            if (game::GetCharacterPosition(charPtr, cur)) {
                if (!posSeeded ||
                    std::fabsf(cur.x - lastSentPos.x) > 4.0f ||
                    std::fabsf(cur.y - lastSentPos.y) > 4.0f) {
                    lastSentPos = cur;
                    posSeeded = true;
                    char spCmd[256];
                    snprintf(spCmd, sizeof(spCmd), "SAVE_POS %s %d %d %d\r\n",
                             userName, (int)cur.x, (int)cur.y, (int)cur.z);
                    net::Reply rp = net::SessionRequest(s, spCmd, 5000);
                    if (rp.result == net::Result::Ok &&
                        strncmp(rp.line, "OK SAVED", 8) == 0) {
                        log::Info("session: checkpointed own spawn (%.0f %.0f %.0f)",
                                  cur.x, cur.y, cur.z);
                    }
                }
            }
            if (periodic % 30 == 1) RefreshCharCache();
            BroadcastWorldPositions(s);
            ApplyRemotePositions(charName);
            RemotePrune();
            if (++nextRefresh >= 30) {
                nextRefresh = 0;
                net::Reply w = net::SessionRequest(s, "WHO\r\n", 5000);
                if (w.result == net::Result::Ok) {
                    char* at = strstr(w.line, "ONLINE ");
                    if (at) HandleOnlineLine(at + 7);
                }
                net::Reply t = net::SessionRequest(s, "TIME?\r\n", 5000);
                if (t.result == net::Result::Ok && strncmp(t.line, "OK CLOCK", 8) == 0) {
                    DWORD c = (DWORD)atol(t.line + 9);
                    if (c != g_clockSec) {
                        g_clockSec = c;
                        log::Info("session: clock query -> %lu", (unsigned long)c);
                    }
                }
            }
            continue;
        }
    }

    net::SessionStop(s);
    return 0;
}

} // namespace

void OnLoginOk(const char* username, const char* serverHost, int serverPort) {
    EnsureLockInit();
    log::Info("session: OnLoginOk entry (%s)", username);
    InterlockedIncrement(&g_s.epoch);
    const LONG newEpoch = g_s.epoch;
    {
        EnterCriticalSection(&g_s.lock);
        strncpy(g_s.host, serverHost, sizeof(g_s.host) - 1);
        g_s.port = serverPort;
        strncpy(g_s.username, username ? username : "", sizeof(g_s.username) - 1);
        LeaveCriticalSection(&g_s.lock);
    }
    log::Info("session: OnLoginOk copied state, epoch=%ld", (long)newEpoch);
    SetStatus("Entering world (restoring from server)...");
    log::Info("session: OnLoginOk status set");
    if (g_s.worker) {
        CloseHandle(g_s.worker);
        g_s.worker = nullptr;
    }
    log::Info("session: login OK '%s'; spawning enter-world worker (epoch=%ld)",
              username, (long)newEpoch);
    g_s.worker = CreateThread(nullptr, 0, EnterWorldWorker,
                              reinterpret_cast<LPVOID>(static_cast<LONG_PTR>(newEpoch)), 0, nullptr);
}

void OnLogout() {
    EnsureLockInit();
    InterlockedIncrement(&g_s.epoch);
    SetStatus("Logged out.");
    log::Info("session: logout (epoch=%ld)", (long)g_s.epoch);
}

const char* Status() {
    return g_s.status;
}

} // namespace session
} // namespace kmmo
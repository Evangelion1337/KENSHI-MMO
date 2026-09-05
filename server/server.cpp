#include "accounts.h"
#include "srvlog.h"
#include "../src/sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>

#pragma comment(lib, "ws2_32.lib")

namespace {

const int kPort = 25565;
volatile LONG g_active = 0;

// Server-side "online" registry: a user is allowed to ENTER_WORLD only if they
// logged in on some connection within the grace window (fresh socket handshake).
struct OnlineUser {
    char name[128];
    DWORD lastLoginMs;
};
const int kMaxOnline = 256;
OnlineUser g_online[kMaxOnline];
CRITICAL_SECTION g_onlineLock;

// Live game sessions: each logged-in client keeps its socket open for the whole
// session so the server can drive autosaves (QUICKSAVE) and remember logins.
struct LiveConn {
    SOCKET s;
    char user[128];
    char pchar[96];      // character name this session commands (SETPLAYER)
    volatile LONG active;
};
LiveConn g_conns[kMaxOnline];
CRITICAL_SECTION g_connLock;

// Position relay: the newest reported world position per in-world character,
// pushed to every OTHER live session so clients can move remote characters.
// Keyed by character name (not account); the source socket lets us skip the
// sender so a client never re-applies its own positions.
struct PosRelay {
    char name[128];
    SOCKET src;
    int x, y, z;
    float w, qx, qy, qz;
    int hasRot;
    DWORD lastMs;
};
PosRelay g_pos[kMaxOnline];
CRITICAL_SECTION g_posLock;

// Shared world clock (seconds since the authority's in-game day start). Clients
// that read their in-game time report it; the server echoes it to everyone.
volatile DWORD g_clockSec = 0;
volatile DWORD g_clockLastMs = 0;

void PosStore(const char* name, int x, int y, int z, SOCKET src,
              const float* rot) {
    EnterCriticalSection(&g_posLock);
    int freeSlot = -1;
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_pos[i].name[0] == '\0') {
            if (freeSlot < 0) freeSlot = i;
            continue;
        }
        if (_stricmp(g_pos[i].name, name) == 0) {
            g_pos[i].src = src;
            g_pos[i].x = x;
            g_pos[i].y = y;
            g_pos[i].z = z;
            if (rot) {
                g_pos[i].w = rot[0]; g_pos[i].qx = rot[1];
                g_pos[i].qy = rot[2]; g_pos[i].qz = rot[3];
                g_pos[i].hasRot = 1;
            }
            g_pos[i].lastMs = GetTickCount();
            LeaveCriticalSection(&g_posLock);
            return;
        }
    }
    if (freeSlot >= 0) {
        strncpy(g_pos[freeSlot].name, name, sizeof(g_pos[freeSlot].name) - 1);
        g_pos[freeSlot].src = src;
        g_pos[freeSlot].x = x;
        g_pos[freeSlot].y = y;
        g_pos[freeSlot].z = z;
        if (rot) {
            g_pos[freeSlot].w = rot[0]; g_pos[freeSlot].qx = rot[1];
            g_pos[freeSlot].qy = rot[2]; g_pos[freeSlot].qz = rot[3];
            g_pos[freeSlot].hasRot = 1;
        }
        g_pos[freeSlot].lastMs = GetTickCount();
    }
    LeaveCriticalSection(&g_posLock);
}

// Broadcast the current position of every recently-seen character to all other
// live sessions. Stale entries are dropped so slots recycle.
void BroadcastPositions() {
    EnterCriticalSection(&g_posLock);
    EnterCriticalSection(&g_connLock);
    const DWORD now = GetTickCount();
    char line[256];
    for (int p = 0; p < kMaxOnline; p++) {
        if (g_pos[p].name[0] == '\0') continue;
        if (now - g_pos[p].lastMs > 20000) {
            g_pos[p].name[0] = '\0'; // stale: char gone; recycle slot
            continue;
        }
        if (g_pos[p].hasRot) {
            snprintf(line, sizeof(line),
                     "POS %s %d %d %d %.3f %.3f %.3f %.3f\r\n",
                     g_pos[p].name, g_pos[p].x, g_pos[p].y, g_pos[p].z,
                     g_pos[p].w, g_pos[p].qx, g_pos[p].qy, g_pos[p].qz);
        } else {
            snprintf(line, sizeof(line), "POS %s %d %d %d\r\n",
                     g_pos[p].name, g_pos[p].x, g_pos[p].y, g_pos[p].z);
        }
        for (int c = 0; c < kMaxOnline; c++) {
            if (g_conns[c].active && g_conns[c].user[0] != '\0') {
                if (g_conns[c].s == g_pos[p].src) continue; // skip sender
                if (send(g_conns[c].s, line, (int)strlen(line), 0) <= 0) {
                    g_conns[c].active = 0; // dead socket; drop
                }
            }
        }
    }
    LeaveCriticalSection(&g_connLock);
    LeaveCriticalSection(&g_posLock);
}

void BroadcastClock() {
    DWORD last = g_clockLastMs;
    if (!last) return;
    char line[64];
    snprintf(line, sizeof(line), "CLOCK %lu\r\n", (unsigned long)g_clockSec);
    EnterCriticalSection(&g_connLock);
    for (int c = 0; c < kMaxOnline; c++) {
        if (g_conns[c].active && g_conns[c].user[0] != '\0') {
            if (send(g_conns[c].s, line, (int)strlen(line), 0) <= 0) {
                g_conns[c].active = 0;
            }
        }
    }
    LeaveCriticalSection(&g_connLock);
}

// Relay a one-shot event line (e.g. a combat state change) to every other
// logged-in session, skipping the sender. No persistence, no reply: mirrors
// the live RPOS relay but for discrete state transitions.
void RelayEvent(const char* line, SOCKET src) {
    EnterCriticalSection(&g_connLock);
    for (int c = 0; c < kMaxOnline; c++) {
        if (g_conns[c].active && g_conns[c].user[0] != '\0') {
            if (g_conns[c].s == src) continue;
            if (send(g_conns[c].s, line, (int)strlen(line), 0) <= 0) {
                g_conns[c].active = 0;
            }
        }
    }
    LeaveCriticalSection(&g_connLock);
}

DWORD WINAPI RelayLoop(LPVOID) {
    for (;;) {
        Sleep(1000);
        BroadcastPositions();
        BroadcastClock();
    }
    return 0;
}

void ConnRegister(SOCKET s) {
    EnterCriticalSection(&g_connLock);
    for (int i = 0; i < kMaxOnline; i++) {
        if (!g_conns[i].active) {
            g_conns[i].s = s;
            g_conns[i].user[0] = '\0';
            g_conns[i].active = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_connLock);
}

void ConnSetUser(SOCKET s, const char* user) {
    EnterCriticalSection(&g_connLock);
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_conns[i].active && g_conns[i].s == s) {
            strncpy(g_conns[i].user, user, sizeof(g_conns[i].user) - 1);
            break;
        }
    }
    LeaveCriticalSection(&g_connLock);
}

// Record which in-world character a session commands. Relayed to peers as
// "PLAYER <user> <char>" so every client can tell whose avatar is whose.
void ConnSetPlayerChar(SOCKET s, const char* name) {
    char user[128] = {};
    const char* pc = name ? name : "";
    EnterCriticalSection(&g_connLock);
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_conns[i].active && g_conns[i].s == s) {
            strncpy(g_conns[i].pchar, pc, sizeof(g_conns[i].pchar) - 1);
            strncpy(user, g_conns[i].user, sizeof(user) - 1);
            break;
        }
    }
    LeaveCriticalSection(&g_connLock);
    if (user[0]) kmmo::srvlog::Info("player %s commands '%s'", user, pc);
    else kmmo::srvlog::Info("anon socket commands '%s'", pc);
}

// Copy the account name bound to a live session socket into `out`.
void ConnUserFetch(SOCKET s, char* out, int cap) {
    out[0] = '\0';
    EnterCriticalSection(&g_connLock);
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_conns[i].active && g_conns[i].s == s) {
            strncpy(out, g_conns[i].user, cap - 1);
            out[cap - 1] = '\0';
            break;
        }
    }
    LeaveCriticalSection(&g_connLock);
}

void ConnUnregister(SOCKET s) {
    EnterCriticalSection(&g_connLock);
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_conns[i].active && (g_conns[i].s == s || g_conns[i].s == INVALID_SOCKET)) {
            g_conns[i].active = 0;
            g_conns[i].user[0] = '\0';
        }
    }
    LeaveCriticalSection(&g_connLock);
}

// Broadcast a QUICKSAVE to every active, logged-in game session.
void BroadcastQuicksave() {
    EnterCriticalSection(&g_connLock);
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_conns[i].active && g_conns[i].user[0] != '\0') {
            if (send(g_conns[i].s, "QUICKSAVE\r\n", 11, 0) <= 0) {
                g_conns[i].active = 0; // dead socket; drop
            }
        }
    }
    LeaveCriticalSection(&g_connLock);
}

void OnlineMark(const char* name) {
    EnterCriticalSection(&g_onlineLock);
    int freeSlot = -1;
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_online[i].name[0] == '\0') {
            if (freeSlot < 0) freeSlot = i;
            continue;
        }
        if (_stricmp(g_online[i].name, name) == 0) {
            g_online[i].lastLoginMs = GetTickCount();
            LeaveCriticalSection(&g_onlineLock);
            return;
        }
    }
    if (freeSlot >= 0) {
        strncpy(g_online[freeSlot].name, name, sizeof(g_online[freeSlot].name) - 1);
        g_online[freeSlot].lastLoginMs = GetTickCount();
    }
    LeaveCriticalSection(&g_onlineLock);
}

// Returns true if the user authenticated on some connection recently.
bool OnlineCheck(const char* name) {
    EnterCriticalSection(&g_onlineLock);
    const DWORD now = GetTickCount();
    bool ok = false;
    for (int i = 0; i < kMaxOnline; i++) {
        if (g_online[i].name[0] == '\0') continue;
        if (_stricmp(g_online[i].name, name) != 0) continue;
        // Login grants a wide window: the player may spend minutes at the menu
        // before their character exists in the world.
        if (now - g_online[i].lastLoginMs < 900000) {
            ok = true;
        } else {
            g_online[i].name[0] = '\0'; // stale
        }
        break;
    }
    LeaveCriticalSection(&g_onlineLock);
    return ok;
}

void SendLine(SOCKET s, const char* line) {
    send(s, line, static_cast<int>(strlen(line)), 0);
}

// Max chunk payload size for save transfer (bounded, preallocated).
static const int kMaxChunk = 1 << 20; // 1 MiB
static char g_chunkBuf[kMaxChunk];

void SendResponse(SOCKET s, const char* status, const char* msg) {
    char out[512];
    snprintf(out, sizeof(out), "%s %s\r\n", status, msg);
    SendLine(s, out);
}

// Server-authoritative spawn: the player's world entry point comes ONLY from
// the server-side save record (stored per account), never from a local save.
void SendSpawn(SOCKET s, const char* user) {
    int x = 0, y = 0, z = 0;
    if (!kmmo::accounts::GetSpawn(user, x, y, z)) {
        SendResponse(s, "ERR", "NO_SAVE");
        return;
    }
    char squads[64] = {};
    bool hasSquad = kmmo::accounts::GetSquad(user, squads, sizeof(squads));
    char msg[256];
    if (hasSquad) {
        snprintf(msg, sizeof(msg), "SPAWN %d %d %d SQUAD %s", x, y, z, squads);
    } else {
        snprintf(msg, sizeof(msg), "SPAWN %d %d %d", x, y, z);
    }
    SendResponse(s, "OK", msg);
    kmmo::srvlog::Info("world spawn for %s: %d %d %d (server save, squad '%s')",
                       user, x, y, z, hasSquad ? squads : "");
}

int RecvLine(SOCKET s, char* out, int cap) {
    int pos = 0;
    while (pos + 1 < cap) {
        char c = 0;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') {
            out[pos] = '\0';
            return pos;
        }
        if (c != '\r') {
            out[pos++] = c;
        }
    }
    out[cap - 1] = '\0';
    return pos;
}

// Read exactly len bytes (binary-safe) from the socket.
int RecvExact(SOCKET s, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        int n = recv(s, p + got, (int)(len - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return (int)got;
}

// Send exactly len bytes (binary-safe). Returns 0 on success.
int SendExact(SOCKET s, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

void SplitTokens(char* line, char* toks[12], int& count) {
    count = 0;
    char* p = line;
    while (*p && count < 12) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        toks[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
}

DWORD WINAPI ClientWorker(LPVOID param) {
    SOCKET s = reinterpret_cast<SOCKET>(param);
    char buf[512];

    char peer[64] = "?";
    {
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        getpeername(s, reinterpret_cast<sockaddr*>(&from), &fromLen);
        inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
    }
    kmmo::srvlog::Info("client connected: %s", peer);
    InterlockedIncrement(&g_active);
    ConnRegister(s);

    bool loggedIn = false;
    (void)loggedIn;
    SendResponse(s, "OK", "KenshiMMO server v0.3 (shared world + combat). Type REGISTER <user> <pass> or LOGIN <user> <pass>");

    while (RecvLine(s, buf, sizeof(buf)) >= 0) {
        if (strlen(buf) == 0) continue;

        char* toks[12];
        int n = 0;
        SplitTokens(buf, toks, n);
        if (n == 0) continue;

        if (_stricmp(toks[0], "REGISTER") == 0 && n >= 3) {
            kmmo::AccountResult r = kmmo::accounts::Register(toks[1], toks[2]);
            switch (r) {
            case kmmo::AccountResult::Ok:
                SendResponse(s, "OK", "REGISTERED");
                kmmo::srvlog::Info("registered user %s", toks[1]);
                break;
            case kmmo::AccountResult::Exists:
                SendResponse(s, "ERR", "ACCOUNT_EXISTS");
                break;
            default:
                SendResponse(s, "ERR", "INVALID");
                break;
            }
        } else if (_stricmp(toks[0], "REGISTER") == 0) {
            SendResponse(s, "ERR", "REGISTER_NEEDS_CREDENTIALS");
        } else if (_stricmp(toks[0], "LOGIN") == 0 && n >= 3) {
            kmmo::Account acct;
            memset(&acct, 0, sizeof(acct));
            kmmo::AccountResult r = kmmo::accounts::Login(toks[1], toks[2], acct);
            switch (r) {
            case kmmo::AccountResult::Ok:
                loggedIn = true;
                OnlineMark(acct.user);
                ConnSetUser(s, acct.user);
                kmmo::srvlog::Info("user logged in: %s (%s)", acct.user, peer);
                SendResponse(s, "OK", "LOGIN_OK");
                break;
            case kmmo::AccountResult::NotFound:
                SendResponse(s, "ERR", "ACCOUNT_NOT_FOUND");
                break;
            case kmmo::AccountResult::BadPassword:
                SendResponse(s, "ERR", "BAD_PASSWORD");
                break;
            default:
                SendResponse(s, "ERR", "INVALID");
                break;
            }
        } else if (_stricmp(toks[0], "ENTER_WORLD") == 0 && n >= 2) {
            if (OnlineCheck(toks[1])) {
                // Mark this socket as a live session so the autosave heartbeat
                // (BroadcastQuicksave) reaches it. LOGIN/RESUME do this too;
                // without it the persistent connection never receives a single
                // QUICKSAVE and the client can never upload its save.
                ConnSetUser(s, toks[1]);
                kmmo::srvlog::Info("user entered world: %s (%s)", toks[1], peer);
                SendSpawn(s, toks[1]);
            } else {
                SendResponse(s, "ERR", "NOT_LOGGED_IN");
            }
        } else if (_stricmp(toks[0], "SAVE_POS") == 0 && n >= 5) {
            // Client persists the character's world position back to the
            // server-side save (the only save that counts) and feeds the
            // live position relay for the other clients.
            if (OnlineCheck(toks[1])) {
                int x = atoi(toks[2]), y = atoi(toks[3]), z = atoi(toks[4]);
                PosStore(toks[1], x, y, z, s, nullptr);
                if (kmmo::accounts::SavePos(toks[1], x, y, z)) {
                    kmmo::srvlog::Info("world save for %s: %d %d %d", toks[1], x, y, z);
                    SendResponse(s, "OK", "SAVED");
                } else {
                    SendResponse(s, "ERR", "NO_ACCOUNT");
                }
            } else {
                SendResponse(s, "ERR", "NOT_LOGGED_IN");
            }
        } else if (_stricmp(toks[0], "WHO") == 0) {
            // List the online roster (users logged in within the grace window).
            char msg[512] = "ONLINE 0";
            const DWORD now = GetTickCount();
            EnterCriticalSection(&g_onlineLock);
            int used = 0;
            for (int i = 0; i < kMaxOnline; i++) {
                if (g_online[i].name[0] == '\0') continue;
                if (now - g_online[i].lastLoginMs >= 900000) {
                    g_online[i].name[0] = '\0'; // stale
                    continue;
                }
                used++;
            }
            snprintf(msg, sizeof(msg), "ONLINE %d", used);
            int names = 0;
            size_t off = strlen(msg);
            for (int i = 0; i < kMaxOnline; i++) {
                if (g_online[i].name[0] == '\0') continue;
                if (now - g_online[i].lastLoginMs >= 900000) continue;
                if (names++ >= 24 || off >= sizeof(msg) - 96) break;
                snprintf(msg + off, sizeof(msg) - off, " %s", g_online[i].name);
                off = strlen(msg);
            }
            LeaveCriticalSection(&g_onlineLock);
            SendResponse(s, "OK", msg);
        } else if (_stricmp(toks[0], "TIME") == 0 && n >= 2) {
            // Client reports the authoritative world clock (seconds).
            g_clockSec = (DWORD)strtoul(toks[1], nullptr, 10);
            g_clockLastMs = GetTickCount();
            SendResponse(s, "OK", "CLOCK_SET");
        } else if (_stricmp(toks[0], "TIME?") == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "CLOCK %lu", (unsigned long)g_clockSec);
            SendResponse(s, "OK", msg);
        } else if (_stricmp(toks[0], "RPOS") == 0 && n >= 5) {
            // High-frequency live position for an arbitrary in-world character
            // (keyed by character name, not account). Optional rotation:
            // "RPOS <name> <x> <y> <z> [<w> <qx> <qy> <qz>]". No persistence,
            // no reply, sender skipped.
            if (loggedIn) {
                float rot[4] = { 0, 0, 0, 0 };
                const float* rptr = nullptr;
                if (n >= 9) {
                    rot[0] = (float)atof(toks[5]);
                    rot[1] = (float)atof(toks[6]);
                    rot[2] = (float)atof(toks[7]);
                    rot[3] = (float)atof(toks[8]);
                    rptr = rot;
                }
                PosStore(toks[1], atoi(toks[2]), atoi(toks[3]), atoi(toks[4]), s, rptr);
            }
        } else if (_stricmp(toks[0], "SETPLAYER") == 0 && n >= 2) {
            // Ownership: declare which in-world character this session commands.
            // No reply (mirrors RPOS); relayed to all other sessions so they can
            // flag shared/overlapping avatars and apply the right broadcasts.
            if (loggedIn) {
                ConnSetPlayerChar(s, toks[1]);
                char usr[128] = {};
                ConnUserFetch(s, usr, sizeof(usr));
                char pl[200];
                snprintf(pl, sizeof(pl), "PLAYER %s %s\r\n", usr, toks[1]);
                RelayEvent(pl, s);
            }
        } else if (_stricmp(toks[0], "CEVT") == 0 && n >= 3) {
            // Combat event relay: "<character> <state>" (1=down, 2=dead).
            // One-shot: relayed to all other sessions, sender skipped, not
            // stored or acknowledged, mirroring RPOS.
            if (loggedIn) {
                int cst = atoi(toks[2]);
                if (cst >= 1 && cst <= 2) {
                    char evLine[256];
                    snprintf(evLine, sizeof(evLine), "CEVT %s %d\r\n", toks[1], cst);
                    kmmo::srvlog::Info("combat event %s -> %d", toks[1], cst);
                    RelayEvent(evLine, s);
                }
            }
        } else if (_stricmp(toks[0], "SAVE_UPLOAD_BEGIN") == 0 && n >= 2) {
            // World blob upload open: resets the shared authority world blob.
            if (loggedIn && kmmo::accounts::WriteSaveBegin(
                    kmmo::accounts::SharedBlobUser())) {
                SendResponse(s, "OK", "READY");
            } else {
                SendResponse(s, "ERR", "OPEN_FAILED");
            }
        } else if (_stricmp(toks[0], "SAVE_UPLOAD") == 0 && n >= 4) {
            // SAVE_UPLOAD <user> <off> <len>\n then exactly len raw bytes.
            long long off = _strtoi64(toks[2], nullptr, 10);
            long long len = _strtoi64(toks[3], nullptr, 10);
            if (len < 0 || len > kMaxChunk) {
                SendResponse(s, "ERR", "BAD_LEN");
                break;
            }
            if (RecvExact(s, g_chunkBuf, (size_t)len) != (int)len) {
                return 0;
            }
            long long wrote = kmmo::accounts::WriteSave(
                kmmo::accounts::SharedBlobUser(), off, g_chunkBuf, len);
            if (wrote == len) {
                SendResponse(s, "OK", "CHUNK");
            } else {
                SendResponse(s, "ERR", "WRITE_FAILED");
            }
        } else if (_stricmp(toks[0], "SAVE_UPLOAD_END") == 0 && n >= 3) {
            long long size = _strtoi64(toks[2], nullptr, 10);
            kmmo::accounts::WriteSaveEnd(kmmo::accounts::SharedBlobUser(), size);
            SendResponse(s, "OK", "SAVED");
            kmmo::srvlog::Info("world blob received (%lld bytes)", size);
        } else if (_stricmp(toks[0], "SAVE_INFO") == 0 && n >= 2) {
            int64_t size = 0;
            if (kmmo::accounts::GetSaveSize(
                    kmmo::accounts::SharedBlobUser(), size)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "SAVESIZE %lld", (long long)size);
                SendResponse(s, "OK", msg);
            } else {
                SendResponse(s, "OK", "NOSAVE");
            }
        } else if (_stricmp(toks[0], "SAVE_DOWNLOAD_BEGIN") == 0 && n >= 2) {
            int64_t size = 0;
            if (kmmo::accounts::GetSaveSize(
                    kmmo::accounts::SharedBlobUser(), size)) {
                char msg[128];
                snprintf(msg, sizeof(msg), "READY %lld", (long long)size);
                SendResponse(s, "OK", msg);
            } else {
                SendResponse(s, "ERR", "NOSAVE");
            }
        } else if (_stricmp(toks[0], "SAVE_DOWNLOAD") == 0 && n >= 4) {
            // SAVE_DOWNLOAD <off> <len> -> OK CHUNK\n then raw bytes.
            long long off = _strtoi64(toks[2], nullptr, 10);
            long long len = _strtoi64(toks[3], nullptr, 10);
            if (len < 0 || len > kMaxChunk) {
                SendResponse(s, "ERR", "BAD_LEN");
                break;
            }
            long long got = kmmo::accounts::ReadSave(
                kmmo::accounts::SharedBlobUser(), off, g_chunkBuf, len);
            if (got < 0) {
                SendResponse(s, "ERR", "READ_FAILED");
                break;
            }
            char head[128];
            snprintf(head, sizeof(head), "OK CHUNK %lld\r\n", (long long)got);
            SendExact(s, head, strlen(head));
            SendExact(s, g_chunkBuf, (size_t)got);
        } else if (_stricmp(toks[0], "SAVE_DOWNLOAD_END") == 0) {
            SendResponse(s, "OK", "DONE");
        } else if (_stricmp(toks[0], "SAVE_CLEAR") == 0) {
            kmmo::accounts::ClearSave(kmmo::accounts::SharedBlobUser());
            SendResponse(s, "OK", "CLEARED");
        } else if (_stricmp(toks[0], "RESUME") == 0 && n >= 2) {
            // Reattach an existing session after a drop: no password check
            // (the socket is already post-login), just register the user so
            // it keeps receiving QUICKSAVE broadcasts.
            if (kmmo::accounts::HasUser(toks[1])) {
                loggedIn = true;
                OnlineMark(toks[1]);
                ConnSetUser(s, toks[1]);
                kmmo::srvlog::Info("session resumed for %s (%s)", toks[1], peer);
                SendResponse(s, "OK", "RESUME_OK");
            } else {
                SendResponse(s, "ERR", "UNKNOWN_USER");
            }
        } else if (_stricmp(toks[0], "SET_SQUAD") == 0 && n >= 3) {
            if (OnlineCheck(toks[1]) && kmmo::accounts::SetSquad(toks[1], toks[2])) {
                kmmo::srvlog::Info("squad for %s -> '%s'", toks[1], toks[2]);
                SendResponse(s, "OK", "SQUAD_SET");
            } else {
                SendResponse(s, "ERR", "NOT_LOGGED_IN");
            }
        } else if (_stricmp(toks[0], "QUIT") == 0) {
            SendResponse(s, "OK", "BYE");
            break;
        } else {
            SendResponse(s, "ERR", "BAD_COMMAND");
        }
    }

    closesocket(s);
    ConnUnregister(s);
    InterlockedDecrement(&g_active);
    kmmo::srvlog::Info("client disconnected: %s", peer);
    return 0;
}

DWORD WINAPI AcceptLoop(LPVOID) {
    kmmo::srvlog::Info("server listening on port %d", kPort);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        kmmo::srvlog::Info("failed to create listener, err=%d", WSAGetLastError());
        return 1;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        kmmo::srvlog::Info("bind failed err=%d", WSAGetLastError());
        return 1;
    }
    listen(listener, 16);

    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            kmmo::srvlog::Info("accept failed err=%d", WSAGetLastError());
            break;
        }
        HANDLE h = CreateThread(nullptr, 0, ClientWorker, reinterpret_cast<LPVOID>(client), 0, nullptr);
        if (h) CloseHandle(h);
    }
    closesocket(listener);
    return 0;
}

} // namespace

int main() {
    kmmo::srvlog::Init();
    kmmo::srvlog::Info("KenshiMMO.Server starting...");

    InitializeCriticalSection(&g_onlineLock);
    InitializeCriticalSection(&g_connLock);
    InitializeCriticalSection(&g_posLock);

    int loaded = kmmo::accounts::Load("accounts.dat");
    kmmo::srvlog::Info("loaded %d accounts", loaded);
    kmmo::accounts::SeedSharedBlobIfMissing("saves");

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        kmmo::srvlog::Info("WSAStartup failed");
        return 1;
    }

    HANDLE h = CreateThread(nullptr, 0, AcceptLoop, nullptr, 0, nullptr);
    if (!h) {
        kmmo::srvlog::Info("failed to start accept thread");
        return 1;
    }
    HANDLE hr = CreateThread(nullptr, 0, RelayLoop, nullptr, 0, nullptr);
    if (!hr) {
        kmmo::srvlog::Info("failed to start relay thread");
        return 1;
    }

    // Autosave heartbeat: ask every logged-in player to quicksave each minute.
    DWORD nextSave = GetTickCount() + 60000;
    for (;;) {
        Sleep(1000);
        DWORD now = GetTickCount();
        if (now >= nextSave) {
            nextSave = now + 60000;
            kmmo::srvlog::Info("autosave heartbeat: QUICKSAVE to all sessions");
            BroadcastQuicksave();
        }
    }
    return 0;
}
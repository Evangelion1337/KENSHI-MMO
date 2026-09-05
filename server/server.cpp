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
    volatile LONG active;
};
LiveConn g_conns[kMaxOnline];
CRITICAL_SECTION g_connLock;

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

void SplitTokens(char* line, char* toks[8], int& count) {
    count = 0;
    char* p = line;
    while (*p && count < 8) {
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
    SendResponse(s, "OK", "KenshiMMO server v0.1. Type REGISTER <user> <pass> or LOGIN <user> <pass>");

    while (RecvLine(s, buf, sizeof(buf)) >= 0) {
        if (strlen(buf) == 0) continue;

        char* toks[8];
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
            // server-side save (the only save that counts).
            if (OnlineCheck(toks[1])) {
                int x = atoi(toks[2]), y = atoi(toks[3]), z = atoi(toks[4]);
                if (kmmo::accounts::SavePos(toks[1], x, y, z)) {
                    kmmo::srvlog::Info("world save for %s: %d %d %d", toks[1], x, y, z);
                    SendResponse(s, "OK", "SAVED");
                } else {
                    SendResponse(s, "ERR", "NO_ACCOUNT");
                }
            } else {
                SendResponse(s, "ERR", "NOT_LOGGED_IN");
            }
        } else if (_stricmp(toks[0], "SAVE_UPLOAD_BEGIN") == 0 && n >= 2) {
            if (kmmo::accounts::WriteSaveBegin(toks[1])) {
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
            long long wrote = kmmo::accounts::WriteSave(toks[1], off, g_chunkBuf, len);
            if (wrote == len) {
                SendResponse(s, "OK", "CHUNK");
            } else {
                SendResponse(s, "ERR", "WRITE_FAILED");
            }
        } else if (_stricmp(toks[0], "SAVE_UPLOAD_END") == 0 && n >= 3) {
            long long size = _strtoi64(toks[2], nullptr, 10);
            kmmo::accounts::WriteSaveEnd(toks[1], size);
            SendResponse(s, "OK", "SAVED");
            kmmo::srvlog::Info("save blob received for %s (%lld bytes)", toks[1], size);
        } else if (_stricmp(toks[0], "SAVE_INFO") == 0 && n >= 2) {
            int64_t size = 0;
            if (kmmo::accounts::GetSaveSize(toks[1], size)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "SAVESIZE %lld", (long long)size);
                SendResponse(s, "OK", msg);
            } else {
                SendResponse(s, "OK", "NOSAVE");
            }
        } else if (_stricmp(toks[0], "SAVE_DOWNLOAD_BEGIN") == 0 && n >= 2) {
            int64_t size = 0;
            if (kmmo::accounts::GetSaveSize(toks[1], size)) {
                char msg[128];
                snprintf(msg, sizeof(msg), "READY %lld", (long long)size);
                SendResponse(s, "OK", msg);
            } else {
                SendResponse(s, "ERR", "NOSAVE");
            }
        } else if (_stricmp(toks[0], "SAVE_DOWNLOAD") == 0 && n >= 4) {
            // SAVE_DOWNLOAD <user> <off> <len> -> OK CHUNK\n then raw bytes.
            long long off = _strtoi64(toks[2], nullptr, 10);
            long long len = _strtoi64(toks[3], nullptr, 10);
            if (len < 0 || len > kMaxChunk) {
                SendResponse(s, "ERR", "BAD_LEN");
                break;
            }
            long long got = kmmo::accounts::ReadSave(toks[1], off, g_chunkBuf, len);
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
        } else if (_stricmp(toks[0], "SAVE_CLEAR") == 0 && n >= 2) {
            kmmo::accounts::ClearSave(toks[1]);
            SendResponse(s, "OK", "CLEARED");
        } else if (_stricmp(toks[0], "RESUME") == 0 && n >= 2) {
            // Reattach an existing session after a drop: no password check
            // (the socket is already post-login), just register the user so
            // it keeps receiving QUICKSAVE broadcasts.
            if (kmmo::accounts::HasUser(toks[1])) {
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

    int loaded = kmmo::accounts::Load("accounts.dat");
    kmmo::srvlog::Info("loaded %d accounts", loaded);

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
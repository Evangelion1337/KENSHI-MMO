#include "accounts.h"

#include "sha256.h"
#include "srvlog.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cctype>

namespace kmmo {
namespace accounts {

namespace {
const int kMaxAccounts = 1024;
// Standard wanderer spawn. A point inside the initially-loaded zone so the
// client can place the character without triggering a zone stream.
const int kDefaultSpawnX = -51200;
const int kDefaultSpawnY = 1580;
const int kDefaultSpawnZ = 3400;
Account g_accounts[kMaxAccounts];
int g_count = 0;
char g_path[MAX_PATH] = {};
CRITICAL_SECTION g_lock;
bool g_lockInit = false;

void EnsureLock() {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
}

int FindUser(const char* user) {
    for (int i = 0; i < g_count; i++) {
        if (_stricmp(g_accounts[i].user, user) == 0) {
            return i;
        }
    }
    return -1;
}

void Save() {
    if (g_path[0] == '\0') return;
    FILE* fp = fopen(g_path, "w");
    if (!fp) return;
    for (int i = 0; i < g_count; i++) {
        fprintf(fp, "%s;%s;%s;%d;%d;%d;%s\n",
                g_accounts[i].user, g_accounts[i].salt, g_accounts[i].hash,
                g_accounts[i].spawnX, g_accounts[i].spawnY, g_accounts[i].spawnZ,
                g_accounts[i].squad);
    }
    fclose(fp);
}

void SaveFolderFor(const char* user, char* out, size_t cap) {
    snprintf(out, cap, "saves/%s", user);
}

void SaveFileFor(const char* user, char* out, size_t cap) {
    snprintf(out, cap, "saves/%s/current.save", user);
}

// Ensure saves/ and saves/<user>/ exist. User names are validated at register
// (alnum/_/-), so they are safe to use directly in a path.
bool EnsureSaveFolder(const char* user) {
    CreateDirectoryA("saves", nullptr);
    char dir[MAX_PATH];
    SaveFolderFor(user, dir, sizeof(dir));
    BOOL ok = CreateDirectoryA(dir, nullptr);
    if (ok) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteSaveFile(const char* user, int x, int y, int z) {
    if (!EnsureSaveFolder(user)) return false;
    char path[MAX_PATH];
    SaveFileFor(user, path, sizeof(path));
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp, "%d %d %d\n", x, y, z);
    fclose(fp);
    return true;
}

bool ReadSaveFile(const char* user, int& x, int& y, int& z) {
    char path[MAX_PATH];
    SaveFileFor(user, path, sizeof(path));
    FILE* fp = fopen(path, "r");
    if (!fp) return false;
    int n = fscanf(fp, "%d %d %d", &x, &y, &z);
    fclose(fp);
    return n == 3;
}

void GenSalt(char saltOut[33]) {
    static const char* alphabet = "0123456789abcdef";
    BYTE buf[16];
    HCRYPTPROV prov = 0;
    if (CryptAcquireContext(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(prov, sizeof(buf), buf);
        CryptReleaseContext(prov, 0);
    } else {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        for (int i = 0; i < 16; i++) {
            buf[i] = static_cast<BYTE>((c.QuadPart >> (i % 8 * 8)) & 0xFF) ^
                     static_cast<BYTE>(GetTickCount() ^ i * 31);
        }
    }
    for (int i = 0; i < 16; i++) {
        saltOut[i * 2] = alphabet[buf[i] >> 4];
        saltOut[i * 2 + 1] = alphabet[buf[i] & 0xF];
    }
    saltOut[32] = '\0';
}
} // namespace

void BlobFileFor(const char* user, char* out, size_t cap) {
    snprintf(out, cap, "saves/%s/save.blob", user);
}

bool GetSaveSize(const char* user, int64_t& size) {
    char path[MAX_PATH];
    BlobFileFor(user, path, sizeof(path));
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long pos = ftell(fp);
    fclose(fp);
    if (pos < 0) return false;
    size = pos;
    return true;
}

int64_t ReadSave(const char* user, int64_t off, void* buf, int64_t len) {
    char path[MAX_PATH];
    BlobFileFor(user, path, sizeof(path));
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, (long)off, SEEK_SET) != 0) { fclose(fp); return -1; }
    int64_t got = (int64_t)fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    return got;
}

bool WriteSaveBegin(const char* user) {
    if (!EnsureSaveFolder(user)) return false;
    char path[MAX_PATH];
    BlobFileFor(user, path, sizeof(path));
    FILE* fp = fopen(path, "wb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

int64_t WriteSave(const char* user, int64_t off, const void* buf, int64_t len) {
    char path[MAX_PATH];
    BlobFileFor(user, path, sizeof(path));
    FILE* fp = fopen(path, "r+b");
    if (!fp) return -1;
    if (fseek(fp, (long)off, SEEK_SET) != 0) { fclose(fp); return -1; }
    int64_t wrote = (int64_t)fwrite(buf, 1, (size_t)len, fp);
    fclose(fp);
    return wrote;
}

void WriteSaveEnd(const char* user, int64_t size) {
    // Truncate to the given final size (blob may have been pre-sized larger).
    char path[MAX_PATH];
    BlobFileFor(user, path, sizeof(path));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, (LONG)size, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
}

void ClearSave(const char* user) {
    if (!EnsureSaveFolder(user)) return;
    char path[MAX_PATH];
    BlobFileFor(user, path, sizeof(path));
    DeleteFileA(path);
}

const char* SharedBlobUser() {
    return "shared";
}

void SeedSharedBlobIfMissing(const char* baseDir) {
    char sharedPath[MAX_PATH];
    BlobFileFor(SharedBlobUser(), sharedPath, sizeof(sharedPath));
    FILE* fp = fopen(sharedPath, "rb");
    if (fp) {
        fclose(fp);
        return; // shared blob already exists
    }

    // Adopt the largest legacy per-account blob as the authority world.
    char bestPath[MAX_PATH] = {};
    long bestSize = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", baseDir);
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        srvlog::Info("shared blob: no saves dir '%s' to seed from", baseDir);
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (_stricmp(fd.cFileName, SharedBlobUser()) == 0) continue;
        char blob[MAX_PATH];
        snprintf(blob, sizeof(blob), "%s\\%s\\save.blob", baseDir, fd.cFileName);
        FILE* b = fopen(blob, "rb");
        if (!b) continue;
        if (fseek(b, 0, SEEK_END) != 0) { fclose(b); continue; }
        long sz = ftell(b);
        fclose(b);
        if (sz > bestSize) {
            bestSize = sz;
            snprintf(bestPath, sizeof(bestPath), "%s", blob);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (bestSize <= 0 || !bestPath[0]) {
        srvlog::Info("shared blob: no legacy save found to seed");
        return;
    }

    if (!WriteSaveBegin(SharedBlobUser())) {
        srvlog::Info("shared blob: could not create shared save");
        return;
    }
    FILE* src = fopen(bestPath, "rb");
    if (!src) return;
    char buf[65536];
    long long off = 0;
    while (off < bestSize) {
        size_t want = (size_t)(bestSize - off);
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t got = fread(buf, 1, want, src);
        if (got == 0) break;
        WriteSave(SharedBlobUser(), off, buf, (int64_t)got);
        off += (int64_t)got;
    }
    fclose(src);
    WriteSaveEnd(SharedBlobUser(), off);
    srvlog::Info("shared blob: seeded from %s (%lld bytes)",
                 bestPath, (long long)off);
}

int Load(const char* path) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    strncpy(g_path, path, MAX_PATH - 1);
    g_path[MAX_PATH - 1] = '\0';
    g_count = 0;

    FILE* fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (g_count < kMaxAccounts && fgets(line, sizeof(line), fp)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;

            char* user = line;
            char* salt = strchr(line, ';');
            if (!salt) continue;
            *salt++ = '\0';
            char* hash = strchr(salt, ';');
            if (!hash) continue;
            *hash++ = '\0';

            Account& a = g_accounts[g_count];
            a = Account{};
            strncpy(a.user, user, sizeof(a.user) - 1);
            strncpy(a.salt, salt, sizeof(a.salt) - 1);
            strncpy(a.hash, hash, sizeof(a.hash) - 1);
            // Optional server-side save: ;x;y;z and optional ;squad
            char* sp = strchr(hash, ';');
            if (sp) {
                *sp++ = '\0';
                int n = sscanf(sp, "%d;%d;%d", &a.spawnX, &a.spawnY, &a.spawnZ);
                if (n < 3) {
                    a.spawnX = a.spawnY = a.spawnZ = 0;
                }
                // squad is the 4th token (everything after the third coord)
                char* third = sp;
                for (int k = 0; k < 2 && third; k++) third = strchr(third, ';');
                if (third && third[1] != '\0') {
                    strncpy(a.squad, third + 1, sizeof(a.squad) - 1);
                    char* nl = strpbrk(a.squad, "\r\n");
                    if (nl) *nl = '\0';
                }
            }
            g_count++;
        }
        fclose(fp);
    }
    LeaveCriticalSection(&g_lock);
    return g_count;
}

AccountResult Register(const char* user, const char* password) {
    EnsureLock();
    EnterCriticalSection(&g_lock);

    if (user[0] == '\0' || password[0] == '\0') {
        LeaveCriticalSection(&g_lock);
        return AccountResult::BadPassword;
    }
    for (const char* p = user; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            LeaveCriticalSection(&g_lock);
            return AccountResult::BadPassword;
        }
    }
    if (FindUser(user) >= 0) {
        LeaveCriticalSection(&g_lock);
        return AccountResult::Exists;
    }
    if (g_count >= kMaxAccounts) {
        LeaveCriticalSection(&g_lock);
        return AccountResult::BadPassword;
    }

    Account& a = g_accounts[g_count];
    a = Account{};
    strncpy(a.user, user, sizeof(a.user) - 1);
    GenSalt(a.salt);

    char salted[160];
    snprintf(salted, sizeof(salted), "%s%s", a.salt, password);
    Sha256Hex(salted, a.hash);
    // New characters are placed at the server's standard wanderer spawn
    // (server is always the source of truth; never the client's local save).
    a.spawnX = kDefaultSpawnX;
    a.spawnY = kDefaultSpawnY;
    a.spawnZ = kDefaultSpawnZ;
    g_count++;
    Save();

    LeaveCriticalSection(&g_lock);
    return AccountResult::Ok;
}

AccountResult Login(const char* user, const char* password, Account& out) {
    EnsureLock();
    EnterCriticalSection(&g_lock);

    int idx = FindUser(user);
    if (idx < 0) {
        LeaveCriticalSection(&g_lock);
        return AccountResult::NotFound;
    }

    const Account& a = g_accounts[idx];
    char salted[160];
    snprintf(salted, sizeof(salted), "%s%s", a.salt, password);
    char hash[65];
    Sha256Hex(salted, hash);

    if (strcmp(hash, a.hash) != 0) {
        LeaveCriticalSection(&g_lock);
        return AccountResult::BadPassword;
    }

    out = a;
    LeaveCriticalSection(&g_lock);
    return AccountResult::Ok;
}

bool HasUser(const char* user) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    bool ok = FindUser(user) >= 0;
    LeaveCriticalSection(&g_lock);
    return ok;
}

bool GetSpawn(const char* user, int& x, int& y, int& z) {
    EnsureLock();
    EnterCriticalSection(&g_lock);

    // Per-account save file wins; fall back to the legacy accounts.dat triplet.
    bool gotFile = ReadSaveFile(user, x, y, z);

    int idx = FindUser(user);
    if (idx < 0) {
        LeaveCriticalSection(&g_lock);
        return gotFile;
    }
    const Account& a = g_accounts[idx];
    if (!gotFile) {
        if (a.spawnX == 0 && a.spawnY == 0 && a.spawnZ == 0) {
            x = kDefaultSpawnX;
            y = kDefaultSpawnY;
            z = kDefaultSpawnZ;
        } else {
            x = a.spawnX;
            y = a.spawnY;
            z = a.spawnZ;
        }
    }
    LeaveCriticalSection(&g_lock);
    return true;
}

// Persist a character's world position as the server-side save for the user.
// Written to the account's own folder: saves/<username>/current.save.
bool SavePos(const char* user, int x, int y, int z) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    int idx = FindUser(user);
    if (idx < 0) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    g_accounts[idx].spawnX = x;
    g_accounts[idx].spawnY = y;
    g_accounts[idx].spawnZ = z;
    bool ok = WriteSaveFile(user, x, y, z);
    LeaveCriticalSection(&g_lock);
    return ok;
}

bool SetSquad(const char* user, const char* squad) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    int idx = FindUser(user);
    if (idx < 0) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    strncpy(g_accounts[idx].squad, squad ? squad : "", sizeof(g_accounts[idx].squad) - 1);
    Save();
    LeaveCriticalSection(&g_lock);
    return true;
}

bool GetSquad(const char* user, char* out, int cap) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    int idx = FindUser(user);
    bool ok = false;
    if (idx >= 0 && g_accounts[idx].squad[0] != '\0') {
        strncpy(out, g_accounts[idx].squad, (size_t)cap - 1);
        out[cap - 1] = '\0';
        ok = true;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

} // namespace accounts
} // namespace kmmo
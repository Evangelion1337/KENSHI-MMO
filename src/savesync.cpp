#include "savesync.h"

#include "log.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

namespace kmmo {
namespace savesync {
namespace {

const uint32_t kBlobMagic = 0x534D424B;     // 'KMBS'
const uint32_t kBlobVersion = 1;
const int kMaxFiles = 4096;
const uint64_t kMaxFileSize = 128ULL << 20; // 128 MiB per file
const uint64_t kMaxTotalSize = 512ULL << 20; // 512 MiB per save
const int kMaxPathLen = 512;

// UTF-8 narrow name -> wide string. Returns false if it does not fit at all.
bool NameToWide(const char* src, wchar_t* out, int cap) {
    if (!src || !out || cap <= 0) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, out, cap);
    if (n <= 0) return false;
    out[cap - 1] = L'\0';
    return true;
}

// Kenshi 1.0.x save root: %LOCALAPPDATA%\kenshi\save. The historic <exe>/save
// layout does not exist on modern builds; using it made the client wipe the
// wrong directory and never see the server-staged folder.
bool GameSaveDirW(wchar_t* out, int cap) {
    wchar_t root[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root,
                                      (DWORD)(sizeof(root) / sizeof(root[0]) - 1));
    if (n > 0 && root[0]) {
        _snwprintf(out, cap, L"%ls\\kenshi\\save", root);
        out[cap - 1] = L'\0';
        return out[0] != L'\0';
    }
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                         SHGFP_TYPE_CURRENT, root) == S_OK && root[0]) {
        _snwprintf(out, cap, L"%ls\\kenshi\\save", root);
        out[cap - 1] = L'\0';
        return true;
    }
    return false;
}

bool IsPlainName(const char* s) {
    if (!s[0] || strlen(s) > 96) return false;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c > 0x7F || c == '\\' || c == '/' || c == ':' || c == '.') return false;
    }
    return true;
}

// File names inside a slot may contain dots (quick.save, 1.save, ...) but must
// not smuggle path separators or drive refs.
bool IsEntryName(const char* s) {
    if (!s[0] || strlen(s) > 96) return false;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c > 0x7F || c == '\\' || c == '/' || c == ':') return false;
    }
    return true;
}

// ---- growing byte buffer -----------------------------------------------------
struct DynBuf {
    unsigned char* d = nullptr;
    size_t n = 0;
    size_t cap = 0;
};

bool DBEnsure(DynBuf& b, size_t need) {
    if (b.n + need <= b.cap) return true;
    size_t nc = b.cap ? b.cap : (1 << 20);
    while (nc < b.n + need) nc *= 2;
    unsigned char* p = static_cast<unsigned char*>(realloc(b.d, nc));
    if (!p) return false;
    b.d = p;
    b.cap = nc;
    return true;
}

bool DBPut(DynBuf& b, const void* src, size_t n) {
    if (!DBEnsure(b, n)) return false;
    memcpy(b.d + b.n, src, n);
    b.n += n;
    return true;
}

bool DBU32(DynBuf& b, uint32_t v) {
    unsigned char x[4];
    x[0] = (unsigned char)v;
    x[1] = (unsigned char)(v >> 8);
    x[2] = (unsigned char)(v >> 16);
    x[3] = (unsigned char)(v >> 24);
    return DBPut(b, x, 4);
}

bool DBU64(DynBuf& b, uint64_t v) {
    unsigned char x[8];
    for (int i = 0; i < 8; i++) x[i] = (unsigned char)(v >> (i * 8));
    return DBPut(b, x, 8);
}

// ---- blob reader -------------------------------------------------------------
struct Reader {
    const unsigned char* d;
    size_t n;
    size_t p;
};

bool RU32(Reader& r, uint32_t& v) {
    if (r.p + 4 > r.n) return false;
    v = (uint32_t)r.d[r.p] | ((uint32_t)r.d[r.p + 1] << 8) |
        ((uint32_t)r.d[r.p + 2] << 16) | ((uint32_t)r.d[r.p + 3] << 24);
    r.p += 4;
    return true;
}

bool RU64(Reader& r, uint64_t& v) {
    if (r.p + 8 > r.n) return false;
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r.d[r.p + i] << (i * 8);
    r.p += 8;
    return true;
}

// ---- recursive folder walk ---------------------------------------------------
struct PackCtx {
    DynBuf db;
    int files = 0;
    uint64_t total = 0;
};

bool WalkFolder(const wchar_t* wdir, const char* rel, PackCtx& ctx) {
    wchar_t pat[1600] = {};
    _snwprintf(pat, 1600, L"%ls\\*", wdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;

    do {
        if (ctx.files >= kMaxFiles) { FindClose(h); return false; }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        char nc[256] = {};
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nc,
                                sizeof(nc) - 1, nullptr, nullptr) <= 0) continue;
        if (!IsEntryName(nc)) continue;

        wchar_t full[1600] = {};
        _snwprintf(full, 1600, L"%ls\\%ls", wdir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char childRel[700] = {};
            snprintf(childRel, sizeof(childRel), "%s%s%s",
                     rel, rel[0] ? "/" : "", nc);
            if (!WalkFolder(full, childRel, ctx)) { FindClose(h); return false; }
            continue;
        }

        ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (sz == 0 || sz > kMaxFileSize || ctx.total + sz > kMaxTotalSize) continue;

        HANDLE f = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        unsigned char* data = static_cast<unsigned char*>(malloc((size_t)sz));
        if (!data) { CloseHandle(f); continue; }
        DWORD got = 0, remain = (DWORD)sz, off = 0;
        bool fullRead = true;
        while (off < (DWORD)sz) {
            if (!ReadFile(f, data + off, remain, &got, nullptr) || got == 0) {
                fullRead = false;
                break;
            }
            off += got;
            remain -= got;
        }
        CloseHandle(f);
        if (!fullRead) { free(data); continue; }

        char path[700] = {};
        snprintf(path, sizeof(path), "%s%s%s", rel, rel[0] ? "/" : "", nc);
        size_t pl = strlen(path);
        if (pl > kMaxPathLen) { free(data); continue; }
        if (!DBU32(ctx.db, (uint32_t)pl) || !DBPut(ctx.db, path, pl) ||
            !DBU64(ctx.db, sz) || !DBPut(ctx.db, data, (size_t)sz)) {
            free(data);
            FindClose(h);
            return false;
        }
        free(data);
        ctx.files++;
        ctx.total += sz;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// ---- path safety + mkdirs ----------------------------------------------------
// Validate a forward-slash relative blob path and rebuild it as a wide path
// under `base` (which is the save root + folder). Returns false if unsafe.
bool SanitizeRelPath(const char* rel, wchar_t* flat, int flatCap,
                     wchar_t* full, int fullCap, const wchar_t* base) {
    if (!rel[0] || strlen(rel) > kMaxPathLen) return false;
    // split on '/'
    const char* seg = rel;
    int flatLen = 0;
    for (;;) {
        const char* end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        if (len == 0 || (len == 1 && seg[0] == '.') || (len == 2 && seg[0] == '.' && seg[1] == '.'))
            return false;
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)seg[i] > 0x7F || seg[i] == ':' || seg[i] == '\\')
                return false;
        }
        if (flatLen > 0) {
            flat[flatLen++] = L'\\';
        }
        int w = MultiByteToWideChar(CP_UTF8, 0, seg, (int)len,
                                    flat + flatLen, flatCap - flatLen - 1);
        if (w <= 0) return false;
        flatLen += w;
        if (!end) break;
        seg = end + 1;
    }
    flat[flatLen] = L'\0';
    _snwprintf(full, fullCap, L"%ls\\%ls", base, flat);
    return true;
}

// Create every parent directory of `full`. Unpack already created the slot
// root, so the remaining unique ancestors are intermediate subfolders only.
bool MakeParentDirs(const wchar_t* full) {
    wchar_t cur[1600] = {};
    wcscpy(cur, full);
    for (wchar_t* slash = wcschr(cur, L'\\'); slash; slash = wcschr(slash + 1, L'\\')) {
        if (slash == cur) continue;
        *slash = L'\0';
        if (!CreateDirectoryW(cur, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
        *slash = L'\\';
    }
    return true;
}

bool WriteWholeFile(const wchar_t* full, const unsigned char* data, size_t len) {
    if (!MakeParentDirs(full)) return false;
    HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD off = 0;
    bool ok = true;
    while (off < len) {
        DWORD wrote = 0;
        if (!WriteFile(f, data + off, (DWORD)(len - off), &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    CloseHandle(f);
    return ok;
}

const DWORD kFindFlags = FILE_ATTRIBUTE_DIRECTORY;

// Recursively delete a directory tree without relying on shell32's
// SHFileOperation, which is flaky under some Wine setups.
bool RemoveDirTree(const wchar_t* path) {
    wchar_t pat[1600] = {};
    _snwprintf(pat, 1600, L"%ls\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            wchar_t full[1700] = {};
            _snwprintf(full, 1700, L"%ls\\%ls", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!RemoveDirTree(full)) ok = false;
            } else {
                if (!DeleteFileW(full)) ok = false;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = false;
    return ok;
}

} // namespace

int WipeLocalSaves() {
    wchar_t dir[1200] = {};
    if (!GameSaveDirW(dir, 1200)) {
        log::Warn("savesync: cannot resolve game save dir");
        return -1;
    }

    wchar_t pat[1400] = {};
    _snwprintf(pat, 1400, L"%ls\\*", dir);

    int removed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log::Info("savesync: save dir empty or absent (%lu)",
                  (unsigned long)GetLastError());
        return 0;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[1500] = {};
        _snwprintf(full, 1500, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (RemoveDirTree(full)) removed++;
        } else {
            if (DeleteFileW(full)) removed++;
        }
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);

    if (removed > 0) {
        char dirA[1200] = {};
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirA, sizeof(dirA) - 1, nullptr, nullptr);
        log::Info("savesync: wiped %d local save entries under '%s'", removed, dirA);
    }
    return removed;
}

int WipeOtherSlots(const char* keepA, const char* keepB) {
    wchar_t dir[1200] = {};
    if (!GameSaveDirW(dir, 1200)) return -1;

    wchar_t pat[1400] = {};
    _snwprintf(pat, 1400, L"%ls\\*", dir);

    wchar_t keepAW[128] = {};
    wchar_t keepBW[128] = {};
    if (keepA) NameToWide(keepA, keepAW, (int)(sizeof(keepAW) / sizeof(keepAW[0])));
    if (keepB) NameToWide(keepB, keepBW, (int)(sizeof(keepBW) / sizeof(keepBW[0])));

    int removed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L"_current1") == 0) continue;
        if (keepAW[0] && wcscmp(fd.cFileName, keepAW) == 0) continue;
        if (keepBW[0] && wcscmp(fd.cFileName, keepBW) == 0) continue;
        wchar_t full[1500] = {};
        _snwprintf(full, 1500, L"%ls\\%ls", dir, fd.cFileName);
        if (RemoveDirTree(full)) removed++;
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);

    if (removed > 0) {
        log::Info("savesync: pruned %d stale local save slot(s)", removed);
    }
    return removed;
}

const char* LatestSaveFolder() {
    static char s_name[128] = {};
    wchar_t dir[1200] = {};
    if (!GameSaveDirW(dir, 1200)) return nullptr;

    wchar_t pat[1400] = {};
    _snwprintf(pat, 1400, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    bool have = false;
    ULARGE_INTEGER best = {};
    char bestName[128] = {};
    do {
        if (!(fd.dwFileAttributes & kFindFlags)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t qf[1600] = {};
        _snwprintf(qf, 1600, L"%ls\\%ls\\quick.save", dir, fd.cFileName);
        WIN32_FIND_DATAW q;
        HANDLE hq = FindFirstFileW(qf, &q);
        if (hq == INVALID_HANDLE_VALUE) continue;
        ULARGE_INTEGER u;
        u.HighPart = q.ftLastWriteTime.dwHighDateTime;
        u.LowPart = q.ftLastWriteTime.dwLowDateTime;
        FindClose(hq);
        if (!have || u.QuadPart > best.QuadPart) {
            have = true;
            best = u;
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, bestName,
                                sizeof(bestName) - 1, nullptr, nullptr);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!have) return nullptr;
    strncpy(s_name, bestName, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    return s_name[0] ? s_name : nullptr;
}

bool FolderHasWorld(const char* folderName) {
    if (!folderName || !IsPlainName(folderName)) return false;
    wchar_t dir[1200] = {};
    if (!GameSaveDirW(dir, 1200)) return false;
    wchar_t folderW[128] = {};
    if (!NameToWide(folderName, folderW, (int)(sizeof(folderW) / sizeof(folderW[0]))))
        return false;
    wchar_t qf[1400] = {};
    _snwprintf(qf, 1400, L"%ls\\%ls\\quick.save", dir, folderW);
    WIN32_FIND_DATAW q;
    HANDLE hq = FindFirstFileW(qf, &q);
    if (hq == INVALID_HANDLE_VALUE) return false;
    FindClose(hq);
    return true;
}

unsigned char* PackSaveFolder(const char* folderName, size_t* outLen) {
    if (outLen) *outLen = 0;
    if (!folderName || !IsPlainName(folderName)) {
        log::Warn("savesync: refuse to pack invalid folder name '%s'",
                  folderName ? folderName : "(null)");
        return nullptr;
    }
    wchar_t root[1200] = {};
    if (!GameSaveDirW(root, 1200)) return nullptr;
    wchar_t folderW[128] = {};
    if (!NameToWide(folderName, folderW, (int)(sizeof(folderW) / sizeof(folderW[0]))))
        return nullptr;
wchar_t dir[1400] = {};
    _snwprintf(dir, 1400, L"%ls\\%ls", root, folderW);

    DWORD attrs = GetFileAttributesW(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        log::Warn("savesync: save folder not found: '%s'", folderName);
        return nullptr;
    }

    PackCtx ctx;
    if (!DBU32(ctx.db, kBlobMagic) || !DBU32(ctx.db, kBlobVersion)) return nullptr;

    size_t nlen = strlen(folderName);
    if (!DBU32(ctx.db, (uint32_t)nlen) || !DBPut(ctx.db, folderName, nlen))
        return nullptr;

    // Reserve the file count in the header; patched once the walk is done so
    // the blob layout is: magic, ver, nlen, name, count, entries...
    size_t countPos = ctx.db.n;
    if (!DBU32(ctx.db, 0)) return nullptr;

    if (!WalkFolder(dir, "", ctx) || ctx.files <= 0) {
        free(ctx.db.d);
        log::Warn("savesync: pack failed (files=%d)", ctx.files);
        return nullptr;
    }
    ctx.db.d[countPos + 0] = (unsigned char)(ctx.files & 0xFF);
    ctx.db.d[countPos + 1] = (unsigned char)((ctx.files >> 8) & 0xFF);
    ctx.db.d[countPos + 2] = (unsigned char)((ctx.files >> 16) & 0xFF);
    ctx.db.d[countPos + 3] = (unsigned char)((ctx.files >> 24) & 0xFF);

    if (outLen) *outLen = ctx.db.n;
    log::Info("savesync: packed '%s' -> %d files, %llu bytes",
              folderName, ctx.files, (unsigned long long)ctx.total);
    return ctx.db.d;
}

int UnpackSaveBlob(const unsigned char* blob, size_t len,
                   char* outName, int outNameCap) {
    if (!blob || len < 12) return -1;
    Reader r = { blob, len, 0 };
    uint32_t magic = 0, ver = 0;
    if (!RU32(r, magic) || !RU32(r, ver) || magic != kBlobMagic || ver != kBlobVersion)
        return -1;
    uint32_t nlen = 0;
    if (!RU32(r, nlen) || nlen == 0 || nlen > 96) return -1;
    if (r.p + nlen > r.n) return -1;
    char fname[128] = {};
    memcpy(fname, r.d + r.p, nlen);
    fname[nlen] = '\0';
    r.p += nlen;
    if (!IsPlainName(fname)) return -1;

    if (outName && outNameCap > 0) {
        strncpy(outName, fname, outNameCap - 1);
        outName[outNameCap - 1] = '\0';
    }

    uint32_t count = 0;
    if (!RU32(r, count) || count == 0 || count > (uint32_t)kMaxFiles) return -1;

    wchar_t root[1200] = {};
    if (!GameSaveDirW(root, 1200)) return -1;
    CreateDirectoryW(root, nullptr); // ensure the save root exists

    wchar_t base[1500] = {};
    wchar_t fnameW[128] = {};
    if (!NameToWide(fname, fnameW, (int)(sizeof(fnameW) / sizeof(fnameW[0])))) return -1;
    _snwprintf(base, 1500, L"%ls\\%ls", root, fnameW);
    if (!CreateDirectoryW(base, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return -1;

    int written = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pl = 0;
        if (!RU32(r, pl) || pl == 0 || pl > (uint32_t)kMaxPathLen) return -1;
        if (r.p + pl > r.n) return -1;
        char rel[600] = {};
        memcpy(rel, r.d + r.p, pl);
        rel[pl] = '\0';
        r.p += pl;
        uint64_t sz = 0;
        if (!RU64(r, sz)) return -1;
        if (sz == 0 || sz > kMaxFileSize || total + sz > kMaxTotalSize) return -1;
        if (r.p + sz > r.n) return -1;

        wchar_t flat[700] = {}, full[1700] = {};
        if (!SanitizeRelPath(rel, flat, 700, full, 1700, base)) return -1;
        if (!WriteWholeFile(full, r.d + r.p, (size_t)sz)) return -1;
        r.p += (size_t)sz;
        total += sz;
        written++;
    }
    log::Info("savesync: unpacked '%s' -> %d files, %llu bytes",
              fname, written, (unsigned long long)total);
    return written;
}

} // namespace savesync
} // namespace kmmo
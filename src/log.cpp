#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace kmmo {
namespace log {

static FILE* g_file = nullptr;
static CRITICAL_SECTION g_mutex;
static bool g_ready = false;

static void EnsureInit() {
    if (g_ready) return;
    InitializeCriticalSection(&g_mutex);
    g_ready = true;
}

void Init() {
    EnsureInit();
    EnterCriticalSection(&g_mutex);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    char logPath[MAX_PATH] = {};
    snprintf(logPath, MAX_PATH, "%s\\KenshiMMO.log", path);
    g_file = fopen(logPath, "w");
    LeaveCriticalSection(&g_mutex);
}

void Close() {
    EnterCriticalSection(&g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    LeaveCriticalSection(&g_mutex);
}

static void VWrite(const char* level, const char* fmt, va_list args) {
    EnsureInit();
    // Never block forever: a crashed thread may have died while holding
    // g_mutex (or mid-write), which would otherwise stall every logger caller
    // and silently freeze the session worker. Try for ~1s, then skip the line.
    const DWORD deadline = GetTickCount() + 1000;
    while (TryEnterCriticalSection(&g_mutex) == 0) {
        if ((int)(GetTickCount() - deadline) >= 0) return;
        Sleep(1);
    }
    if (g_file) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_file, "[%02u:%02u:%02u.%03u][%s] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
        vfprintf(g_file, fmt, args);
        fprintf(g_file, "\n");
        fflush(g_file);
    }
    LeaveCriticalSection(&g_mutex);
}

void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VWrite("INFO", fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VWrite("WARN", fmt, args);
    va_end(args);
}

void Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VWrite("ERROR", fmt, args);
    va_end(args);
}

} // namespace log
} // namespace kmmo
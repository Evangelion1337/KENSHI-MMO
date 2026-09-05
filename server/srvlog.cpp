#include "srvlog.h"

#include <cstdarg>
#include <ctime>

namespace kmmo {
namespace srvlog {

static FILE* g_fp = nullptr;

void Init() {
    g_fp = fopen("KenshiMMO.Server.log", "w");
}

static void V(const char* level, const char* fmt, va_list args) {
    char stamp[64] = {};
    time_t t = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &t);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    printf("[%s][%s] ", stamp, level);
    vprintf(fmt, args);
    printf("\n");

    if (g_fp) {
        fprintf(g_fp, "[%s][%s] ", stamp, level);
        vfprintf(g_fp, fmt, args);
        fprintf(g_fp, "\n");
        fflush(g_fp);
    }
}

void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    V("INFO", fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    V("WARN", fmt, args);
    va_end(args);
}

} // namespace srvlog
} // namespace kmmo
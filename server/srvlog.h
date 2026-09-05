#pragma once

#include <cstdio>
#include <windows.h>

namespace kmmo {
namespace srvlog {

void Init();
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);

} // namespace srvlog
} // namespace kmmo
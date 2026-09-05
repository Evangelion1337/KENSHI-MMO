#pragma once

namespace kmmo {

namespace log {
void Init();
void Close();
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);
}

}
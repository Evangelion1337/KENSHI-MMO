#pragma once

#include <cstdint>
#include <cstddef>

namespace kmmo {

// SHA-256 over a single message. Hex digest is written (65 bytes incl. NUL).
void Sha256(const void* data, size_t len, char hexOut[65]);

// Convenience: hash a NUL-terminated C string.
void Sha256Hex(const char* str, char hexOut[65]);

} // namespace kmmo
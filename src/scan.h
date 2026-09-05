#pragma once

#include "mem.h"
#include <cstdint>

namespace kmmo {

namespace scan {

struct Sections {
    const uint8_t* text = nullptr;
    size_t textSize = 0;
    const uint8_t* rdata = nullptr;
    size_t rdataSize = 0;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
};

bool EnumerateSections(const mem::Module& mod, Sections& out);

// Find the absolute address of a literal byte string in memory.
uintptr_t FindString(const Sections& sec, const char* str, size_t len);

// Find the address of a LEA reg,[RIP+disp] instruction referencing a target.
uintptr_t FindStringXref(const Sections& sec, uintptr_t stringAddr);

// Resolve the start of the function containing a given code address,
// using .pdata first, then a bounded prologue scan. Fallback: scan backwards.
uintptr_t FindFunctionStart(const mem::Module& mod, uintptr_t addr, int maxLookback);

// Scan a function's code range for RIP-relative loads pointing into .data.
// Returns the (nth) target address of such loads.
uintptr_t FindGlobalLoad(const mem::Module& mod, const Sections& sec, uintptr_t fnStart, int nth);

} // namespace scan
} // namespace kmmo
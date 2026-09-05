#include "scan.h"

#include <windows.h>
#include <cstring>

namespace kmmo {
namespace scan {

bool EnumerateSections(const mem::Module& mod, Sections& out) {
    if (!mod.IsValid()) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod.image);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mod.image + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        size_t vs = sec->Misc.VirtualSize;
        uintptr_t va = mod.base + sec->VirtualAddress;
        if (memcmp(sec->Name, ".text", 5) == 0) {
            out.text = reinterpret_cast<const uint8_t*>(va);
            out.textSize = vs;
        } else if (memcmp(sec->Name, ".rdata", 6) == 0) {
            out.rdata = reinterpret_cast<const uint8_t*>(va);
            out.rdataSize = vs;
        } else if (memcmp(sec->Name, ".data", 5) == 0) {
            out.data = reinterpret_cast<const uint8_t*>(va);
            out.dataSize = vs;
        }
    }
    return out.text && out.rdata;
}

uintptr_t FindString(const Sections& sec, const char* str, size_t len) {
    if (!sec.rdata || len == 0) return 0;
    const uint8_t* start = sec.rdata;
    const uint8_t* end = sec.rdata + sec.rdataSize - len;
    for (const uint8_t* p = start; p < end; p++) {
        if (memcmp(p, str, len) == 0) {
            return reinterpret_cast<uintptr_t>(p);
        }
    }
    return 0;
}

uintptr_t FindStringXref(const Sections& sec, uintptr_t stringAddr) {
    if (!sec.text || sec.textSize < 7) return 0;
    const uint8_t* text = sec.text;
    for (size_t i = 0; i + 7 < sec.textSize; i++) {
        if ((text[i] != 0x48 && text[i] != 0x4C) || text[i + 1] != 0x8D) {
            continue;
        }
        uint8_t modrm = text[i + 2];
        if ((modrm >> 6) != 0 || (modrm & 7) != 5) continue;
        int32_t disp;
        memcpy(&disp, &text[i + 3], sizeof(disp));
        uintptr_t instrAddr = reinterpret_cast<uintptr_t>(&text[i]);
        uintptr_t target = instrAddr + 7 + static_cast<intptr_t>(disp);
        if (target == stringAddr) {
            return instrAddr;
        }
    }
    return 0;
}

namespace {
bool IsPrologue(const uint8_t* p) {
    if (p[0] == 0x48 && p[1] == 0x89 && (p[2] & 0xF8) == 0x40 && p[3] == 0x24) return true;
    if (p[0] == 0x4C && p[1] == 0x89 && (p[2] & 0xF8) == 0x40 && p[3] == 0x24) return true;
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return true;
    if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return true;
    if ((p[0] == 0x40 && (p[1] & 0xF8) == 0x50) || (p[0] == 0x53 && p[1] == 0x48)) return true;
    if (p[0] == 0x55 && p[1] == 0x48) return true;
    return false;
}
} // namespace

uintptr_t FindFunctionStart(const mem::Module& mod, uintptr_t addr, int maxLookback) {
    if (!mod.IsValid()) return 0;
    if (addr < mod.base || addr >= mod.base + mod.size) return 0;

    // .pdata (RtlLookupFunctionEntry) is authoritative on x64.
    DWORD64 imageBase = 0;
    auto* rf = RtlLookupFunctionEntry(
        static_cast<DWORD64>(addr), &imageBase, nullptr);
    if (rf) {
        uintptr_t start = static_cast<uintptr_t>(imageBase) + rf->BeginAddress;
        if (start != 0 && start < addr) {
            return start;
        }
    }

    // Bounded prologue walk backwards.
    uintptr_t low = (addr > static_cast<uintptr_t>(maxLookback))
                        ? addr - maxLookback
                        : mod.base;
    for (uintptr_t p = addr - 1; p > low; p--) {
        if (*reinterpret_cast<const uint8_t*>(p) == 0xCC ||
            *reinterpret_cast<const uint8_t*>(p) == 0xC3) {
            uintptr_t candidate = p + 1;
            while (*reinterpret_cast<const uint8_t*>(candidate) == 0xCC) {
                candidate++;
            }
            if (mem::IsReadable(candidate, 6) && IsPrologue(reinterpret_cast<const uint8_t*>(candidate))) {
                return candidate;
            }
        }
    }
    return 0;
}

uintptr_t FindGlobalLoad(const mem::Module& mod, const Sections& sec, uintptr_t fnStart, int nth) {
    if (!mod.IsValid() || fnStart == 0 || !sec.data) return 0;
    uintptr_t fnEnd = fnStart + 4096;
    DWORD64 imageBase = 0;
    auto* rf = RtlLookupFunctionEntry(static_cast<DWORD64>(fnStart), &imageBase, nullptr);
    if (rf) {
        uintptr_t end = static_cast<uintptr_t>(imageBase) + rf->EndAddress;
        if (end > fnStart && end < fnStart + 65536) {
            fnEnd = end;
        }
    }
    if (fnEnd > mod.base + mod.size) fnEnd = mod.base + mod.size;

    int found = 0;
    const uint8_t* code = reinterpret_cast<const uint8_t*>(fnStart);
    size_t len = fnEnd - fnStart;
    for (size_t i = 0; i + 7 < len; i++) {
        bool hasRex = (code[i] == 0x48 || code[i] == 0x4C);
        bool isMemOp = (code[i + 1] == 0x8B || code[i + 1] == 0x8D);
        if (!hasRex || !isMemOp) continue;
        uint8_t modrm = code[i + 2];
        if ((modrm >> 6) != 0 || (modrm & 7) != 5) continue;
        int32_t disp;
        memcpy(&disp, &code[i + 3], sizeof(disp));
        uintptr_t instrAddr = fnStart + i;
        uintptr_t target = instrAddr + 7 + static_cast<intptr_t>(disp);
        uintptr_t dataBase = reinterpret_cast<uintptr_t>(sec.data);
        if (target >= dataBase && target < dataBase + sec.dataSize) {
            if (found == nth) return target;
            found++;
        }
    }
    return 0;
}

} // namespace scan
} // namespace kmmo
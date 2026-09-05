#include "mem.h"

#include <windows.h>
#include <cstring>

namespace kmmo {
namespace mem {

bool FindModule(const char* name, Module& out) {
    HMODULE mod = GetModuleHandleA(name);
    if (!mod) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    out.base = base;
    out.size = nt->OptionalHeader.SizeOfImage;
    out.image = reinterpret_cast<uint8_t*>(base);
    return true;
}

bool IsReadable(uintptr_t addr, size_t len) {
    if (addr == 0 || len == 0) return false;
    if (addr >= 0x00007FFFFFFFFFFFULL) return false; // kernel range
    MEMORY_BASIC_INFORMATION mbi = {};
    SIZE_T res = VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
    if (res == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    DWORD prot = mbi.Protect;
    if (!(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        return false;
    }
    return true;
}

bool Read(uintptr_t addr, void* dst, size_t len) {
    if (!IsReadable(addr, len)) return false;
    memcpy(dst, reinterpret_cast<const void*>(addr), len);
    return true;
}

bool Write(uintptr_t addr, const void* src, size_t len) {
    if (!IsReadable(addr, len)) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    memcpy(reinterpret_cast<void*>(addr), src, len);
    VirtualProtect(reinterpret_cast<LPVOID>(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), len);
    return true;
}

} // namespace mem
} // namespace kmmo
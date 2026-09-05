#pragma once

#include <cstdint>
#include <cstddef>

namespace kmmo {

namespace mem {

struct Section {
    const char* name;
    uintptr_t rva;
    size_t size;
    uint8_t* data;
};

struct Module {
    uintptr_t base = 0;
    size_t size = 0;
    uint8_t* image = nullptr;

    bool IsValid() const { return base != 0 && image != nullptr; }
};

bool FindModule(const char* name, Module& out);
bool IsReadable(uintptr_t addr, size_t len);
bool Read(uintptr_t addr, void* dst, size_t len);
bool Write(uintptr_t addr, const void* src, size_t len);

template <typename T>
bool Read(uintptr_t addr, T& out) {
    return Read(addr, &out, sizeof(T));
}

} // namespace mem
} // namespace kmmo
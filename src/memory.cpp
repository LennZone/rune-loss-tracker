#include "memory.h"

uintptr_t GetModuleBase(const char* moduleName) {
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(moduleName));
}

// SEH wrapper — keeps __try away from C++ destructors
static bool SafeRead64(uintptr_t addr, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ResolvePointerChain(uintptr_t base, std::initializer_list<uintptr_t> offsets) {
    uintptr_t addr = base;
    size_t remaining = offsets.size();

    for (uintptr_t offset : offsets) {
        addr += offset;
        --remaining;
        if (remaining > 0) {
            if (!SafeRead64(addr, &addr)) return 0;
        }
    }
    return addr;
}

int32_t ReadInt32(uintptr_t address) {
    __try {
        return *reinterpret_cast<const int32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

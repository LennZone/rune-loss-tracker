#pragma once
#include <windows.h>
#include <cstdint>
#include <initializer_list>

uintptr_t GetModuleBase(const char* moduleName);

// Resolves a pointer chain starting at `base`.
// For offsets {A, B, C}: computes base+A, derefs, +B, derefs, +C.
// Returns address of the final value (last offset is NOT dereferenced).
// Returns 0 if any dereference causes an access violation.
uintptr_t ResolvePointerChain(uintptr_t base, std::initializer_list<uintptr_t> offsets);

int32_t ReadInt32(uintptr_t address);

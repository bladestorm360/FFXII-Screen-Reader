#pragma once

#include <Windows.h>
#include <cstdint>

// SEH-guarded raw memory reads. Game objects can be destructed asynchronously and
// pointer chains can dangle, so every dereference of a game address goes through one
// of these — an access violation returns false / nullptr instead of crashing.
// POD-only (no C++ objects in the __try scope, per SEH rules). Shared by the menu
// reader and the message reader so the guard logic lives in exactly one place.
namespace MemRead {

// Read a pointer at address `at`. Returns false (and leaves *out untouched) on fault.
inline bool SafeReadPtr(const void* at, void** out) {
    if (!at) return false;
    __try { *out = *reinterpret_cast<void* const*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Read the pointer stored at base+off. Returns nullptr on null base or fault.
inline void* PtrAt(void* base, uint32_t off) {
    if (!base) return nullptr;
    void* v = nullptr;
    return SafeReadPtr(reinterpret_cast<char*>(base) + off, &v) ? v : nullptr;
}

// The object's vtable/handler pointer at offset 0 (used as a runtime class identity).
inline void* Obj0(void* obj) { return PtrAt(obj, 0); }

inline bool SafeReadU8(void* base, uint32_t off, uint8_t* out) {
    if (!base) return false;
    __try { *out = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(base) + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool SafeReadU16(void* base, uint32_t off, uint16_t* out) {
    if (!base) return false;
    __try { *out = *reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(base) + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool SafeReadS16(void* base, uint32_t off, int16_t* out) {
    if (!base) return false;
    __try { *out = *reinterpret_cast<int16_t*>(reinterpret_cast<char*>(base) + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool SafeReadU32(void* base, uint32_t off, uint32_t* out) {
    if (!base) return false;
    __try { *out = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(base) + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool SafeReadU64(void* base, uint32_t off, uint64_t* out) {
    if (!base) return false;
    __try { *out = *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(base) + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Read a 32-bit float at base+off. Used by the navigation module for the player
// world-matrix translation (matrix+0x30/0x34/0x38) and yaw fields — all floats.
inline bool SafeReadF32(void* base, uint32_t off, float* out) {
    if (!base) return false;
    __try { *out = *reinterpret_cast<float*>(reinterpret_cast<char*>(base) + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Read an int at address `p` (no offset). Returns false on fault.
inline bool SafeReadInt(void* p, int* out) {
    if (!p) return false;
    __try { *out = *reinterpret_cast<int*>(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace MemRead

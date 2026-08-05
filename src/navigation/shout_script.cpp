#include "navigation/shout_script.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"

namespace ShoutScript {
namespace {

constexpr int      kSlots        = 5;
constexpr uint32_t kSlotStride   = 0x288;
constexpr uint32_t kOffEbpBase   = 0x00;
constexpr uint32_t kOffClass1    = 0x40;
constexpr uint32_t kOffClass0    = 0x48;
constexpr uint32_t kOffClass4    = 0x58;
constexpr uint32_t kOffClass5    = 0x60;
constexpr uint32_t kOffDescTable = 0x78;

// ⚠ THE RUNTIME BASE IS THE FILE BASE + 0x80, and getting that wrong is what made every earlier
// build fail to resolve a module. `mod[0]` does not address the file header: it addresses the
// SECTION DIRECTORY, which the file places at offset 0x80 and which announces itself with
// 0x8000000B. The 2026-08-05 raw dump settled it -- record +00 pointed at bytes `0B 00 00 80`,
// not at `EBP2`.
//
// Everything the loader reads then lines up, which is the confirmation. Read at the FILE base its
// header fields looked like zeros, which invited the wrong conclusion that a relocation pass filled
// them in at load time; read at base+0x80 they are all populated in the shipped file:
//     base+0x14 = 0x9BA0     the count table the loader walks
//     base+0x28 = 0x2DD30    the VARIABLE DESCRIPTOR TABLE  (mod[0x0F])
//     base+0x40 = 0x180      the class-3 storage base
// and the three name strings sit at base+0x90: "00/00 00:00", "naomif", "byu_a01.src".
constexpr uint32_t kEbpMagic     = 0x8000000B;   // section-directory magic, at the runtime base
constexpr uint32_t kEbpNameBlock = 0x90;         // stamp \0 author \0 <module>.src \0
constexpr uint32_t kEbpClass3Off = 0x40;         // class-3 base = ebpBase + *(u32*)(ebpBase+0x40)

// Element strides, indexed by descriptor elemType (0=u8 1=s8 2=u16 3=s16 4=u32 5=float).
// Only used to reject a type we have no reader for; the meter is a scalar so no array indexing
// arithmetic is needed.
bool ElemTypeKnown(uint8_t t) { return t <= 5; }

// Copy the module's name block and pull the third NUL-terminated string out of it.
// The block is bounded: a torn or non-EBP image yields an empty name rather than a walk off the end.
bool ReadSrcName(void* ebpBase, char* out, int cap) {
    if (!ebpBase || !out || cap <= 0) return false;
    out[0] = '\0';

    uint32_t magic = 0;
    if (!MemRead::SafeReadU32(ebpBase, 0, &magic) || magic != kEbpMagic) return false;

    char blob[128] = {};
    if (!MemRead::SafeReadBytes(reinterpret_cast<char*>(ebpBase) + kEbpNameBlock,
                                blob, sizeof(blob) - 1))
        return false;
    blob[sizeof(blob) - 1] = '\0';

    // three NUL-terminated strings: build stamp, author, source name
    const char* p = blob;
    const char* end = blob + sizeof(blob) - 1;
    for (int i = 0; i < 2; ++i) {
        while (p < end && *p) ++p;
        if (p >= end) return false;
        ++p;                               // step over the NUL
    }
    if (p >= end || !*p) return false;

    // The name must look like an authoring name, not arbitrary bytes that survived the walk.
    int n = 0;
    while (p < end && *p && n < cap - 1) {
        const char c = *p;
        const bool printable = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '-';
        if (!printable) return false;
        out[n++] = c;
        ++p;
    }
    out[n] = '\0';
    return n > 0;
}

void* SlotRecord(int slot) {
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return nullptr;
    return reinterpret_cast<char*>(base) + static_cast<uintptr_t>(slot) * kSlotStride;
}

} // namespace

// The engine's own "currently executing module" pointer. See the header.
constexpr uint32_t RVA_CURRENT_MODULE = 0x1F79D70;

Module FromRecord(void* rec) {
    Module m;
    if (!rec) return m;
    void* ebpBase = MemRead::PtrAt(rec, kOffEbpBase);
    char name[32] = {};
    if (!ReadSrcName(ebpBase, name, sizeof(name))) return m;
    const ShoutTable::Row* row = ShoutTable::ForSrcName(name);
    if (!row) return m;
    m.valid   = true;
    m.record  = rec;
    m.ebpBase = ebpBase;
    m.slot    = -1;
    m.row     = row;
    strncpy_s(m.srcName, sizeof(m.srcName), name, _TRUNCATE);
    return m;
}

Module FromCurrentModule() {
    void* slot = Hooks::ResolveRva(RVA_CURRENT_MODULE);
    if (!slot) return Module();
    void* rec = nullptr;
    if (!MemRead::SafeReadPtr(slot, &rec) || !rec) return Module();
    return FromRecord(rec);
}

void DumpRecords() {
    Log::Write("SHOUT-RAW", "module records -- what is ACTUALLY at HANDLE_TABLE_BASE + slot*0x288");
    for (int slot = 0; slot < kSlots; ++slot) {
        void* rec = SlotRecord(slot);
        if (!rec) continue;
        uint64_t q[6] = {};
        for (int i = 0; i < 6; ++i) MemRead::SafeReadU64(rec, static_cast<uint32_t>(i * 8), &q[i]);
        char m[288];
        snprintf(m, sizeof(m),
                 "  slot %d rec=%p  +00=%016llX +08=%016llX +10=%016llX +18=%016llX +20=%016llX "
                 "+28=%016llX",
                 slot, rec, (unsigned long long)q[0], (unsigned long long)q[1],
                 (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
                 (unsigned long long)q[5]);
        Log::Write("SHOUT-RAW", m);

        // For each of the first three quadwords that looks like a user-mode pointer, show what it
        // addresses. An EBP2 image announces itself in the first four bytes.
        for (int i = 0; i < 3; ++i) {
            void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(q[i]));
            if (q[i] < 0x10000 || q[i] > 0x00007FFFFFFFFFFFULL) continue;
            uint8_t b[16] = {};
            if (!MemRead::SafeReadBytes(p, b, sizeof(b))) continue;
            char hex[128]; int off = 0;
            for (int k = 0; k < 16; ++k)
                off += snprintf(hex + off, sizeof(hex) - static_cast<size_t>(off), "%02X ", b[k]);
            char asc[20];
            for (int k = 0; k < 16; ++k) asc[k] = (b[k] >= 32 && b[k] < 127) ? (char)b[k] : '.';
            asc[16] = '\0';
            snprintf(m, sizeof(m), "    +%02X -> %p : %s |%s|", i * 8, p, hex, asc);
            Log::Write("SHOUT-RAW", m);
        }
    }

    void* cmSlot = Hooks::ResolveRva(RVA_CURRENT_MODULE);
    void* cur = nullptr;
    if (cmSlot) MemRead::SafeReadPtr(cmSlot, &cur);
    char m[224];
    snprintf(m, sizeof(m), "  current-module global (RVA 0x%X) = %p", RVA_CURRENT_MODULE, cur);
    Log::Write("SHOUT-RAW", m);
    if (cur) {
        uint64_t q0 = 0;
        MemRead::SafeReadU64(cur, 0, &q0);
        uint8_t b[16] = {};
        const bool got = (q0 >= 0x10000 && q0 <= 0x00007FFFFFFFFFFFULL) &&
                         MemRead::SafeReadBytes(reinterpret_cast<void*>(static_cast<uintptr_t>(q0)),
                                                b, sizeof(b));
        char asc[20];
        for (int k = 0; k < 16; ++k) asc[k] = got && b[k] >= 32 && b[k] < 127 ? (char)b[k] : '.';
        asc[16] = '\0';
        snprintf(m, sizeof(m), "    its +00 = %016llX%s |%s|", (unsigned long long)q0,
                 got ? " ->" : " (not readable)", asc);
        Log::Write("SHOUT-RAW", m);
    }
}

Module FindShoutModule(char* outNames, int outNamesCap) {
    Module m;
    int used = 0;
    if (outNames && outNamesCap > 0) outNames[0] = '\0';

    for (int slot = 0; slot < kSlots; ++slot) {
        void* rec = SlotRecord(slot);
        if (!rec) continue;
        void* ebpBase = MemRead::PtrAt(rec, kOffEbpBase);

        char name[32] = {};
        const bool named = ReadSrcName(ebpBase, name, sizeof(name));

        if (outNames && outNamesCap > 0 && used < outNamesCap - 1) {
            used += snprintf(outNames + used, static_cast<size_t>(outNamesCap - used),
                             "%s[%d]=%s", used ? " " : "", slot, named ? name : "-");
            if (used >= outNamesCap) used = outNamesCap - 1;
        }
        if (!named || m.valid) continue;

        const ShoutTable::Row* row = ShoutTable::ForSrcName(name);
        if (!row) continue;

        m.valid   = true;
        m.record  = rec;
        m.ebpBase = ebpBase;
        m.slot    = slot;
        m.row     = row;
        strncpy_s(m.srcName, sizeof(m.srcName), name, _TRUNCATE);
    }
    return m;
}

bool VarAddress(const Module& m, uint8_t varIdx, void** outAddr, uint8_t* outElemType,
                uint32_t* outRawDesc) {
    if (outAddr) *outAddr = nullptr;
    if (outElemType) *outElemType = 0xFF;
    if (outRawDesc) *outRawDesc = 0;
    if (!m.valid || !m.record) return false;

    void* descTable = MemRead::PtrAt(m.record, kOffDescTable);
    if (!descTable) return false;

    uint32_t desc = 0;
    if (!MemRead::SafeReadU32(descTable, 4 + static_cast<uint32_t>(varIdx) * 8, &desc)) return false;
    if (outRawDesc) *outRawDesc = desc;

    const uint8_t  elemType = static_cast<uint8_t>(desc >> 28);
    const uint8_t  cls      = static_cast<uint8_t>((desc >> 24) & 7);
    const uint32_t byteOff  = desc & 0xFFFFFF;
    if (outElemType) *outElemType = elemType;
    if (!ElemTypeKnown(elemType)) return false;

    void* classBase = nullptr;
    switch (cls) {
        case 0: classBase = MemRead::PtrAt(m.record, kOffClass0); break;
        case 1: classBase = MemRead::PtrAt(m.record, kOffClass1); break;
        case 4: classBase = MemRead::PtrAt(m.record, kOffClass4); break;
        case 5: classBase = MemRead::PtrAt(m.record, kOffClass5); break;
        case 3: {
            uint32_t rel = 0;
            if (!m.ebpBase || !MemRead::SafeReadU32(m.ebpBase, kEbpClass3Off, &rel)) return false;
            classBase = reinterpret_cast<char*>(m.ebpBase) + rel;
            break;
        }
        // Class 2 is per-actor: the engine resolves it by CALLING mod[0x13] with a VM context that
        // does not exist outside a running native. There is no honest address to compute, so this
        // fails open rather than pointing somewhere plausible.
        default: return false;
    }
    if (!classBase) return false;

    if (outAddr) *outAddr = reinterpret_cast<char*>(classBase) + byteOff;
    return true;
}

bool ReadVar(void* addr, uint8_t elemType, int32_t* out) {
    if (!addr || !out) return false;
    switch (elemType) {
        case 0: { uint8_t  v = 0; if (!MemRead::SafeReadU8 (addr, 0, &v)) return false; *out = v; return true; }
        case 1: { uint8_t  v = 0; if (!MemRead::SafeReadU8 (addr, 0, &v)) return false; *out = static_cast<int8_t>(v);  return true; }
        case 2: { uint16_t v = 0; if (!MemRead::SafeReadU16(addr, 0, &v)) return false; *out = v; return true; }
        case 3: { int16_t  v = 0; if (!MemRead::SafeReadS16(addr, 0, &v)) return false; *out = v; return true; }
        case 4:
        case 5: { uint32_t v = 0; if (!MemRead::SafeReadU32(addr, 0, &v)) return false; *out = static_cast<int32_t>(v); return true; }
        default: return false;
    }
}

bool WriteVar(void* addr, uint8_t elemType, int32_t value) {
    if (!addr) return false;
    __try {
        switch (elemType) {
            case 0:
            case 1: *reinterpret_cast<uint8_t*>(addr)  = static_cast<uint8_t>(value);  return true;
            case 2:
            case 3: *reinterpret_cast<uint16_t*>(addr) = static_cast<uint16_t>(value); return true;
            case 4: *reinterpret_cast<uint32_t*>(addr) = static_cast<uint32_t>(value); return true;
            // A float meter would need the value converted, not bit-copied. The census says every
            // shout meter is an integer counter compared with OPGTE, so a float here means the
            // descriptor was misread -- decline rather than write a denormal into script state.
            case 5: return false;
            default: return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace ShoutScript

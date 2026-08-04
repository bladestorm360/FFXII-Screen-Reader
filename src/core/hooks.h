#pragma once

#include <cstdint>

// MinHook wrapper. RVA-first per project rules: all installs take an RVA
// (relative to FFXII_TZA.exe's image base), resolved here to an absolute
// address via GetModuleHandle(nullptr).
namespace Hooks {

bool Init();
void Shutdown();

// Install a hook by RVA. detour is the replacement function; original_out
// receives a trampoline pointer to call the unhooked function.
// Returns true on success. Failure is logged with the specific MinHook error.
// Duplicate-RVA installs log + return false (defensive).
bool Install(uint32_t rva, void* detour, void** original_out);

// Convenience for the common typed-pointer pattern. Usage:
//   typedef void(*Pfn_Renderer)(void*, uint32_t*);
//   static Pfn_Renderer s_original = nullptr;
//   Hooks::InstallTyped(0x121D40, &MyDetour, &s_original);
template <typename T>
bool InstallTyped(uint32_t rva, T detour, T* original_out) {
    return Install(rva, reinterpret_cast<void*>(detour),
                   reinterpret_cast<void**>(original_out));
}

// Uninstall a previously-installed hook. Idempotent.
bool Uninstall(uint32_t rva);

// Resolves rva to an absolute address using FFXII_TZA.exe's base. Useful for
// reading globals (e.g., the menu registry at RVA 0x216EA60) where we don't
// hook, just read.
void* ResolveRva(uint32_t rva);

// One line, at the end of deferred init, saying how many hooks were attempted and how many took.
//
// WHY THIS EXISTS. Install() has always logged each failure, but nothing ever logged a TOTAL, and
// no caller but CombatEvents::Init checked a return value. A tester ran a build in which MinHook
// ran out of trampoline slots at hook 63 of 66; the three that failed were the combat hooks, and
// the session looked completely normal unless you happened to grep for MH_CreateHook. The bug was
// hunted as a combat-log regression for two sessions. A partial-hook session must announce itself.
//
// Log only -- never speech. A failed hook is a diagnostic, and CLAUDE.md's "never speak filler"
// rule applies: the player has nothing to do with this information.
void LogInstallCensus();

} // namespace Hooks

#pragma once

// THE STARTUP-CRASH FIX -- ffgriever's "FF12 tkMalloc Fix", built into this DLL (S194, user-authorized
// 2026-09-21). Source: https://gitlab.com/ffgriever/ff12-tkmalloc, v0.2.2 (`e3f69b8`), BSD 2-Clause.
//
// WHAT GOES WRONG WITHOUT IT. FFXII is a 64-bit exe carrying PS2-era code that stores pointers as
// 32-bit integers, so every one of the game's tkMalloc memory pools has to sit below 2 GB. The game
// gets those pools from plain `malloc` early in startup and simply hopes they land low. Nothing
// guarantees it: a large driver shader cache, or a thread reserving address space first, pushes them
// above 2 GB and the game crashes -- at startup, loading a save, or crossing into a zone. Newer GPU
// drivers made that common. It is not our mod's bug, and it is not NVIDIA-specific.
//
// WHAT THE FIX DOES. Before the game's own code runs, reserve and commit one 288 MB block below 2 GB
// (0x10000000 if free, else the first free fit), make a dlmalloc mspace out of it, and point the
// game's pool allocations at that mspace instead of the CRT heap. 28 call instructions in the exe are
// retargeted, each checked byte for byte first. The game's own allocator then runs unchanged inside
// memory that is guaranteed to be low.
//
// WHY NEITHER OF FFGRIEVER'S OWN BUILDS COULD BE USED. The standalone build IS a `dinput8.dll`, and
// the module build needs the External File Loader, which is also a `dinput8.dll`. Both want the slot
// this mod occupies. So the fix is compiled into our proxy instead.
//
// WHAT IT DOES NOT TOUCH -- the question the user asked first. Every RVA this mod uses addresses the
// EXE IMAGE (code and static globals), and the image does not move: the fix relocates only the
// memory BEHIND the game's heap pools. Everything the mod reads from the heap it reaches through a
// pointer it read at runtime, so it follows the pool wherever it is. Audited S194: no hardcoded heap
// address anywhere in src\, no pointer-range test that 0x10000000..0x22000000 would fail (the only
// range test, `shout_script.cpp`'s user-mode bound, accepts it), and `MemRead` guards by SEH, not by
// address. MinHook's trampolines are allocated in Stage B, after this, and find other space.
//
// THE CHARTER -- a game-memory write, like sneak assist and the shout fill, so it is bounded:
//   1. It writes only the 28 rel32 operands in its table, and only after all 28 match the Steam
//      build byte for byte. Any mismatch -- another exe version, another mod's patch -- and it writes
//      NOTHING and the game runs exactly as unmodded.
//   2. It refuses to install if the game has already set up a pool (`DAT_022d89b8`, RVA 0x21B89B8,
//      the per-pool "set up" bitmask FUN_00369d30 ORs into). Redirecting a `free` after the pools came
//      from the CRT would free CRT memory into the mspace -- so "too late" means "do nothing".
//   3. It NEVER fails the DLL. ffgriever's DllMain returns FALSE on failure, which here would take the
//      whole screen reader down with it. Every failure leaves the game unmodded and says why in the
//      log.
//   4. Always on, no menu row. The player it exists for crashes before the F8 menu exists.
namespace TkmallocFix {

// DLL_PROCESS_ATTACH, FIRST -- before anything else in this DLL reserves address space. Loader-lock
// safe: VirtualQuery / VirtualAlloc / VirtualProtect only, no LoadLibrary, no logging (the logger is
// not up yet). The verdict is kept and written by LogReport.
void Install();

// Stage B, straight after Log::Init. Writes the install verdict and every pool the game has set up so
// far; pools set up after this (the game re-creates pool 11 at runtime) are logged as they happen.
void LogReport();

} // namespace TkmallocFix

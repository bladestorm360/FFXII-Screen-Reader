#pragma once
#include <cstdint>

// Reader for the License Board / Job system (field menu -> "Licenses"). Three surfaces:
//
//   * Character-select (FUN_00560910, menuCtx+0x158) -- a DEDICATED window-proc hook (this
//     surface does NOT route focus through the shared FUN_00247510 dispatch). Announces the
//     highlighted character on SHOW (entry) and on cursor moves: name, job(s), LP.
//   * Job-select ring (FUN_00557db0, menuCtx+0x320) -- reached via the shared FUN_00247510
//     msg-0x8000 focus (MenuReader::HookedDispatch delegates here via OnDispatchFocus).
//     Announces the job name on move; `o` reads the job description.
//   * License board grid (FUN_0055cd40, menuCtx+0x320) -- node moves via the shared
//     FUN_00247510 msg-0x8000 focus (its `val` is a POINTER to the focused cell). Announces
//     node name + status + LP cost; `o` reads the effect description. Board SHOW announces the
//     job + current LP via a dedicated proc hook. The STATUS is read off the focused cell's own
//     flag word (cell+0x18) -- the one word FUN_0055cd40's Confirm branch tests -- so the spoken
//     word and the game's accept/buzz decision cannot disagree. It answers ONE question, "will
//     Confirm do anything here": learned / can learn / (silence, when the prerequisites are not met
//     and Confirm is a no-op). Affordability is NOT spoken -- the cost is in the line, `U` gives the
//     total, and the game shows its own message on a node you cannot pay for.
//   * `U` key -> current LP (only while the license board is open; silent otherwise).
//
// CONTRACT (same as the other readers): read-only, SEH-guarded memory reads. The only game
// calls are pure getters -- FUN_0035d330 (name resolver), FUN_002f9860 (menu message books),
// FUN_00323600 (node status, kept as a LOG-ONLY cross-check against the cell flags -- nothing
// spoken depends on it any more) -- made on the game thread, exactly like ingame_menu_reader's
// ResolveDefName. Text is the game's own, decoded via GameText. No dedup; empty/invalid ->
// silent (never fabricate a label).
namespace LicenseReader {

bool Init();
void Shutdown();

// Delegated from MenuReader::HookedDispatch on a msg-0x8000 focus. Handles the job-select ring
// and the license board grid (told apart by obj[0]); `val` is the raw 64-bit focus argument
// (for the board it is a pointer to the focused cell). Returns true when `owner` is a license
// surface (so the dispatcher skips its own row/config handling). Runs on the game thread.
bool OnDispatchFocus(void* owner, uintptr_t val);

} // namespace LicenseReader

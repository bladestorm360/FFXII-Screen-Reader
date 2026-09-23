#pragma once

// THE PARTY LEADER ANNOUNCEMENT (S195, the user's: *"the mod should actually speak who has become the
// party leader when changed"*). Says "<name>, leader" the moment the game hands control to a
// different character -- the D-pad on the field, the party menu, the game's own hand-over when the
// leader falls, a story swap. One hook, because there is one writer.
//
// THE WRITER. `DAT_022c7fe0` (RVA 0x21A7FE0, NavRva::LEADER_HANDLE) is the controlled character's
// scene handle, and exactly ONE instruction in the exe stores a non-zero value into it: the commit in
// `FUN_00358bc0` (RVA 0x238BC0), which copies the staged handle (`DAT_022c7fe4`) into it when the stage flag
// `DAT_022c7ff8` is set. Every path that changes who the player controls stages the new handle
// through `FUN_003594b0` (flag := 1, staged := handle) and lands here -- the six writers of the leader
// index `W+0x5AA4` all end in `FUN_00326500`, which is one of the three stagers. The only other
// writers of the handle store 0 (`FUN_00358b40`, `FUN_003590e0`: teardown). No code takes the
// global's address. Hook the writer of the state, not a reader of it (`L-21`).
//
// WHAT IS A CHANGE. The commit runs from the main loop every frame and is a no-op unless something was
// staged, so the detour compares the handle before and after the original and does nothing else on
// the other ~all frames. A different handle is then spoken only if it is a DIFFERENT CHARACTER:
//   * from handle 0 (a map load, after teardown) -- the party spawning, not a change: remembered,
//     silent.
//   * the same character id as the last leader -- the actor was rebuilt under a new handle
//     (`FUN_003220e0` does this): silent.
// Both are transition tests on the game's own state, not a repeat filter: every real change of
// character is spoken, including a switch back.
//
// Every outcome logs one `PARTY` line, the silent ones included (`L-83`), so a log says which branch
// ran. Game thread (the main loop). Speaks with interrupt: the player usually just pressed for it.
namespace PartyLeader {

bool Init();
void Shutdown();

} // namespace PartyLeader

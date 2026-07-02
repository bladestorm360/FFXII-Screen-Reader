#pragma once

// Speaks the FFXII:TZA title command-menu options as they come into focus.
//
// The title menu is NOT the in-game menu system (MenuObserver / FUN_00241d40) —
// it is a distinct baked-sprite menu. Its option labels are pre-rendered glyph
// IMAGES in title_logo.tm2 (the game supplies no text string for them), so the
// labels are keyed off the focused cell's texture source-rect and mapped to a
// small catalog read from the game's own art. Confirmed live 2026-07-02.
//
// Hooks: FUN_003939b0 (title window handler — 0x8000 focus notify), FUN_00393950
// (per-row decorator — source of the cell table), FUN_00394070 (logo/press-start).
namespace TitleReader {

bool Init();
void Shutdown();

} // namespace TitleReader

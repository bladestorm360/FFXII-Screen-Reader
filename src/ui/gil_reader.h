#pragma once

// `g` key -> speak the party's gil total. Works anywhere a save/party is loaded (field, shop, menus);
// SILENT when none is (title screen), per the never-speak-filler rule. Memory-only read (no game call),
// so it is safe on the input thread -- exactly like the `U` License-Points key.
//
// `g` is free in this game's bindings AND in the mod's reserved keys (Docs/Controls.md).
namespace GilReader {

bool Init();       // registers the `g` hotkey callback
void Shutdown();
void Announce();   // read gil and speak it (also the registered `g` handler)

} // namespace GilReader

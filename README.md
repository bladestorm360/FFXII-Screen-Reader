# FFXII-Screen-Reader

## Purpose

Adds NVDA output, pathfinding, menu reading and other accessibility aides to Final Fantasy XII: The Zodiac Age.

## Known Issues

This is an early test build. The following are known — please report anything else you run into.

Multi-page dialogue is read all at once instead of a page at a time. Longer story and tutorial screens (for example Basch explaining the save crystal) arrive as one block of text.

Button prompts inside tutorial text are skipped. A line like "press [button] to save" reads as "press to save" — the button icon, and often the punctuation right after it, is dropped.

Exits do not say where they lead. They are found, listed, and can be routed to, but they read as "Exit" rather than naming the destination area.

The party status keys (4, 5, 6) and the target status key (semicolon) are new and have not yet been confirmed in play. If they say nothing at all, that is worth reporting.

NVDA's own keyboard commands do not work while the game has focus. FFXII takes exclusive control of the keyboard, which blocks NVDA's shortcuts. The mod's own keys and its speech are unaffected.

Not yet read: FMV movie subtitles and full-page Handbook tutorials (both are pre-rendered images, not text). The License Board, Gambit editor, Bestiary and Clan Primer screens are not implemented yet.

There is no combat log yet. Battle is readable through the target and party status keys.

## Install

Buy and install Final Fantasy XII: The Zodiac Age on Steam.

Find the game's x64 folder. By default this is:

C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\x64

If you installed to a Steam library on another drive, it is:

drive:\path to library\SteamLibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\x64

Copy dinput8.dll from the release zip into that x64 folder, alongside FFXII_TZA.exe.

Copy Tolk.dll and nvdaControllerClient64.dll into the same x64 folder. These are not included in the release — you supply them. Get them from the Tolk project (https://github.com/dkager/tolk), or reuse the copies from another screen-reader mod.

Start NVDA (or JAWS, Narrator, or another supported reader), then launch the game. The mod announces itself a few seconds after the game starts.

There is no configuration file to install — the mod writes its own on first launch.

### Compatibility

The mod ships as dinput8.dll. The FF12 External File Loader and FF12 Module Loader use that same filename, so they cannot be installed at the same time as this mod — installing this replaces them.

### If it does not speak

Check that Tolk.dll and nvdaControllerClient64.dll are in the x64 folder next to dinput8.dll and FFXII_TZA.exe. Without them the mod loads but stays silent.

Check that your screen reader is running before you launch the game.

Check the log the mod writes next to the game executable: FFXII-Screen-Reader-Latest.log. It records what loaded and what failed.

## Keys

### Game

- W, A, S, D: movement.
- Arrow keys: camera.
- I, K, J, L: menu cursor up, down, left, right. Numpad 8, 2, 4, 6 also work. Q and E also move left and right.
- Space or Enter: confirm.
- C or Backspace: cancel.
- F: battle menu.
- R: party menu.
- Left Ctrl: escape (flee from battle).
- Esc: pause game.
- M, Numpad Plus, or Z: display map.
- H, Numpad 0, or X: reset camera.
- Left Shift: toggle walk and run.
- 1, 2, 3: game speed — normal, double, quadruple.
- F1, F2, F3: game speed — normal, double, quadruple (same as 1, 2, 3).

### Mod

The mod reserves none of the game's keys. Every mod key is pressed on its own — no Shift, Ctrl or Alt.

#### Navigation

- Backtick (the key above Tab): rescan nearby objects and announce the current area.
- Left bracket: previous object.
- Right bracket: next object.
- Minus: previous object category.
- Equals: next object category.
- Slash: describe the selected object — name, direction, distance, and whether anything blocks the way.
- Backslash: turn-by-turn directions to the selected object.
- P: turn-by-turn directions to the target the game currently has selected.

Categories cycle with minus and equals: All, Exit, Save Crystal, Gate Crystal, Treasure, NPC, Interactables, Enemy.

Directions are given relative to the way you are facing — "north" means straight ahead, not world north.

#### Status

- 4: party slot 1 status — name, HP and MP with maximums.
- 5: party slot 2 status.
- 6: party slot 3 status.
- Semicolon: status of the target the game currently has selected. Works outside battle too, when you select something with the cursor keys.

An empty party slot says nothing.

#### Reading

- O: read the focused item's description or tooltip.
- T: repeat the last thing spoken.
- Apostrophe: write a diagnostic dump to the log. Useful when reporting a bug — it records what the mod can see around you.

# FFXII-Screen-Reader

## Purpose

Adds NVDA output, pathfinding, menu reading and other accessibility aides to Final Fantasy XII: The Zodiac Age.

## Known Issues

This is an early test build. The following are known — please report anything else you run into.

Multi-page dialogue is read all at once instead of a page at a time. Longer story and tutorial screens (for example Basch explaining the save crystal) arrive as one block of text.

Button prompts inside tutorial text are still skipped. A line like "press \[button] to save" reads as "press to save" — the button icon has no name the mod can read, so it is left out. The text around it should now be intact: this build fixes a decoding bug that also swallowed the punctuation after an icon and could leave a stray letter behind. If you still hear a sentence lose its ending, or a stray letter appear mid-word, that is worth reporting.

Party status and the combat log are both new in this build and have not been through much play yet. If a key says nothing at all, or names the wrong character, that is worth reporting.

Enemy HP is given as a percentage rather than a number. Party members and allies give real numbers. Libra to be implemented once I have access to the spell.

NVDA's own keyboard commands do not work while the game has focus. FFXII takes exclusive control of the keyboard, which blocks NVDA's shortcuts. The mod's own keys and its speech are unaffected.

Not yet read: FMV movie subtitles and full-page Handbook tutorials (both are pre-rendered images, not text). The License Board, Gambit editor, Bestiary and Clan Primer screens are not implemented yet.

The combat log holds the last 100 events and is not cleared between battles, so what you read back may run into the previous fight. This is deliberate — but it means Home does not necessarily land on the start of the battle you are in.

## Install

Buy and install Final Fantasy XII: The Zodiac Age on Steam.

Find the game's x64 folder. By default this is:

C:\\Program Files (x86)\\Steam\\steamapps\\common\\FINAL FANTASY XII THE ZODIAC AGE\\x64

If you installed to a Steam library on another drive, it is:

drive:\\path to library\\SteamLibrary\\steamapps\\common\\FINAL FANTASY XII THE ZODIAC AGE\\x64

Copy all three DLLs from the release zip — dinput8.dll, Tolk.dll and nvdaControllerClient64.dll — into that x64 folder, alongside FFXII\_TZA.exe. All three go in the same place.

Launch the game. The mod announces itself a few seconds after the game starts.

There is no configuration file to install — the mod writes its own on first launch.

### Compatibility

The mod ships as dinput8.dll. The FF12 External File Loader and FF12 Module Loader use that same filename, so they cannot be installed at the same time as this mod — installing this replaces them. This means for now other mods are likely not supported.

### If it does not speak

Check that Tolk.dll and nvdaControllerClient64.dll are in the x64 folder next to dinput8.dll and FFXII\_TZA.exe. Without them the mod loads but stays silent.

Check the log the mod writes next to the game executable: FFXII-Screen-Reader-Latest.log. It records what loaded and what failed.

## Keys

### Game

* W, A, S, D: movement, menu navigation.
* Arrow keys: camera, menu navigation.
* I, K, J, L: camera, menu navigation. 
* Q and E: move left and right when targeting.
* Space or Enter: confirm, battle menu.
* C or Backspace: cancel.
* F: battle menu.
* R: party menu.
* Left Ctrl: escape. This is a toggle,  pressing it plays a sound and locks the menus when in battle. so the battle menu and party menu will not open. Pressing it again plays another sound and unlocks them. If your menus suddenly stop opening, press Left Ctrl once.
* Esc: pause game. Note that alt tabbing out of the game window pauses the game automatically. To resume press backspace.
* M, Numpad Plus, or Z: display map.
* H, Numpad 0, or X: reset camera.
* Left Shift: toggle walk and run.
* 1, 2, 3: game speed — normal, double, quadruple.
* F1, F2, F3: game speed — normal, double, quadruple (same as 1, 2, 3).

### Mod

The mod reserves none of the game's keys. Every mod key is pressed on its own — no Shift, Ctrl or Alt.

#### Navigation

* &#x20;`: rescan nearby objects and announce the current area.
* \[: previous object.
* ]: next object.
* \-: previous object category.
* =: next object category.
* /: describe the selected object — name, direction, distance, and whether anything blocks the way.
* \\: turn-by-turn directions to the selected object.
* P: turn-by-turn directions to the target the game currently has selected.

#### Status

* 4: first party member — name, HP, MP, and any statuses such as Poison or Slow.
* 5: second party member.
* 6: third party member.
* 7: guest, when you have one.

These work on the field as well as in battle.

Semicolon: status of the active target. 

#### Combat log

The mod keeps a running log of the last 100 battle events. Important ones are spoken as they happen — a command that failed, someone knocked out or revived, a level up, loot, gil, an attack being nullified, a command category disabled, a back attack. Routine chatter such as every action announcement and every small heal is logged but not spoken.

* ,: Browse backward through the log.
* .: Browse forward through the log.
* Home: jump to the oldest event logged.
* End: jump to the newest event.

#### Reading

* O: read the focused item's description or tooltip.
* T: repeat the last thing spoken.
* Apostrophe: write a diagnostic dump to the log. Useful when reporting a bug — it records what the mod can see around you.


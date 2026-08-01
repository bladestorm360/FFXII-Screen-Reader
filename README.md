# FFXII-Screen-Reader

## Purpose

Adds NVDA output, pathfinding, menu reading and other accessibility aides to Final Fantasy XII: The Zodiac Age.

## Known Issues

This is an early test build. The following are known — please report anything else you run into.

Button prompts inside tutorial text are still skipped. A line like "press \[button] to save" reads as "press to save" — the button icon has no name the mod can read, so it is left out. The text around it should now be intact: this build fixes a decoding bug that also swallowed the punctuation after an icon and could leave a stray letter behind. If you still hear a sentence lose its ending, or a stray letter appear mid-word, that is worth reporting.

Pathfinding directions can change abruptly if the game moves the camera. Directions are given relative to the camera — "north" means the way an Up push sends you — because that is the only frame the stick can actually act in. The game moves the camera on its own: stepping onto a ledge or stairs, hugging a wall, and constantly in battle as it tracks your target. When it does, the same route is described from the new angle, so a route that was "northeast" can become "southwest" without you having gone wrong — the way you push the stick has changed with it. If directions reverse or swing suddenly, the camera moved: press the directions key again and follow the new reading. There is no way for the mod to prevent this without breaking battle targeting, which uses the same camera to lock onto enemies.

Walking close to a wall is the most common cause of that flip. In a narrow street, an alley or a doorway the game pulls the camera in tight and swings it around to keep you in shot, and it can turn a long way in a moment. Because directions are camera-relative, a route that was "keep going north" can become "go south" while you are still walking the same way down the same wall — nothing has gone wrong, the frame the directions are given in has rotated. If it happens, step away from the wall and press the directions key again: with the camera back behind you the reading settles. This is the game's own camera behaviour and the mod cannot stop it without breaking battle targeting, which locks onto enemies through that same camera.

Party status and the combat log are both new in this build and have not been through much play yet. If a key says nothing at all, or names the wrong character, that is worth reporting.

Enemy HP is given as a percentage rather than a number. Party members and allies give real numbers. Libra to be implemented once I have access to the spell.

NVDA's own keyboard commands work while the game is running. Earlier builds said here that they did not, because FFXII takes exclusive control of the keyboard — in play they do respond, so that note has been withdrawn. Why it changed is not established. If you find an NVDA command that does nothing while the game has focus, that is worth reporting.

Not yet read: FMV movie subtitles and full-page Handbook tutorials (both are pre-rendered images, not text). The Gambit editor, Bestiary and Clan Primer screens are not implemented yet.

The combat log holds the last 100 events and is not cleared between battles, so what you read back may run into the previous fight. This is deliberate — but it means Home does not necessarily land on the start of the battle you are in.

## Install

Buy and install Final Fantasy XII: The Zodiac Age on Steam.

Find the game's x64 folder. By default this is:

C:\\Program Files (x86)\\Steam\\steamapps\\common\\FINAL FANTASY XII THE ZODIAC AGE\\x64

If you installed to a Steam library on another drive, it is:

drive:\\path to library\\SteamLibrary\\steamapps\\common\\FINAL FANTASY XII THE ZODIAC AGE\\x64

Copy all four DLLs from the release zip — dinput8.dll, SDL3.dll, Tolk.dll and nvdaControllerClient64.dll — into that x64 folder, alongside FFXII\_TZA.exe. All four go in the same place.

**SDL3.dll is required, not optional.** The mod plays its own sounds through it, and it is loaded the moment the mod starts. If SDL3.dll is missing the game will fail to start rather than simply running without sound, and Windows will say very little about why. If you have copied the other files and the game will not launch at all, this is the first thing to check.

Launch the game. The mod announces itself a few seconds after the game starts.

There is no configuration file to install — the mod writes its own on first launch.

### Compatibility

The mod ships as dinput8.dll. The FF12 External File Loader and FF12 Module Loader use that same filename, so they cannot be installed at the same time as this mod — installing this replaces them. This means for now other mods are likely not supported.

### If the game will not start at all

Check that SDL3.dll is in the x64 folder next to dinput8.dll. The mod links against it directly, so a missing SDL3.dll stops the game from launching instead of just disabling the beacon.

### If it does not speak

Check that Tolk.dll and nvdaControllerClient64.dll are in the x64 folder next to dinput8.dll and FFXII\_TZA.exe. Without them the mod loads but stays silent.

Check the log the mod writes next to the game executable: FFXII-Screen-Reader-Latest.log. It records what loaded and what failed.

### If the beacon makes no sound

Speech working but no beacon means SDL3 loaded and the audio device did not open. The log records this under AUDIO, including the reason SDL gave. Check the beacon is switched on with `F11` or the `F8` menu.

## Keys

### Game

* W, A, S, D: movement, menu navigation.
* Arrow keys: camera, menu navigation.
* I, K, J, L: camera, menu navigation. 
* Q and E: move left and right when targeting.
* Space or Enter: confirm, battle menu.
* C or Backspace: cancel.
* F: battle menu.
* R: field menu (the game's own Controls screen calls this the "Party Menu").
* Left Ctrl: escape. This is a toggle,  pressing it plays a sound and locks the menus when in battle. so the battle menu and field menu will not open. Pressing it again plays another sound and unlocks them. If your menus suddenly stop opening, press Left Ctrl once.
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
* \\: turn-by-turn directions to the selected object, and start the audio beacon — see below. With Auto-walk switched on in the mod menu, this key also walks you there.
* P: turn-by-turn directions to the target the game currently has selected.
* F4: switch combat verbosity between Normal and Verbose — see the mod menu below.
* F5: switch between listing everything and listing only what the story has opened up. It says which mode it is in and how many objects are left. Everything is listed by default, so nothing is ever hidden unless you ask for it.
* F6: give the selected object your own name, taken from the clipboard.
* F11: turn the audio beacon on or off. It only responds to F11 on its own — hold Shift, Ctrl or Alt and it does nothing, so Shift+F11 stays yours for your screen reader.
* F10: turn sneak assist on or off — see the mod menu below.
* F8: open or close the mod menu.

#### The audio beacon

Turn-by-turn directions tell you the route once. The beacon keeps telling you, while you walk.

Press \\ as normal. As well as speaking the directions, the mod starts a repeating sound placed in the direction you need to walk — to the right if the route says northeast, hard left if it says west, and so on. It is the same direction as the spoken word, just given as a sound instead of a syllable. As you get closer the sound repeats faster, from about once a second up to five times a second when you are nearly there.

Each turn of the route is a point along the way. Reaching one moves the beacon on to the next without saying anything, and the repeats slow down again — so speeding up means you are getting close to the next turn, and suddenly slowing down means you just passed it. Reaching your actual destination plays the sound once at a higher pitch and then stops.

A sound behind you is quieter, duller, and about a fifth lower in pitch than one in front. Behind means the whole half of the world behind your shoulders, not just directly astern, and the three cues arrive together — so anything behind you sounds behind you, rather than sounding gradually more behind the further round it goes.

Left and right keep working all the way round. Something behind and to your left is played to your left and carries the behind cues on top, so you hear its bearing and which half it is in at the same time.

If you wander well off the route the mod quietly works out a new one and re-aims the beacon. It does not say anything when it does this — you were not asked for new directions. Press \\ any time to hear the route again and re-aim.

#### The target beacon

In a fight, the route beacon stands down and a second sound takes over: it follows whatever your party is attacking, moving as the enemy moves, so you can hear where it is without asking. If nothing is being attacked it stays silent rather than leading you somewhere in the middle of a fight. When the fight is over the route beacon comes back on the same leg it left off.

This is its own setting. It plays in battle whether or not you had a route running, so you can use it on its own, and switching the route beacon off does not switch it off too. Both beacons have their own volume, from 20% up to 100%. Neither volume goes all the way to silent — each beacon has its own on/off switch, so a beacon that is switched on is never silent for a reason you cannot hear.

F11 turns the route beacon off and on (F11 on its own — not with Shift, Ctrl or Alt held, so your screen-reader chords are untouched), and both beacons and both volumes are in the F8 menu. Your choices are remembered between sessions. Changing area stops the route beacon — the route belonged to the old map — but not the target beacon, which follows whatever you are fighting wherever you are.

#### Naming things yourself (F6)

Where several people or objects share one name the mod numbers them — Rabanastran 1, Rabanastran 2 — and those numbers now stay put, so the same person keeps the same number for as long as you are on that map, and again when you come back. F6 lets you replace that with words of your own: the innkeeper, quest guy, the stairs home.

Copy the name you want in any other program — Notepad, a browser, an editor — then select the object with \[ and ] and press F6. The mod reads back "Labelled" and your name, and from then on that is what it calls the object, in the list, in descriptions and in directions. Pressing F6 with an empty clipboard clears the name again.

Names are saved and survive reloads, area changes and closing the game. They are kept in your own user folder (AppData\\Local\\FFXII-Screen-Reader), not in the game folder, so nothing in the game install is touched.

The clipboard is used because the mod deliberately cannot read your typing — it never takes a key away from the game, which is what keeps it safe to leave installed. Copying text is the way around that.

Map exits cannot be named this way. They come from the map's own script rather than from an object in the world, so there is nothing to attach a name to; F6 stays silent on one.

#### Status

* 4: first party member — name, HP, MP, and any statuses such as Poison or Slow.
* 5: second party member.
* 6: third party member.
* 7: guest, when you have one.

These work on the field as well as in battle.

* g: how much gil the party is carrying.
* ;: the active target. In battle, its name and HP. Outside battle, what Confirm will act on where you are standing — "Talk: Montblanc", "Action: Save Crystal". Silent when nothing is in reach.

#### Status screen

The Status screen reads as you move through it — no mod key. On the Attributes page the arrow keys step through the character's attributes and status effects, up and down a line at a time and left and right by section, with Home and End jumping to either end. The Magicks and Technicks pages read from the game's own cursor as you move it.

#### Combat log

The mod keeps a running log of the last 100 battle events. Important ones are spoken as they happen — a command that failed, someone knocked out or revived, a level up, loot, gil, an attack being nullified, a command category disabled, a back attack. Routine chatter such as every small heal is logged but not spoken, and so is the blow-by-blow damage — that is what browsing the log is for.

* ,: Browse backward through the log.
* .: Browse forward through the log.
* Home: jump to the oldest event logged.
* End: jump to the newest event.

How much gets spoken aloud is up to you — see Combat verbosity below.

#### The mod menu (F8)

F8 opens the mod's own settings, and F8 again closes it. Up and Down move between settings, Left and Right change the one you are on, and O reads a description of it. The description changes with the setting, so it always tells you what the setting is doing now rather than what it could do.

One setting so far:

* Combat verbosity — Normal or Verbose. Normal speaks enemy defeat and EXP, party member low HP and KO, and loot drops. Verbose speaks everything Normal does, and also tells you when an enemy is readying an ability or beginning to cast a spell, which is your window to interrupt or move. Normal is the default.
* Audio beacon — On or Off. The repeating sound that leads you along the route, described under Navigation above. On is the default.
* Auto-walk — Off or On. With it On, the route key does not just speak the route and start the beacon — the mod walks your character along it. It stops the instant you touch a movement key, the instant a fight starts, when you arrive, when a menu opens, and after fifteen seconds of no progress, and it never starts walking again on its own — press the route key when you want it back. Off is the default. One honest limitation for controller players: the mod cannot see the stick, so moving the stick does not cancel it — tap any movement key on the keyboard, or switch it off in this menu.

* Sneak assist — Off or On. A few points in the story make you sneak past guards who catch you if you come too close, and being caught puts you back to the start of the sequence. With this On, the guards on those maps stop noticing you, so you can walk the route at your own pace instead of racing a timer you cannot see. It changes nothing anywhere else in the game, and nothing is saved by it — switch it off and the guards behave exactly as the game wrote them, immediately. Off is the default, and it cannot get stuck on: it switches itself off whenever you change area, and it starts every session off. F10 only does something while you are actually standing on one of those guarded maps — press it anywhere else and it stays quiet, because there is nothing there for it to do. If a sneaking sequence ever seems to stop making progress while this is On, switch it off and play that part normally.

F4 switches Combat verbosity, F11 switches the Audio beacon and F10 switches Sneak assist, all without opening the menu, so you can change any of them in the middle of a fight. Your choices are remembered between sessions.

Two things worth knowing. The mod cannot take keys away from the game, so while the menu is open the arrow keys still move your character — press F8 while standing still, or just use F4. And Verbose speaks the enemy's announcement when the game makes it: the game stays quiet when an enemy repeats the same ability on the same target, and when the caster is a long way off. That pacing is the game's own, not something the mod is hiding from you.

#### Reading

* O: read the focused item's description or tooltip.
* T: repeat the last thing spoken.
* U: current License Points, on the License Board.
* ': write a diagnostic dump to the log. Useful when reporting a bug — it records what the mod can see around you.


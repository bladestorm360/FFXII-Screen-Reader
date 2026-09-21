# FFXII-Screen-Reader

## Purpose

Adds NVDA output, pathfinding, menu reading and other accessibility aides to Final Fantasy XII: The Zodiac Age.

## Known Issues

The following are known and still open in this release — please report anything else you run into.

Button prompts inside tutorial text are skipped. A line like "press \[button] to save" reads as "press to save". The text around it should be intact — if you hear a sentence lose its ending, or a stray letter appear mid-word, that is worth reporting.

Pathfinding directions can change abruptly if the game moves the camera. Directions are given relative to the camera — "north" means the way an Up push sends you — because that is the only frame the stick can actually act in. The game moves the camera on its own: stepping onto a ledge or stairs, hugging a wall, and constantly in battle as it tracks your target. When it does, the same route is described from the new angle, so a route that was "northeast" can become "southwest" without you having gone wrong — the way you push the stick has changed with it. If directions reverse or swing suddenly, the camera moved: press the directions key again and follow the new reading. There is no way for the mod to prevent this without breaking battle targeting, which uses the same camera to lock onto enemies.

Walking close to a wall is the most common cause of that flip. In a narrow street, an alley or a doorway the game pulls the camera in tight and swings it around to keep you in shot, and it can turn a long way in a moment. Because directions are camera-relative, a route that was "keep going north" can become "go south" while you are still walking the same way down the same wall — nothing has gone wrong, the frame the directions are given in has rotated. If it happens, step away from the wall and press the directions key again: with the camera back behind you the reading settles. This is the game's own camera behaviour and the mod cannot stop it without breaking battle targeting, which locks onto enemies through that same camera.

Enemy HP is given as a percentage rather than a number, the same way the game gives you a bar rather than digits. Party members and allies give real numbers. With Libra up the enemy gives real numbers too, and O reads the rest of what Libra reveals.

Not yet read: the bodies of the full-page Handbook tutorials. Both are pre-rendered images rather than text. 

In the Clan Primer, scrolling a list past the end wraps it round to the other end, and the row it lands on is not announced — it stays silent rather than reading you the row you just left. Move the cursor once more to hear where you are. This is known and half-fixed: the wrong-row reading is gone, the silence is what is left of it.

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

The mod includes ffgriever's FF12 tkMalloc Fix, which stops the game crashing or showing a white screen at startup, when loading a save or between areas on newer graphics drivers. Do not install that fix separately: it also uses the dinput8.dll filename.

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
* R: party menu.
* Left Ctrl: escape. This is a toggle,  pressing it plays a sound and locks the menus when in battle. so the battle menu and party menu will not open. Pressing it again plays another sound and unlocks them. If your menus suddenly stop opening, press Left Ctrl once.
* Esc: pause game. Note that alt tabbing out of the game window pauses the game automatically. To resume press backspace.
* M, Numpad Plus, or Z: display map.
* H, Numpad 0, or X: reset camera.
* Left Shift: toggle walk and run.
* 1: cycles game speed. In a battle target list it switches target group instead — Foes, Party, Reserve, Allies — and the mod speaks the group you land on. On a controller this used to be L1; the mod has taken that button, so on a pad this is the keyboard key for it.
* 3: switches target group the other way in a battle target list.
* F1, F2, F3: game speed — normal, double, quadruple.

### Mod

The mod reserves none of the game's keys. Every mod key is pressed on its own — no Shift, Ctrl or Alt. A controller is different — see Controller below.

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
* F6: give the selected object your own name. A box opens for you to type it in. If the object already has a name you gave it, F6 asks instead whether you want to clear it.
* F7: switch Auto detail on or off — see the mod menu below.
* F11: turn the audio beacon on or off. It only responds to F11 on its own — hold Shift, Ctrl or Alt and it does nothing, so Shift+F11 stays yours for your screen reader.
* F8: open or close the mod menu.

#### The audio beacon

Turn-by-turn directions tell you the route once. The beacon keeps telling you, while you walk.

Press \\ as normal. As well as speaking the directions, the mod starts a repeating sound placed in the direction you need to walk — to the right if the route says northeast, hard left if it says west, and so on. It is the same direction as the spoken word, just given as a sound instead of a syllable. As you get closer the sound repeats faster, from about once a second up to five times a second when you are nearly there.

Each turn of the route is a point along the way. Reaching one moves the beacon on to the next without saying anything, and the repeats slow down again — so speeding up means you are getting close to the next turn, and suddenly slowing down means you just passed it. Reaching your actual destination plays the sound once at a higher pitch and then stops.

A sound behind you is quieter, duller, and about a fifth lower in pitch than one in front. Behind means the whole half of the world behind your shoulders, not just directly astern, and the three cues arrive together — so anything behind you sounds behind you, rather than sounding gradually more behind the further round it goes.

Left and right keep working all the way round. Something behind and to your left is played to your left and carries the behind cues on top, so you hear its bearing and which half it is in at the same time.

If you wander well off the route the mod quietly works out a new one and re-aims the beacon. It does not say anything when it does this — Press \\ any time to hear the route again and re-aim.

#### The target beacon

In a fight, the route beacon stands down and a second sound takes over: it follows whatever your party leader is attacking, moving as the enemy moves, so you can hear where it is without asking. If nothing is being attacked it stays silent rather than leading you somewhere in the middle of a fight. When the fight is over the route beacon comes back on the same leg it left off. While escape is on, the route beacon plays instead, so you can run for the exit.

This is its own setting. It plays in battle whether or not you had a route running, so you can use it on its own, and switching the route beacon off does not switch it off too. Both beacons have their own volume, from 20% up to 100%. Neither volume goes all the way to silent — each beacon has its own on/off switch, so a beacon that is switched on is never silent for a reason you cannot hear.

F11 turns the route beacon off and on (F11 on its own — not with Shift, Ctrl or Alt held, so your screen-reader chords are untouched), and both beacons and both volumes are in the F8 menu. Your choices are remembered between sessions. Changing area stops the route beacon — the route belonged to the old map — but not the target beacon, which follows whatever you are fighting wherever you are.

#### Naming things yourself (F6)

Where several people or objects share one name the mod numbers them — Rabanastran 1, Rabanastran 2 — and those numbers now stay put, so the same person keeps the same number for as long as you are on that map, and again when you come back. F6 lets you replace that with words of your own: the innkeeper, quest guy, the stairs home.

Select the object with \[ and ], then press F6. A small box opens with a text field in it. Type the name you want and press Enter, or Tab to OK and press it. The mod reads back "Labelled" and your name, and from then on that is what it calls the object, in the list, in descriptions and in directions. Escape, or Cancel, closes the box and changes nothing.

While that box is open the game does not receive anything you type, and neither does the mod — your keys go to the text field and nowhere else. When you close it the game gets the keyboard straight back.

**To clear a name you have given, press F6 on that object again.** Because it already has your name on it, you get a Yes/No question instead of a text field: clear it and go back to what the game calls the object? Yes clears it, No leaves it alone, and No is what happens if you just press Enter. **To rename something, clear it first and then press F6 again** — one question per press, so neither a new name nor a cleared one can happen by accident.

**Most people and objects in FFXII have no name until you talk to them, and that is the game working correctly, not the mod failing.** The game gives an NPC their name at the point in the story where you meet them, so someone who reads as "Rabanastran" now may well introduce themselves the first time you speak to them, and from then on the mod uses the real name. Naming things yourself is meant for the ones that stay nameless — an unmarked door you keep needing, a shopkeeper the game never introduces, the staircase back to the inn. Try talking to someone once before you decide they need a name from you.

Names are saved and survive reloads, area changes and closing the game. They are kept in your own user folder (AppData\\Local\\FFXII-Screen-Reader), not in the game folder, so nothing in the game install is touched.

Map exits cannot be named this way. They come from the map's own script rather than from an object in the world, so there is nothing to attach a name to; F6 stays silent on one.

#### Status

* 4: first party member — name, HP, MP, and any statuses such as Poison or Slow.
* 5: second party member.
* 6: third party member.
* 7: guest, when you have one.
* 8: the summoned Esper, when one is out — its name, statuses, HP and summon gauge.

These work on the field as well as in battle.

Keys 4 to 9 do something else on a few screens — in a shop or an equip screen they read the equipment comparison, and on the save and load lists they read that save's party. Both are covered below. The switch is automatic and there is no mode to get stuck in: leave the screen and the keys go back to party status.

* g: how much gil the party is carrying.
* ;: the active target. In battle, its name and HP. Outside battle, what Confirm will act on where you are standing — "Talk: Montblanc", "Action: Save Crystal". Silent when nothing is in reach.

#### Status screen

The Status screen reads as you move through it — no mod key. On the Attributes page the arrow keys step through the character's attributes and status effects, up and down a line at a time and left and right by section, with Home and End jumping to either end. The Magicks and Technicks pages read from the game's own cursor as you move it.

#### Gambits

The Gambit screen reads as you move through it — no mod key. The top row is the character and whether their gambits are switched on at all. Each slot below reads as its condition, its action, and whether that slot is on: "Ally: HP < 70%, Cure, on". An unused slot says "Empty slot", so you can always tell the cursor moved.

Up and Down move between slots and read the whole slot. Left and Right move across the three parts of one slot — the on/off switch, the condition, the action — and read only the part you land on, since you heard the rest a moment ago. Choosing a new condition or action reads as you browse it.

#### Clan Primer

Opening a Bestiary, Hunts or Traveller's Tips entry reads its page, and turning the page reads the new one. Up and Down step through the open page a line at a time, Home and End jump to its start or end, and O re-reads it.

#### Buying and equipping (4 to 9)

Shops and the "equip to whom" screen draw a column of numbers per character showing what an item would do to their stats. The mod reads those columns on the number keys: 4 is the first character, 5 the second, and so on up to 9 for the sixth. It speaks the character, the stat, and which way it moves — "Vaan: Attack Power up 12, Evade down 3". A character the item cannot go on, or a stat that does not change, says nothing.

It is on a keypress rather than automatic because six characters times two stats is far too much to hear every time the cursor moves. A key you did not press costs you nothing; press only the characters you are choosing between.

The party menu's own Equipment screen compares one character, usually one or two stats. 4 reads it there too, and with Auto detail on it also reads as you move.

#### Save and load (4 to 9)

Each row of the save and load lists reads as you move to it: the slot number, where the save was made, the playtime, the party leader and their level, gil, and the clan rank and points. That is the row plus the panel underneath it, which is what a sighted player takes in at a glance. The row the game marks with an icon instead of a number is read without one, and a save from before you have a clan says nothing about rank or points, because the screen shows neither.

4 to 9 read party members 1 to 6 of the highlighted save — the same six portraits the panel shows, each as name and level. An empty slot says nothing.

#### Combat log

The mod keeps a running log of the last 100 battle events. Important ones are spoken as they happen — a command that failed, someone knocked out or revived, a level up, loot, gil, an attack being nullified, a command category disabled, a back attack. Routine chatter such as every small heal is logged but not spoken, and so is the blow-by-blow damage — that is what browsing the log is for.

* ,: Browse backward through the log.
* .: Browse forward through the log.
* Home: jump to the oldest event logged.
* End: jump to the newest event.

Home and End belong to the page while the Status screen's Attributes page or a Clan Primer entry is open — there they jump to the top or bottom of that page instead. Everywhere else, including in menus and while the game is paused, they reach the log.

How much gets spoken aloud is up to you — see Combat verbosity below.

#### Bhujerba: shouting in the streets

* B: the infamy meter, as a percentage.
* N: how many people are around you to shout at, and where the nearest guard is.

The meter also speaks on its own whenever it moves, whether it rises or an Imperial hears you and it falls. The keys and the spoken meter work only while the shouting is actually running, and are silent everywhere else in the game.

#### Stilshrine of Miriam: the three statues

* B: each of the three guardians in turn — either solved, or which way to turn it and how many times.

It reads all three from anywhere in the Stilshrine, and is silent everywhere else.

Two settings appear in the mod menu while you are shouting, and only then. Puzzle guide covers the spoken meter and the two keys, and starts on. Instant success fills the meter on your first shout, and starts off.

#### Sochen Cave Palace: the two door puzzles

* B: the step you are on in this palace's current puzzle, and which exit or door comes next.

The palace has two puzzles. The waterfall puzzle runs between Falls of Time, Mirror of the Soul and Destiny's March in four legs, and opens both Pilgrim's Doors. The door puzzle is eight of Destiny's March's own doors opened in one fixed order, and opens the Ascetic's Door.

B says, for example, "Waterfall puzzle, step 2 of 4. Mirror of the Soul 3.", and puts your selection on that exit — so the route key then leads you to it. In Destiny's March it guides the door puzzle, unless you are part-way through a waterfall leg, in which case it keeps you on that. It is silent everywhere outside the palace.

Press B again any time to pick the puzzle back up — after a chest, a fight, or a wander. It reads the puzzle afresh every press, so it tells you where the sequence actually stands and points you at the next target again; from a room the puzzle does not use, it points you back toward one that it does.

Worth knowing before you detour in Destiny's March: opening any of the eight doors is a move in the puzzle, even if you were only passing through to reach something. Do your exploring there before you start, or expect to start over. If it does happen, B says the count has stopped, and leaving the area and coming back restarts it.

#### Controller

The mod reads a plugged-in controller as well as the keyboard. It reads **any** pad Windows recognises — PlayStation, Xbox, Switch Pro and generic USB controllers alike, with no extra software in between. (In V1.0 it read Xbox pads only, so a PlayStation controller did nothing unless something like Steam Input or DS4Windows was translating it. If that was you, it works now.) Clicking **both sticks in at once** — L3 and R3 together — switches between the two control schemes, the right stick for the pathfinder or the right stick for the camera, and says which is now on.

L1 and R1 are the upper shoulder buttons; L3 and R3 are the left and right sticks clicked in. A, B, X and Y are named the way the game names them, so on a PlayStation pad A is Cross, B is Circle, X is Square and Y is Triangle — the button in that position, whatever your controller prints on it. Anything not listed here reaches the game unchanged, and A, B, X and Y are never taken in normal play.

The mod takes four things the game also uses, and each was a deliberate trade: **L1** (game speed — still on the keyboard's 1, 2 and 3, and in the options menu), **Back** (the map toggle), **L3** (the area map) and **R3** (recentre the camera). Everything else it holds, the game leaves free.

* **Right stick:** Up describes the selected object, or reads Libra on an enemy in battle; on the field with nothing to describe it reads the previous category instead. Down is the next category, Left the previous object, Right the next object. While the mod reads the pad, the right stick no longer turns the field camera, unless Right stick camera is on (below).
* **D-pad, on the field only:** party members one, two, three and your guest, clockwise from Up. Everywhere else, battle included, it stays the game's cursor.
* **Right stick camera**, in the mod menu: with it On the right stick turns the camera again and the mod leaves it alone. On the field the D-pad takes its place — Up and Down change the category, Left and Right change the object — and in a fight with no menu open the D-pad reads party members one, two, three and your guest, clockwise from Up. With a menu or target cursor up it is the game's cursor as before. R3 still switches the audio beacon.
* **L1:** what you are about to interact with — the person or object the game itself would act on if you pressed Confirm. In a fight it reads the enemy your party is on, with its HP.
* **R1:** turn-by-turn directions to the selected object, and the audio beacon. **This works in battle too**, so you can pick an exit and be led out of a fight you do not want.
* **Back:** mod mode. It says "Mod", the next button is a mod key, and it expires after five seconds.
* **L3:** the reachability filter on and off. **R3:** the audio beacon on and off. Both say which setting they changed and what it is now.
* **L3 and R3 together:** switch control scheme — the same as the Right stick camera setting.

L1 and R1 both go to the game in menus and while a targeting cursor is up — that is where the game uses them to switch target group, and the mod reads the group out for you instead of taking the button.

In mod mode, press Back and then:

* **X:** your gil. In a fight, the enemy's name and HP instead.
* **Y:** rescan and announce the area. In a fight, directions to the enemy your party is targeting instead.
* **B:** the summoned Esper's HP. Silent when you have no Esper out.
* **Start:** open the mod menu.
* **A:** ends mod mode and says "Cancelled".
* **Back again:** opens the game's map. Back is the mod's modifier, so pressing it twice is how you
reach the map button it costs you.
* Anything else ends mod mode and says "Cancelled".

While the mod menu is open, the D-pad moves between settings and changes them, right stick Up reads a description, and B toggles the setting you are on or opens a menu. A backs out of a menu one level at a time and closes the mod menu at the top, like Backspace. Back closes the whole mod menu from anywhere in it, like Escape. Nothing reaches the game while the mod menu is open — no key and no controller input, and the stick clicks do nothing there.

Everything else the mod can do is on the keyboard, and the mod menu reaches every setting. A controller has few buttons and the ones above are the ones worth spending.

#### The mod menu (F8)

F8 opens the mod's own settings. F8 again closes it, and so does Escape, from wherever you are in it. Up and Down move between settings, Home and End jump to the first and last, Left and Right change the one you are on, and O reads a description of it. The description changes with the setting, so it always tells you what the setting is doing now rather than what it could do. A setting that opens a menu of its own is opened with Right, and Backspace comes back out of it — at the top level Backspace closes the menu.

The settings it holds:

* Combat verbosity — Normal or Verbose. Normal speaks enemy defeat and EXP, party member low HP and KO, and loot drops. Verbose speaks everything Normal does, and also tells you when an enemy is readying an ability or beginning to cast a spell, which is your window to interrupt or move. Normal is the default.
* Audio beacon — On or Off. The repeating sound that leads you along the route, described under Navigation above. On is the default. On a controller, clicking the right stick in changes it without opening the menu.
* Audio beacon volume — 20% up to 100%, in fifths. 100% is the default.
* Target beacon — On or Off. The separate sound that tracks the enemy your party is fighting, described above. On is the default.
* Target beacon volume — 20% up to 100%, in fifths. 100% is the default.
* Auto detail — Off or On. Off reads the extra detail only when you ask for it, on O and on 4 to 9. On reads it as you move instead: the description of whatever you have highlighted — a magick, a technick, an item, a piece of equipment, a config row — along with the equipment comparison as you go down a shop list and the Libra readout as you move the target cursor between enemies. It always comes after the short line, never instead of it, and every key keeps working either way. Off is the default.
* Auto-walk — Off or On. With it On, the route key does not just speak the route and start the beacon — the mod walks your character along it. It stops the instant you touch a movement key, the instant a fight starts, when you arrive, when a menu opens, and after fifteen seconds of no progress, and it never starts walking again on its own — press the route key when you want it back. Off is the default. One honest limitation for controller players: auto-walk watches the keyboard alone for that, and the left stick is passed straight to the game and never read, so pushing the stick does not cancel it — tap any movement key on the keyboard, or switch it off in this menu.
* Diacritics override — Standard or Polish translation. Which font the game is running, which is what decides how accented letters are read. On Standard the mod works it out from the game itself, and that covers the unmodified game in any language it shipped in. Set it to Polish translation if you are playing the PL fan patch and accented letters come out as the wrong letter — that setting is remembered and always wins. Standard is the default.
* Unreachable filter — Off or On. On hides anything the route key has answered No path to, until a door opens, a waterfall moves or you leave the area. Enemies are always listed. Off is the default, and lists everything. On a controller, clicking the left stick in changes it without opening the menu.
* Puzzle guide — On or Off. The spoken infamy meter and the B and N keys during Bhujerba's shouting, described above. This setting and the next appear only while the shouting is actually running, so open the menu there to reach them. On is the default.
* Instant success — Off or On. Fills the infamy meter on your first shout, and the scene continues from there. Off is the default.
* Solve door puzzles — Off or On. Appears only in Sochen Cave Palace. On marks the waterfall puzzle and the clock puzzle solved, so both Pilgrim's Doors and the Ascetic's Door open when you use them. If a waterfall or an exit has not changed, leave the area and come back. Turning it off does not unsolve them. Off is the default.
* Soundscape settings — a menu of its own. Press Right to open it, Backspace to come back.
* Right stick camera — Off or On. On gives the right stick back to the game so it turns the camera, and moves its job to the D-pad, as set out under Controller above. Off is the default. Clicking both sticks in at once changes it without opening the menu.

The soundscape is a sound for each thing near you, repeating from the direction it is in, so you can hear the shape of the room around you rather than asking for it a category at a time. Nearer things are louder, and a thing behind you sounds duller and lower than one in front. Where several of a kind are near you they are spread in pitch and each repeats at a slightly different rate, so two people standing together do not blur into one sound. It follows things that move. It speaks nothing and it is not a route — the audio beacon is still what leads you somewhere.

Its menu holds:

* Soundscape — Off or On, the master switch for all of it. Off is the default: it is continuous sound layered over the game's own, so it is something you switch on when you want it. Turning it off silences everything without forgetting which kinds you had chosen.
* Soundscape volume — 20% up to 100%, in fifths. 100% is the default. This sets the level of the whole soundscape; each kind below has its own volume within it.
* Then two rows for each kind of thing — Exit, Door, Shop, Save Crystal, Gate Crystal, Treasure, NPC, Interactables, Enemy and Items. The first is Off or On for that kind, the second is its volume. Every kind starts On at 100%, so switching the master on gives you all of it, and you turn off the ones you do not want to hear. If you only want treasure and save crystals, switch the other eight off.

F4 switches Combat verbosity, F7 switches Auto detail and F11 switches the Audio beacon, all without opening the menu, so you can change any of them in the middle of a fight. Your choices are remembered between sessions.

While the menu is open no key reaches the game, so nothing you press there moves your character or the camera. Worth knowing as well: Verbose speaks the enemy's announcement when the game makes it.

#### Reading

* O: read the focused item's description or tooltip — the description of the thing you are on, not the screen's own standing help. On an open Clan Primer page it re-reads the whole page, and in the mod menu it describes the setting you are on. With an enemy under the target cursor in battle it reads what Libra reveals — HP as numbers, level, any statuses on it, and what it is weak to, absorbs, halves or is immune to — or says Libra is not active. Marks and bosses that shrug off Libra give you the same nothing they give a sighted player.
* T: repeat the last line of dialogue — a conversation page, a prompt, or an obtained-item message. Silent when none of those is on screen.
* U: current License Points, on the License Board.


## Credits

The startup-crash fix is ffgriever's FF12 tkMalloc Fix (https://gitlab.com/ffgriever/ff12-tkmalloc), built into this mod under its licence:

BSD 2-Clause License

Copyright (c) 2026, ffgriever

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

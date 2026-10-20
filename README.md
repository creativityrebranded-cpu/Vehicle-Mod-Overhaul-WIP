# Vehicle Mod Overhaul (WIP) — SeamlessVehicle for Starfield

An SFSE plugin that turns the REV-8 (and other vehicles) into a hover vehicle with gears, terrain assist and an
autopilot. Your vehicle can be summoned and can follow you. NPCs take free vehicles on their own when a fight starts and escort or chase you; they get out again when things are calm.

> **Work in progress.** Some features are built but not yet tested in game; they are listed under
> [Status](#status).

---

## Requirements

| What | Version | Link |
|---|---|---|
| **Starfield (Steam)** | **1.16.244** exactly (the plugin turns itself off on any other build) | Steam |
| **Starfield Script Extender (SFSE)** | the build for 1.16.244 (0.2.21 or newer: `sfse_1_16_244.dll`) | [sfse.silverlock.org](https://sfse.silverlock.org/) · [Nexus mod 106](https://www.nexusmods.com/starfield/mods/106) |

SFSE works with the **Steam** version only (not Game Pass / Microsoft Store).

### Optional mods (the plugin uses them when they are installed)

| Mod | What it adds here | Link |
|---|---|---|
| **Parked Vehicles** (SarthesArai) | Parked REV-8 / Deimog / Moon Jumper props at cities and outposts. The plugin's vehicle registry records vehicles there; it does not create vehicles. | [Nexus mod 13838](https://www.nexusmods.com/starfield/mods/13838) |
| **The Gang's All Here** (Aurelianis) | Several companions and crew following you at once. In a fight, a follower takes a free vehicle (see [NPC vehicles](#npc-vehicles-automatic)). Works with any followers; this mod just lets you have more. | [Nexus mod 7469](https://www.nexusmods.com/starfield/mods/7469) |
| Deimog (Bethesda, free Creation) | Lets Parked Vehicles show Deimogs. | In-game Creations menu |

Follow each optional mod's own install instructions. For The Gang's All Here, its page says to dismiss followers,
save and exit before installing or upgrading.

---

## Install

1. Install SFSE: copy `sfse_1_16_244.dll` and `sfse_loader.exe` into the Starfield folder (next to
   `Starfield.exe`), e.g. `C:\Program Files (x86)\Steam\steamapps\common\Starfield\`.
2. Copy this repository's **`Data`** folder into the same Starfield folder (merge it), so the plugin ends up at:

   ```
   <Starfield>\Data\SFSE\Plugins\SeamlessVehicle.dll
   ```
3. Enable the two data plugins that came in the same `Data` folder (add them to your plugin list, below Parked Vehicles):
   - **`Vehicles - Real.esm`**: turns the Parked Vehicles props into real, drivable vehicles (needs Parked Vehicles = `Vehicles.esm`).
   - **`SeamlessPilots.esm`**: the quest package that makes an NPC walk to a vehicle and get in. Without it NPCs cannot be sent to vehicles.
   Both are built from the game's own data by the scripts in `tools/` (`make_vehicles_real.py`, `make_pilots_esm.py`).
4. Start the game with **`sfse_loader.exe`** (Steam running), not the normal launcher.
5. Check it loaded: `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle.log` starts with
   `SeamlessVehicle 0.60.0 loaded`.

To uninstall, delete `Data\SFSE\Plugins\SeamlessVehicle.dll`. Vehicles in your save are kept.

---

## Controls

Free keys are chosen at start-up so the plugin does not clash with your bindings. The keys it picked are written to
the log (`Rover call key: …`, `Rover flight keys: …`). The defaults on a standard keyboard layout are:

### On foot

| Key | Action |
|---|---|
| **N** | **Summon** your own vehicle. Far away (over 150 m, not loaded or another worldspace): it is moved 12 m behind you, out of view. Close: it flies to you and stops 8 m away. Press again to cancel. |
| **U** | **Follow** on / off: your vehicle trails you, parks when you stand still and lifts off when you move away. |
| **E** (the game's Activate) | **Get in**: the HUD shows it when you are within 6 m of your own vehicle. |

NPC vehicles need no key: see [NPC vehicles](#npc-vehicles-automatic).

Only your own vehicle (the one the game links to you) can be summoned or set to follow.

### In the vehicle

| Key | Action |
|---|---|
| **Space ×2** (double-tap) | Hover on / off (lifts off when switched on). |
| **W** | Accelerate (double-tap W: keep accelerating until W or S). |
| **S** | Brake. |
| **Shift + W / Shift + S** | Gear up / down (gears 1–4). Shift alone does nothing. **1–4** pick a gear directly. |
| **A / D** | Turn / roll. Mouse aims (yaw and pitch). |
| **Q / Z** | Up / down. **Shift + Space**: down. |
| **T** | **Auto** on / off: the autopilot flies you to your **map marker** (nothing without a marker), with the same terrain following and obstacle avoidance as summon / follow. Any of **W / A / S / D** takes control back. Lifts off first if you were not hovering. |
| **]** (when the game leaves it free) | While Auto is on: **Cruise** on / off (fly where you look, at your current gear's speed). Out of your vehicle: cycles the profile of your unattended vehicle (scout, return home, hold, default). |
| *Get out with Auto on* | The vehicle **holds where it is** (it neither follows nor lands). Summon or Follow ends the hold; getting back in gives you control. |
| **F7** | Edit the on-screen layout: Page Up / Page Down pick any item (each button, the game's own vehicle HUD parts, or a group), arrows move it (Ctrl: x10), Delete resets it. Saved to `Logs\SeamlessVehicle\hoverui.ini`. |

Driving on the ground, the gears cap speed at 25 / 50 / 75 / 100 %. Movement keys follow your Vehicle bindings.

### On-screen buttons (contextual)

Buttons only show when they can be used; the scanner is no longer needed.

- **On foot** (only if you own a vehicle): **Summon [N]**, **Follow [U]**, **Get in [E]** (only near your vehicle), a
  status box (*SUMMONING 120 m*, *FOLLOWING 42 m*, *HOLDING*, *MOVED TO YOU*) and, when NPC vehicles are active, the gang line
  (*Gang 3  1 pursuit, 1 escort, 1 boarding*) with up to four rows under it (*pursuit  REV8  120 m*).
- **In the vehicle**: the W/A/S/D keys with labels, Gear− / Gear+ / Hover, a GEAR box (Hover turns blue while on),
  **Auto [T]** (blue while on, the W/A/S/D keys dim), an *AUTO GEAR n* line while Auto flies, and the gang line.

F7 moves everything (on foot the arrows move the whole panel).

---

## What the autopilot does (your vehicle and NPC vehicles)

- **Terrain assist:** checks the ground ahead in three directions; climbs over, turns around or stops.
- **Navigation:** the path toward the target is checked; when it is blocked the vehicle detours in 35° steps and
  comes back once the way is clear. It also follows **your trail** (where you walked) when the direct way is
  blocked.
- **Height:** cruises just over the highest ground ahead.
- **Speed:** matches your speed and uses its own gears (5 / 18 / 50 / 150 m/s). Obstacles limit the gear: 2 when
  blocked, 3 on rough ground or while the ground ahead loads.
- **Stuck:** first backs out, then climbs, then is moved next to you. If it gets no closer for 12 s while over
  60 m away, it is moved too.
- **Tight spaces:** being pushed up by something underneath, or bumping into something, no longer ends the flight.
  Three contacts in 10 s: moved next to you.
- **Flipped:** tipped over for 2 s: set upright where it is.

### Not meeting other vehicles (lane planner)

Every autopilot vehicle (yours on Auto / summon / follow, and every NPC vehicle) predicts its own path and every other
vehicle's about 10 s ahead, in 3D. If two paths are closing on each other, the vehicle with the lower right of way
yields, trying in order: a higher **altitude lane** (+8 / +16 / +24 m), a **side offset** (keep right first), both, and
finally a **speed cap** (stopping short). Right of way: the vehicle you drive > your own autopilot vehicle > NPC
vehicles (ties: the lower form id passes); parked vehicles never move, so everyone yields to them. A lane is held at
least 3 s and released after the path has been clear for 3 s. Oncoming vehicles both keep right. Vehicles moving side
by side at a steady gap (the gang's formation) are not a conflict. Log lines: `Route plan: …`.

### Drive mode (cramped spaces)

Hover's terrain assist struggles in tight spaces. After repeated contacts, push-ups, stuck decisions or being boxed in,
an autopilot vehicle with someone aboard (NPC driver, or you while Auto flies) **turns hover off and drives with the
game's own controls**: steer, accelerate, brake, **forward boost** (when open and aligned and the speed wanted is above
30 m/s) and **vertical boost** (over what is ahead, a climb phase, or a hop after being stuck). After 8 s, once the way is
clear again, hover comes back on. Log lines: `drive mode ON / off`, `drive: speed …`.

### Unlimited boost

The game's boost charge is kept full for every vehicle that is in use (driven by you, hovering, your own, or driven by an NPC job), so
boosting never runs out. Parked vehicles keep the game's own charge. Exceptions: a hovering vehicle (the flight model replaces the boost),
the vehicle you are driving (Shift is the gear key there) and your own unattended vehicle, unless drive mode is using the game's boost.

## NPC vehicles (automatic)

A small rules engine (`context.cpp`) decides, about twice a second, what NPC vehicles do. Every NPC vehicle is one of the
profiles below, assigned through the same table as the others (hold, return home, patrol, hauler, courier, scout, cruise).

- **A fight starts** (any NPC within 80 m of you is in combat): that NPC, if it is not already in a vehicle, goes to the nearest
  free vehicle within 40 m (it **runs** when it is in combat, walks when calm) and takes the driver seat. One of your followers
  boards as **Escort**; anyone else as **Pursuit**. A follower with no free vehicle near boards a vehicle an NPC is driving, as a
  **passenger** (a free seat of that vehicle).
- **Escort:** trails you in formation, parks when you stand still, never fires.
- **Pursuit:** hostile driver, circles you at 30 m, 6 m above your height, fires the vehicle weapon with the nose on you.
- **Calm again:** the vehicle is nearly still (under 2 m/s) and nobody on board or you is in combat for 60 s: everyone gets out.
- **Leash:** an NPC vehicle keeps following until it is 600 m from where the NPC started (its origin), however far you are; going indoors also ends it.
- Up to **6** NPC vehicles at once. The seat is the driver's instant seat after the NPC has walked to the vehicle (the animated entry
  is switched off in this build).
- Switch the engine off with `Logs\SeamlessVehicle\seamless.ini`: `[context]` `enabled=0`.

Tuning and test keys: `[log]` `verbose=1` brings back the chatty log lines. `[keys]` `debug=1` enables the manual test keys
(board the NPC you face `;`, exit all `Insert`, escort / pursuit toggle `.`, cycle NPC vehicle profiles `[`); they are claimed from
the keys the game leaves free and listed in the log (`Keys: ...`).

## Getting into an occupied vehicle

When you press **Activate (E)** at a vehicle (within 5 m, in front of you) that has someone in the driver seat:

| | Tap E | Hold E (1 s) |
|---|---|---|
| **Any driver** | You get the **passenger seat** (the companion seat) and ride along | Pulls the driver out and you take the wheel |
| Your gang / follower | same | They hand you the wheel (no crime) |
| Neutral NPC | same | **Pulled out:** assault alarm, plus a theft alarm if someone else owns the vehicle |
| Hostile NPC | same | **Hijack:** they're pulled out and fight you on foot |

While you face an occupied vehicle on foot, two small prompts appear next to Get in (half size): **Passenger** (tap) and
**Hold: eject**, both showing the Activate key. They can be moved with the F7 layout editor (Page Up / Down: Passenger, Eject).

Empty vehicles use the game's own get-in and theft rules.

### NPCs forcing you out

Only while your vehicle is on the ground (hover off) and slow (under 3 m/s), and only after a 2 s warning:
**drive off to escape**.

| Who (within 4 m) | When | What happens |
|---|---|---|
| **Guard** | Hostile to you and part of a crime faction (you're wanted) | You're taken out (the game's own exit); the game's arrest / fight follows |
| **Owner** | The vehicle's owner, or a member of its owning faction; you're in their vehicle | You're taken out, and they get back in |
| **Hijacker** | Hostile NPC in combat with you | You're pulled out at once; they take the vehicle and come after you (Pursuit) |

Your own vehicle is never reclaimed by an owner, and your gang never forces anyone out.

### Riding as a passenger (taxi) and taking the wheel

Next to a vehicle a gang member is flying (within 6 m), press the **passenger key** (the first free of G, I, L, M, X, V, /;
the log line `Passenger: key …` names it):

| Action | What happens |
|---|---|
| **Tap** | You take the companion seat. The driver flies where **you look**; **W / S** set how far ahead it aims (its cruise speed). Tap again to get out. |
| **Map marker placed** | **Taxi:** the driver flies to your marker and holds 6 m from it; after 2.5 s there you are let out. Remove the marker and it flies where you look again. |
| **Hold 1 s** | **Take the wheel:** the driver steps out, you are seated as the driver, then the driver takes the companion seat. |

Nobody forces a passenger out of the vehicle. Planned next: hailing a taxi and orders.

## View distance

Vehicles fade out at 3 times the game's own distance (objects share one fade multiplier, so this also applies to other objects of that class:
statics, furniture, doors). It only changes how far loaded objects stay visible; a vehicle in a cell that is not loaded does not show at any
setting. It costs some frame rate. In `Logs\SeamlessVehicle\seamless.ini`:

```
[view]
objectFade=3   ; 1 = the game's own distance, 0 = never fade, 3 = default
actorFade=1    ; the same for actors
```

The file is re-read every 5 seconds. The log says `View distance: applied` when it takes effect.

## Vehicle registry

Nothing is spawned. Every real vehicle in the loaded area is recorded with its place, owner and who uses it
(parked / player / npc escort / npc pursuit / own), and changes are written to
`Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\vehicles.tsv`. Extra vehicles that an older build created
at Parked Vehicles spots are hidden once (one is kept).

---

## Status

Tested in game earlier: hover flight, gears, HUD buttons, summon / follow, NPC vehicles following with trail navigation and the gear
system.

Built, **not yet tested in game**: everything in [NPC vehicles](#npc-vehicles-automatic) (the rules engine, the passenger seats, the
profile table with escort and pursuit in it, the per-vehicle HUD rows), riding as a passenger / taxi / take the wheel, the seat rules and
forced exits, NPC vehicle weapon fire (Pursuit), aiming shots at the player, the tight-space handling, flip recovery, the vehicle registry
and duplicate cleanup, the lane planner, drive mode, unlimited boost, the contextual HUD, **Auto** to the map marker, Cruise, and the
shared navigation (nav map, heading sampler, hull-sized sweep, which fix the forward rays that never returned a hit in earlier builds).

Also not tested in game: the view distance setting, the 600 m leash, and the redrawn gang HUD (one boxed line and a movable vehicles box).

Removed from this build: the follower-command Walk / Enter keys, the `,` send key, and followers taking vehicles when you get in
(gang boarding).

Added 2026-10-20, built, **not yet tested in game**:
- Vehicles are no longer terrain to each other (no "magnet" between hovering vehicles); a silent proximity warning slows the autopilot,
  with right of way by rank (parked > you > followers > neutral > hostile).
- Scanner marker: aims along the camera, estimates out to 1 km, marker chains (tap T to queue, hold T to replace) swapped about 4 s before
  arrival, and an **Auto: Stop / Orbit / Patrol** HUD button.
- Your vehicle following you catches you from below when you are in the air, and keeps following when you go indoors.
- Followers drive your vehicle when you press Summon / Follow / set a marker (crew driver); hold Follow to recall escorts.
- Escorts defend you (enemy vehicles first): intercept, shield, overwatch, attack, convoy column, roles, threat level, rescue when your
  health is low, scout ahead to your marker, wait at the door, pick up wounded followers.
- Enemy vehicles: flank, intercept, block, overwatch, retreat near their hit limit (driver fights on foot), pick up their wounded, call
  reinforcements; enemies run to free vehicles up to 150 m away in vehicle fights.
- Hit counter: shots through a vehicle add up; at your health worth its autopilot switches off for 30 s (`[hits]` in seamless.ini).
- Walls are no longer a terrain-assist "magnet": a wall never raises the hover's hold, its floor or a climb-over. It sets a wall warning
  instead: autopilots slow to stop about 6 m short and go around (sliding off side walls); flying by hand you only get an
  "Assist: wall ahead" line. The forward rays and the hull sweep look farther ahead (25 m or 2.4 s of travel) for an earlier warning.
- Night and bad weather (planet-local night, rain / snow / dust): autopilots fly slower and a little higher. Shift changes start a
  transit at once, bystanders flee or cower after a vehicle crash, and hostile outposts send patrol convoys.
- Civil traffic (outside fights, at most 2 at once, never essential / protected NPCs): transit with drop-off, shared rides, offer a ride,
  outpost patrols, supply runs, return home, parking in clear spots, fleeing hostile vehicles, slowing for people on foot.

Logs to send when something goes wrong: `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle.log` (and
`SeamlessVehicle.prev.log` for the previous session).

---

## Settings

All optional, in `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\` (re-read every 5 seconds):

| File | Section | Key | What it does |
|---|---|---|---|
| `seamless.ini` | `[log]` | `verbose=1` | The chatty log lines (per-second NPC state, pilot positions, HUD notices) |
| `seamless.ini` | `[context]` | `enabled=0` | Turns the NPC rules engine off |
| `seamless.ini` | `[keys]` | `debug=1` | Manual test keys: board the NPC you face `;`, exit all `Insert`, escort / pursuit toggle `.`, NPC profile cycle `[` |
| `seamless.ini` | `[view]` | `objectFade=3`, `actorFade=1` | Fade distance multipliers (1 = the game's own, 0 = never fade) |
| `nav.ini` | `[nav]` | `sampler=0`, `shape=0`, `rays=0` | Turn off the heading sampler, the hull sweeps, or all engine ray and sweep queries |
| `hoverui.ini` | | | HUD layout, saved by the F7 editor |

## Troubleshooting

- **Nothing happens / no log:** the plugin only runs on Starfield **1.16.244** with SFSE for that build. The first log line says why when it disables itself.
- **NPCs never go to a vehicle:** `SeamlessPilots.esm` is not enabled (the log says so).
- **Parked props, not real vehicles:** `Vehicles - Real.esm` is not enabled, or Parked Vehicles is missing.
- **A crash:** send `SeamlessVehicle.log` (and `.prev.log`). The report lists the faulting address, the stack and what the plugin did in the last moments.
- **Frame rate drops with Auto or NPC vehicles:** set `[nav] rays=0` to test; lower `[view] objectFade`.

More: `docs/ARCHITECTURE.md` (how it fits together), `docs/ENGINE_NOTES.md` (engine addresses and findings), `docs/TESTING.md` (what is tested, the in-game checklist),
`docs/KNOWN_ISSUES.md` (open bugs and next steps), `docs/research/` (research notes).

## Building from source

Needs Visual Studio 2022 Build Tools (C++ workload) and CMake 3.24+.

```
git clone https://github.com/ianpatt/sfse ref/sfse
cmake -S source -B source/build -G "Visual Studio 17 2022" -A x64
cmake --build source/build --config Release --target SeamlessVehicle
```

Output: `source/build/Release/SeamlessVehicle.dll`. The SFSE headers must be the version for Starfield 1.16.244
(SFSE 0.2.21). `source/CMakeLists.txt` also builds `SeamlessPlanets.dll`, a separate traversal plugin that is not
needed for this mod.

### Repository layout

```
Data/SFSE/Plugins/SeamlessVehicle.dll   the built plugin (copy into the Starfield folder)
Data/SeamlessPilots.esm                 the walk-to-vehicle quest package (enable it)
Data/Vehicles - Real.esm                Parked Vehicles props -> real vehicles (enable it)
source/                                 C++ source and CMake build (src/, tests/)
tools/                                  integrate.sh (build + install), make_vehicles_real.py, make_pilots_esm.py
docs/                                   ARCHITECTURE, ENGINE_NOTES, TESTING, KNOWN_ISSUES, research/
```

`bash tools/integrate.sh "note"` builds and installs (it refuses to install while the game runs). It needs the SFSE headers for 1.16.244 in `ref/sfse`
next to this folder (`git clone https://github.com/ianpatt/sfse ref/sfse`) or `SFSE_ROOT` set.

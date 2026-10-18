# Vehicle Mod Overhaul (WIP) — SeamlessVehicle for Starfield

An SFSE plugin that turns the REV-8 (and other vehicles) into a hover vehicle with gears, terrain assist and an
autopilot. Your vehicle can be summoned and can follow you. NPCs can board vehicles and escort or chase you.

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
| **The Gang's All Here** (Aurelianis) | Several companions and crew following you at once. When you get in a vehicle, each follower gets one too (see [Gang vehicles](#gang-vehicles)). Works with any followers; this mod just lets you have more. | [Nexus mod 7469](https://www.nexusmods.com/starfield/mods/7469) |
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
3. Start the game with **`sfse_loader.exe`** (Steam running), not the normal launcher.
4. Check it loaded: `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle.log` starts with
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
| **,** (comma) | The NPC you are facing (within 60 m, 20° of your heading) gets into the nearest free vehicle and follows you. |
| **.** (period) | Switch every NPC vehicle that is following you between **Escort** (trails you, parks, never fires) and **Pursuit** (hostile, circles you at 30 m, fires the vehicle weapon). |

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
| **T** | **Auto** on / off: the autopilot flies you where you look (mouse), at your current gear's speed, with the same terrain following and obstacle avoidance as summon / follow. Any of **W / A / S / D** takes control back. Lifts off first if you were not hovering. |
| *Get out with Auto on* | The vehicle **holds where it is** (it neither follows nor lands). Summon or Follow ends the hold; getting back in gives you control. |
| **F7** | Edit the on-screen layout: Page Up / Page Down pick any item (each button, the game's own vehicle HUD parts, or a group), arrows move it (Ctrl: x10), Delete resets it. Saved to `Logs\SeamlessVehicle\hoverui.ini`. |

Driving on the ground, the gears cap speed at 25 / 50 / 75 / 100 %. Movement keys follow your Vehicle bindings.

### On-screen buttons (contextual)

Buttons only show when they can be used; the scanner is no longer needed.

- **On foot** (only if you own a vehicle): **Summon [N]**, **Follow [U]**, **Get in [E]** (only near your vehicle), a
  status box (*SUMMONING 120 m*, *FOLLOWING 42 m*, *HOLDING*, *MOVED TO YOU*) and the gang line (*Gang 3 escort [.]*)
  when NPC vehicles follow you.
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

The game's boost charge is kept full for every vehicle, so boosting never runs out. Exceptions: a hovering vehicle (the
flight model replaces the boost), the vehicle you are driving (Shift is the gear key there) and your own unattended
vehicle, unless drive mode is using the game's boost.

## Gang vehicles

Your followers (the game's follower list, which The Gang's All Here fills) take vehicles when you do:

- **You get in a vehicle:** after about 1 s, every follower within 80 m who isn't already riding gets one:
  1. your **spare vehicles** first (owned by you, or your own vehicle while you're in another one), within 400 m;
  2. then the nearest **free vehicle** within 120 m of them, unowned before owned.

  Nothing is spawned. Followers the game seats as your passengers stay passengers.
- **While you drive:** they escort you in formation (left, right, then rows further back), match your speed and use
  the same obstacle and trail navigation as your own vehicle.
- **You get out:** they land and get out; their vehicles stay where they landed.
- Up to **6** NPC vehicles at once; extra followers stay on foot.
- **.** (period) switches them between Escort and Pursuit. Pursuit currently targets you; targeting enemies is next.

## Sending an NPC to a vehicle, step by step (Walk / Enter)

Built from the game's own pieces (from its console command `CommandedActivate`, the follower "use this" command):

| Key | What it does |
|---|---|
| **Walk** (the first free of `[` `;` `'`; named in the log: `WALK key …`) | The NPC you face is **commanded** with the game's own follower command to use the nearest free vehicle: it walks there (the command-mode packages), no instant seat. The log prints a state line every second: distance, process level, package type, flags, seat interaction. |
| **Enter** (the first free of `]` `\` `-`) | That NPC does the **get-in now**: the entry animation the player uses, wherever it stands. |
| *(trigger)* | The moment the game reports the NPC seated, control is handed to the autopilot (escort / pursuit) as before. |

The older `,` key (send, with a fallback that seats the NPC after 5 s) still works.

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

## Vehicle registry

Nothing is spawned. Every real vehicle in the loaded area is recorded with its place, owner and who uses it
(parked / player / npc escort / npc pursuit / own), and changes are written to
`Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\vehicles.tsv`. Extra vehicles that an older build created
at Parked Vehicles spots are hidden once (one is kept).

---

## Status

Tested in game: hover flight, gears, HUD buttons, summon / follow, NPC boarding with the comma key, NPC vehicles
following with trail navigation and the gear system.

Built, **not yet tested in game**: riding as a passenger / taxi / take the wheel, the seat rules and forced exits, gang vehicles (boarding with you, formation, landing with you, 6 NPC
vehicles), NPC vehicle weapon fire (Pursuit), aiming shots at the player, the tight-space handling, flip recovery,
the vehicle registry and duplicate cleanup. Also new (2026-10-17 evening): NPC vehicles fly at your height (attackers
6 m above you) and fire only with the nose on you; no driver, no flight (you or an NPC driver dying stops that
vehicle); ground contact no longer cuts the hover at low height; every HUD item moves on its own in the F7 editor.

Also built, **not yet tested in game**: the lane planner (vehicles not meeting each other), drive mode (cramped spaces),
unlimited boost, and the contextual HUD (on-foot panel, no scanner), **Auto** for your own vehicle
(T), holding position when you get out with Auto on, and the summon teleport rule (over 150 m, behind you).

Planned next: Auto to the map marker, and gang combat against enemies (vehicle vs vehicle, passengers, getting out to
fight).

Logs to send when something goes wrong: `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle.log` (and
`SeamlessVehicle.prev.log` for the previous session).

---

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
source/                                 full C++ source and CMake build
  src/vehicleflight.cpp                 hover flight model, autopilot, navigation, gears
  src/npcvehicles.cpp                   NPC boarding, escort / pursuit profiles, vehicle weapon
  src/realvehicles.cpp                  vehicle registry
  src/hoverui.cpp                       on-screen buttons (Scaleform)
```

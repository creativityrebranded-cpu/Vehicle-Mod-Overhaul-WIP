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
| **N** | **Summon** your own vehicle. Far away (over 300 m, not loaded or another worldspace): it is moved next to you. Close: it flies to you and stops 8 m away. Press again to cancel. |
| **U** | **Follow** on / off: your vehicle trails you, parks when you stand still and lifts off when you move away. |
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
| **F7** | Edit the on-screen button layout (saved to `Logs\SeamlessVehicle\hoverui.ini`). |

Driving on the ground, the gears cap speed at 25 / 50 / 75 / 100 %. Movement keys follow your Vehicle bindings.

### On-screen buttons

Above the vehicle health bar: the W/A/S/D keys with labels, Gear− / Gear+ / Hover, and a GEAR box (Hover turns
blue while on). The scanner shows Summon [N], Follow [U] and a SUMMONING / FOLLOWING status.

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

## Getting into an occupied vehicle

When you press **Activate (E)** at a vehicle (within 5 m, in front of you) that has someone in the driver seat:

| Driver | Tap E | Hold E (1 s) |
|---|---|---|
| Your gang / follower | **Hand-over:** they get out at once and you get in | same |
| Neutral NPC | **Blocked** ("Occupied - hold E to pull them out") | **Pulled out:** assault alarm, plus a theft alarm if someone else owns the vehicle |
| Hostile NPC | **Blocked** | **Hijack:** they're pulled out and fight you on foot |

Empty vehicles use the game's own get-in and theft rules. Planned next: NPCs forcing you out (owners, hijackers,
guards), riding as a passenger, "drive me", take the wheel, and orders.

## Vehicle registry

Nothing is spawned. Every real vehicle in the loaded area is recorded with its place, owner and who uses it
(parked / player / npc escort / npc pursuit / own), and changes are written to
`Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\vehicles.tsv`. Extra vehicles that an older build created
at Parked Vehicles spots are hidden once (one is kept).

---

## Status

Tested in game: hover flight, gears, HUD buttons, summon / follow, NPC boarding with the comma key, NPC vehicles
following with trail navigation and the gear system.

Built, **not yet tested in game**: gang vehicles (boarding with you, formation, landing with you, 6 NPC
vehicles), NPC vehicle weapon fire (Pursuit), aiming shots at the player, the tight-space handling, flip recovery,
the vehicle registry and duplicate cleanup.

Planned next: autopilot for your own vehicle (**B**), a contextual on-foot panel (Summon / Follow / Get in, no scanner),
summon teleport rules, and gang combat against enemies (vehicle vs vehicle, passengers, getting out to fight).

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

# Vehicle Mod Overhaul (WIP)

### Hover vehicles, an autopilot that thinks, and NPCs who actually use vehicles - for Starfield

Your REV-8 becomes a hover vehicle with gears, terrain following and a real autopilot. Call it, let it follow you, send it to a
map marker, or let a follower drive it. Followers jump into vehicles to defend you, enemies take vehicles to hunt you down, and
outside a fight the people around you drive, patrol and run errands on their own.

> **Work in progress.** Much of this mod is built but has **not been tested in game yet**. Every feature below is marked
> **Tested** or **Untested**, and the [Status](#status) section lists exactly what has and has not been tried. Expect bugs, and please
> send your log when something goes wrong (see [Reporting a problem](#reporting-a-problem)).

---

## Feature overview

| Feature | What you get | State |
|---|---|---|
| **Hover flight** | Double-tap Space to hover; gears, boost, climb and descend, terrain assist | Tested |
| **Summon and Follow** | Call your vehicle to you, or let it trail you and park when you stop | Tested |
| **Auto to the map marker** | The autopilot flies you to your marker; any movement key takes over | Untested |
| **Auto behaviours** | At the marker: **Stop**, **Orbit** it, or **Patrol** a chain of markers; an on-screen button shows which | Untested |
| **Scanner markers** | Mark what you look at up to ~1 km away; **chain** several markers into a route | Untested |
| **Catch from below** | Jump or fall from a height and your following vehicle flies under you to catch you | Untested |
| **Follower drivers** | With a follower nearby, Summon / Follow / a marker sends the follower to drive your vehicle for you | Untested |
| **Escorts defend you** | Followers in vehicles intercept, shield, give overwatch and attack - enemy vehicles first | Untested |
| **Rescue, scout, wait** | A follower vehicle picks you up when you are hurt, scouts your marker, waits at the door | Untested |
| **Smarter enemies** | Enemy vehicles flank, intercept, block, hover overhead, retreat when damaged and call for help | Untested |
| **Vehicle hit counter** | Vehicles take "damage" from shots; at the limit their autopilot shuts down for a while | Untested |
| **Everyday traffic** | Outside fights, NPCs drive to far destinations, share rides, patrol outposts and run supply trips | Untested |
| **Vehicles respect each other** | No more vehicles "magnetised" together; right of way decides who gives way | Untested |
| **No more wall magnet** | Walls no longer pull the hover up their face; they become an early warning instead | Untested |
| **Shared 3D navigation** | Altitude cost fields guide local trajectory plans; bounded recovery holds failed trips with Retry / Skip / Clear controls | Untested in game; offline checks pass |
| **Environment-aware traffic and combat** | Traffic detours check the world map; combat positions consider cover and automated fire requires a clear-shot probe | Untested in game; offline checks pass |
| **Checked boarding and drop-off** | Vehicle footprint, slope, water, headroom, standing space, and stopped arrival are checked before boarding or exit | Untested in game; offline checks pass |
| **Flies under things** | Overhangs and bridges are flown under; ceilings cap the height | Untested |
| **Knows the area up front** | The game's own collision objects are read into the vehicles' map, so planning starts with the walls already known | Untested |
| **Night and weather** | Autopilots slow down and fly a little higher at night and in rain, snow or dust | Untested |
| **Bystanders react** | People near a crash flee or cower | Untested |
| **Passenger, taxi, take the wheel** | Ride with an NPC driver, let them taxi you, or take over | Untested |
| **Unlimited boost, view distance** | Boost never runs out on vehicles in use; vehicles stay visible farther away | Untested |

---

## Requirements

The [shared-systems audit](docs/SHARED_SYSTEMS_AUDIT.md) maps each consumer to navigation and environmental checks. Save/load persistence is paused and is not included in this build.

| What | Version | Link |
|---|---|---|
| **Starfield (Steam)** | **1.16.244** exactly (the plugin switches itself off on any other build) | Steam |
| **Starfield Script Extender (SFSE)** | the build for 1.16.244 (0.2.21 or newer: `sfse_1_16_244.dll`) | [sfse.silverlock.org](https://sfse.silverlock.org/) · [Nexus mod 106](https://www.nexusmods.com/starfield/mods/106) |

SFSE works with the **Steam** version only (not Game Pass / Microsoft Store).

### Recommended (optional)

| Mod | Why | Link |
|---|---|---|
| **Parked Vehicles** (SarthesArai) | Puts parked vehicles at cities and outposts. Without parked vehicles, NPCs have nothing to drive. | [Nexus mod 13838](https://www.nexusmods.com/starfield/mods/13838) |
| **The Gang's All Here** (Aurelianis) | More followers at once: more escorts and more crew. | [Nexus mod 7469](https://www.nexusmods.com/starfield/mods/7469) |
| Deimog (Bethesda, free Creation) | Lets Parked Vehicles show Deimogs. | In-game Creations menu |

Follow each optional mod's own install instructions. For The Gang's All Here, its page says to dismiss followers, save and exit
before installing or upgrading.

---

## Installation

1. Install SFSE: copy `sfse_1_16_244.dll` and `sfse_loader.exe` into the Starfield folder (next to `Starfield.exe`).
2. Copy this mod's **`Data`** folder into the Starfield folder (merge it). The plugin ends up at
   `<Starfield>\Data\SFSE\Plugins\SeamlessVehicle.dll`.
3. Enable the two data plugins that came with it (below Parked Vehicles in your load order):
   - **`SeamlessPilots.esm`** - lets NPCs walk to a vehicle and get in. **Without it, NPCs never use vehicles.**
   - **`Vehicles - Real.esm`** - turns the Parked Vehicles props into real, drivable vehicles (needs Parked Vehicles).
4. Start the game with **`sfse_loader.exe`** (Steam running).
5. Check it loaded: `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle.log` starts with `SeamlessVehicle ... loaded`.

**Uninstall:** delete `Data\SFSE\Plugins\SeamlessVehicle.dll` (and the two `.esm` files if you like). Vehicles in your save are kept.

---

## Controls

The plugin only takes keys the game leaves free, so it never clashes with your bindings. The keys it picked are written to the
log at start-up (lines such as `Rover call key: …`, `Scanner marker: Auto behaviour key …`, `Passenger: key …`). Defaults on a
standard layout:

### On foot

| Key | Action |
|---|---|
| **N** | **Summon** your vehicle. Far away: it is moved in behind you, out of sight. Close: it flies to you. Press again to cancel. |
| **U** | **Follow** on / off: it trails you, parks when you stand still, lifts off when you move on, catches you when you fall. |
| **Hold U (1 s)** | **Recall**: every follower vehicle drops the fight and comes back into formation for 15 s. |
| **E** (Activate) | **Get in** (shown within 6 m of your vehicle). At an occupied vehicle: tap = passenger seat, hold = take the wheel / pull the driver out. |
| **Auto behaviour key** (first free of `]` `J` `K` `N`) | Cycles what Auto does at the marker: **Stop → Orbit → Patrol**. With a follower free and a marker set, a follower drives your vehicle there. |

### Hand scanner open

| Key | Action |
|---|---|
| **T** (tap) | Puts the game's map marker where you look, up to ~1 km away (past the loaded area the distance is an estimate, shown with `~`). Your vehicle is summoned, and Auto flies you there when you get in. **While a trip is already on, a tap adds the point to a chain** (`+N queued` on screen). |
| **T** (hold 0.8 s) | Throws the chain away and sets just this point. |

Chained markers swap automatically: about 4 s before your vehicle reaches the current marker, the next one takes its place and
the vehicle flies on without stopping. With **Auto: Patrol**, the chain loops for ever.

### In the vehicle

| Key | Action |
|---|---|
| **Space ×2** | Hover on / off |
| **W / S** | Accelerate / brake (double-tap W: keep accelerating) |
| **Shift + W / Shift + S**, **1–4** | Gear up / down, or pick a gear |
| **A / D**, mouse | Turn / roll; the mouse aims |
| **Q / Z**, **Shift + Space** | Up / down |
| **T** | **Auto** on / off: the autopilot flies you to your map marker. Any of W / A / S / D takes control back. |
| **]** (when free) | While Auto is on: **Cruise** (fly where you look at your gear's speed) |
| *Get out with Auto on* | The vehicle holds where it is until you Summon, Follow or get back in. |
| **F7** | Move any on-screen button (Page Up / Down pick, arrows move, Ctrl ×10, Delete resets). Saved to `hoverui.ini`. |

### On-screen buttons

Buttons only show when they can be used:
- **On foot** (if you own a vehicle): Summon, Follow, Get in (near your vehicle), the **Auto: Stop / Orbit / Patrol** button, a
  status box (*SUMMONING 120 m*, *FOLLOWING 42 m*…), and when NPC vehicles are active, who is in them (*Sarah, Barrett +1*) and a line
  per NPC (*Sarah  escort  42 m*).
- **In the vehicle:** W / A / S / D, Gear− / Gear+ / Hover, the gear box, **Auto**, the **Auto behaviour** button (flashes blue when
  you press its key) and the NPC vehicle lines.
- **Route display on the HUD/scanner:** native status boxes for the destination and queued stops, with native **Retry / Skip / Clear** key prompts. Select **Route** in the F7 editor to move the group.

---

## Your vehicle

### The autopilot
- **Plans a few seconds ahead (MPC):** 20 times a second it tries 37 short paths (9 turn rates × 3 speeds, "turn then straighten" and
  its last plan), simulates each 2.5 s ahead and scores them against what the vehicles have mapped (walls, ground above its flight
  level, bump spots, unknown space), the other vehicles' predicted paths, progress toward the target and smoothness. It flies the best
  one and plans again. Decisions are snappier than before.
- **Knows the area up front:** every couple of seconds the mod reads the game's own list of loaded objects that have collision
  (buildings, walls, rocks, bridge decks…) within 250 m and writes each one's box (its size, position and heading) into the map the
  planner uses, as a height band, so open space under a deck stays open. The planner no longer has to discover walls by bumping its
  rays into them. The boxes are the starting picture and the rays have the last word: where a ray has recently flown through a box
  (an arch's opening, the gap in an L-shaped building), that spot counts as open; where a ray hit something, it is solid whatever
  the boxes say. Very large objects (over 120 m across) are left to the rays, and where the vehicle is inside an object's box (a
  big composite structure) the planner trusts the rays instead.
- **Stays inside what it knows:** the autopilot never flies faster than it could stop within the mapped area ahead (about 85 m/s
  with 250 m mapped). It plans with the flown vehicle's own size, and a spot a ray once hit is forgotten when a later ray flies
  straight through it (an opened door, a moved object).
- **Cheap escapes:** if every path is blocked, the older rules take over (turn away in steps, follow **your trail**, climb as a last
  resort). If it barely moves for 6 s while still far from its target, it escapes for 3 s: along your trail when there is one,
  otherwise away from the spot where it got stuck.
- **Follows the terrain** and keeps just above the highest ground ahead.
- **Uses its own gears**, matches your speed, and is moved next to you if it gets truly stuck or lost.
- **Tight spaces:** after repeated bumps it switches hover off and **drives** with the game's own controls (steer, boost, jump),
  then hovers again once the way is clear.
- **Flipped:** set upright where it is.
- **Night and bad weather:** at planet-local night, and in rain, snow or dust, autopilots fly about 25-30 % slower and a little higher.

### What counts as ground
**Ground** is the real terrain (hills and cliffs, seen from high above), or any surface **at or below the vehicle's flight level** (a
street, platform or roof it can reach without climbing into anything). Everything **above** the flight level that is not terrain
(building faces, overhangs, bridges, undersides) never lifts the hover: it is only a warning (below). Two rays look up (straight up and
up-ahead): under a ceiling the height is capped 5 m below it, so the vehicle **flies through under bridges and overhangs** instead of
pressing into them.

### Walls (no more "magnet")
Earlier builds treated a wall like a hill: the hover tried to climb its face and stuck to it. Now a wall **never lifts the vehicle**.
It raises a **wall warning** instead, which the forward rays pick up 25 m (or 2.4 s of travel) ahead:
- **Autopilot:** slows so it can stop about 6 m short, goes round the wall, and slides off side walls closer than 4 m. It only flies
  higher as a last resort, when both sides are blocked.
- **Flying by hand:** you only get an **"Assist: wall ahead"** line; the vehicle does not steer for you.

### Other vehicles
Vehicles are no longer "ground" to each other, so two hovering vehicles no longer pull together. A quiet proximity warning
makes the lower-ranked vehicle give way (it slows and slides aside). **Right of way:** parked > you and your vehicle > followers >
neutral NPCs > hostiles. A longer-range lane planner also predicts every vehicle's path about 10 s ahead and changes height, side or
speed to keep paths apart.

### Catch from below
When you are more than 4 m above the ground (jumping off a ledge, jetpacking, falling), your following vehicle flies to a point about
7 m under where your fall will take you, so it can catch you. It moves smoothly (no wobbling) and goes back to normal following once
you land.

---

## Followers

Followers are read from the game's own follower list.

### Follower drivers (crew)
If a follower is free (on foot, within 60 m) when you press **Summon**, **Follow**, set a **scanner marker** or press the **Auto
behaviour** key, your vehicle is brought to you and **the follower gets in and drives it**:

| You press | The follower… |
|---|---|
| Summon / Follow | drives your vehicle after you, defends you in a fight, picks up other followers as passengers |
| Scanner marker / Auto behaviour key | drives to your marker and does the Auto behaviour there (stop, orbit, patrol the chain) |
| Follow again | gets out |

No follower free? The buttons work as before. Your vehicle's own autopilot leaves it alone while a follower drives.

### Escorts
When a fight starts near you, followers take free vehicles close to them and become **escorts**:

| Behaviour | When | What it does |
|---|---|---|
| Follow, convoy | always | Formation behind you; one column when you move fast |
| **Intercept** | an enemy vehicle closes on you from far | Cuts it off between you and it |
| **Shield** | the enemy is out of range, or there are many enemies, or it is the second escort | Sits between you and the enemy |
| **Overwatch** | enemies on foot | Hovers high behind you and fires down |
| **Attack** | an enemy vehicle in range | Circles it and fires |
| **Rescue** | your health drops under 30 % | The nearest escort lands beside you so you can get in |
| **Scout** | you walk toward your map marker | One escort flies ahead and tells you how many enemies are there |
| **Wait at the door** | you go indoors | They park and wait aboard, then follow when you come out |
| **Pick up wounded** | a follower is under 30 % health | An escort picks them up as a passenger |

Escorts target **enemy vehicles first**, spread their targets so two don't pile onto one enemy, and never target you or other followers.
Hold **U** to recall them.

---

## Enemies

In a fight, an enemy near a free vehicle (within 40 m; within 150 m once the fight has vehicles in it) runs to it and gives chase.
Enemy drivers attack you **and your followers**, vehicles first, and choose what to do from the fight:

| Behaviour | When |
|---|---|
| **Retreat** | its vehicle is near the hit limit: it pulls back, lands, and the driver fights on foot (and calls up to 3 nearby allies into the fight) |
| **Block** | you sit in a slow vehicle: the first enemy lands in front of you |
| **Intercept** | you drive away fast: it flies at where you will be |
| **Overwatch** | you are on foot and there are two or more enemy vehicles: one hovers over you and fires down |
| **Flank** | two or more enemy vehicles: each comes from its own side |
| **Pick up wounded** | an ally is under 30 % health: it carries them off and pulls out |
| **Hijack** | your vehicle is on the ground and slow: a hostile pulls you out (after a 2 s warning; drive off to escape) |

Hostile outposts also send **patrol convoys**: a hostile owner patrols in their vehicle, a second hostile vehicle follows, and they
turn on you when the fight starts.

### The hit counter
Vehicles have no health in the game, so the mod follows every shot. A shot that passes through a vehicle counts as a hit
(15 by default). When a vehicle has taken about as much as **you** could (your own health), its **autopilot shuts down for 30 s** and it
comes down. Friendly fire is not counted. It heals after 15 s without hits.

---

## Everyday traffic (outside fights)

When nobody near you is fighting, NPCs within 200 m use vehicles on their own (at most 2 at a time, so fights always have vehicles free):

| Behaviour | What happens |
|---|---|
| **Transit** | An NPC walking somewhere over 300 m away, with a vehicle it may use within 30 m, drives there, parks and walks the rest |
| **Shift change** | When an NPC's daily schedule moves them on to a far place, they set off by vehicle straight away |
| **Shared ride** | Another NPC going the same way rides along |
| **Offer a ride** | A driver passing you on foot stops for 10 s - tap the vehicle to ride along |
| **Outpost patrol** | An owner by their parked vehicle drives two laps round the outpost and parks again |
| **Supply run** | Sometimes the owner drives to the next settlement or outpost (200 m - 1 km), waits, and comes back |
| **Return home** | After a fight, non-followers fly the vehicle back to where they took it before getting out |
| **Parking** | Every trip ends in a clear spot, not on top of or right next to another vehicle |
| **Flee** | Civilian drivers flee from hostile vehicles |
| **Mind the people** | Autopilots slow down for anyone on foot ahead |
| **Bystanders** | After a crash, people within 10 m flee and within 25 m cower |

NPCs only take vehicles that are unowned or belong to them or their faction. **Quest NPCs (essential or protected) and followers are
never used for traffic.**

---

## Settings

All optional, in `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\`:

| File | Section | Key | Default | What it does |
|---|---|---|---|---|
| `seamless.ini` | `[log]` | `verbose` | `0` | `1` = detailed log lines (needed for most bug reports) |
| `seamless.ini` | `[context]` | `enabled` | `1` | `0` = NPCs never take vehicles on their own (fights and traffic) |
| `seamless.ini` | `[hits]` | `damage` | `15` | Hit counter: damage per shot |
| `seamless.ini` | `[hits]` | `limit` | `0` | `0` = your own health; any other number = a fixed limit |
| `seamless.ini` | `[hits]` | `cooldown` | `30` | Seconds the autopilot stays off at the limit |
| `seamless.ini` | `[witness]` | `enabled` | `1` | `0` = bystanders do not react to crashes |
| `seamless.ini` | `[scanner]` | `key` | `84` (T) | The scanner marker key (Windows virtual-key code) |
| `seamless.ini` | `[view]` | `objectFade`, `actorFade` | `3`, `1` | Fade distance multipliers (1 = the game's own, 0 = never fade) |
| `seamless.ini` | `[keys]` | `debug` | `0` | `1` = manual test keys (listed in the log) |
| `nav.ini` | `[nav]` | `sampler`, `shape`, `rays` | `1` | `0` turns off the heading sampler, the hull sweeps, or all ray queries |
| `hoverui.ini` | | | | On-screen layout, written by the F7 editor |

---

## Compatibility

- **Vehicle mods** that add vehicles the game recognises (with the vehicle keyword) work with the autopilot and NPC rules.
- **Follower mods** work: followers come from the game's own list.
- **Grav jump mods** (Seamless Grav Jumps, FTL, Grav Lanes) do not interact with this mod.
- Not compatible with other mods that replace the vehicle physics step or the vehicle input handling.

---

## Status

**Tested in game:** hover flight, gears, the HUD buttons, summon / follow, NPC vehicles following with trail navigation, the gears.

**Built, not yet tested in game** (everything else), including: Auto and Cruise, the Auto behaviours and their button, scanner markers
and chains, catch from below, follower drivers, every escort and enemy behaviour, the hit counter, everyday traffic, vehicle-to-vehicle
right of way, the wall warning, night and weather, bystander reactions, passenger / taxi / take the wheel, the seat rules and forced
exits, drive mode, unlimited boost, view distance and the vehicle registry.

**Known risks:**
- Reading your health (rescue, the hit limit, picking up the wounded) is not proven yet. The log line `Vehicle hits: your health reads …`
  shows whether it works; without it the hit limit falls back to 300.
- Whether the game lets an NPC sit in **your own** vehicle (follower drivers) is the first thing to check in game.
- Cities are the least tested place: tight streets, many objects (watch the frame rate), and vehicles do not yet hold back their
  weapons near civilians.
- Quest aliases most likely set the same "essential" flag the mod checks, but that is not proven.

---

## Reporting a problem

1. Set `[log] verbose=1` in `seamless.ini`.
2. Play until it goes wrong.
3. Send `Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle.log` (and `SeamlessVehicle.prev.log` for the previous session). A crash
   report in the log lists the faulting address, the stack and what the plugin did in its last moments.

**Common fixes**
- **Nothing happens / no log:** the plugin only runs on Starfield **1.16.244** with SFSE for that build.
- **NPCs never use vehicles:** `SeamlessPilots.esm` is not enabled, or there are no free parked vehicles near them.
- **Parked props, not real vehicles:** `Vehicles - Real.esm` is not enabled, or Parked Vehicles is missing.
- **Frame drops with many vehicles:** try `[nav] rays=0`, or a lower `[view] objectFade`.

---

## For modders: building from source

Needs Visual Studio 2022 Build Tools (C++ workload) and CMake 3.24+.

```
git clone https://github.com/ianpatt/sfse ref/sfse
cmake -S source -B source/build -G "Visual Studio 17 2022" -A x64
cmake --build source/build --config Release --target SeamlessVehicle
```

Output: `source/build/Release/SeamlessVehicle.dll`. The SFSE headers must be the version for Starfield 1.16.244 (SFSE 0.2.21);
point `SFSE_ROOT` at them if they are not in `ref/sfse`.

```
Data/SFSE/Plugins/SeamlessVehicle.dll   the built plugin
Data/SeamlessPilots.esm                 the walk-to-vehicle quest package
Data/Vehicles - Real.esm                Parked Vehicles props -> real vehicles
source/                                 C++ source and CMake build (src/, tests/)
tools/                                  integrate.sh (build + install), make_vehicles_real.py, make_pilots_esm.py
docs/                                   ARCHITECTURE, ENGINE_NOTES, TESTING, KNOWN_ISSUES, research/
```

More detail: `docs/ARCHITECTURE.md`, `docs/ENGINE_NOTES.md`, `docs/TESTING.md`, `docs/KNOWN_ISSUES.md`, `docs/research/`.

## Credits

- **SFSE** team (ianpatt and contributors) for the script extender.
- **SarthesArai** for Parked Vehicles; **Aurelianis** for The Gang's All Here.
- Built with the help of Claude (Anthropic).

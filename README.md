# Vehicle Mod Overhaul (WIP)

### Hover vehicles, an autopilot that thinks, and NPCs who actually use vehicles - for Starfield

Compatible native vehicles become hover vehicles with gears, terrain following and a real autopilot. Call it, let it cover you, send it to a
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
| **Summon / Recall and Cover** | N brings your vehicle to you over anything it is doing; U makes it follow and defend you | Summon/follow tested; Recall/Cover untested |
| **Guard point and ram** | Hold T on a place: your vehicle guards it. Hold T on a person or vehicle: rammed in a straight line, flat out | Untested |
| **Stance** | One key cycles Defend / Attack / Hold fire for your vehicle and your escorts | Untested in game; offline checks pass |
| **Your vehicle defends you** | Under Cover or on a guard point your empty vehicle takes a defensive spot and fires its weapon; never while you drive it | Untested in game; offline checks pass |
| **Navigation / Combat / Weapons profiles** | Each vehicle keeps where it goes, how it fights and when it shoots separately; a fight picks the combat profile from the trip | Untested in game; offline checks pass |
| **Auto to the map marker** | Selecting a destination starts the autopilot; any movement key takes over | Untested |
| **Scanner markers** | Mark what you look at up to ~1 km away; **chain** several markers into a route | Untested |
| **Catch from below** | Jump or fall from a height and your following vehicle flies under you to catch you | Untested |
| **Follower drivers** | With a follower nearby, Cover or a marker sends the follower to drive your vehicle for you | Untested |
| **Escorts defend you** | Followers in vehicles intercept, shield, give overwatch and attack - enemy vehicles first | Untested |
| **Rescue, scout, wait** | A follower vehicle picks you up when you are hurt, scouts your marker, waits at the door | Untested |
| **Smarter enemies** | Enemy vehicles flank, intercept, block, hover overhead, retreat when damaged and call for help | Untested |
| **Vehicle hit counter** | Vehicles take "damage" from shots; at the limit their autopilot shuts down for a while | Untested |
| **Everyday traffic** | Outside fights, NPCs drive to far destinations, share rides, patrol outposts and run supply trips | Untested |
| **Vehicles respect each other** | No more vehicles "magnetised" together; right of way decides who gives way | Untested |
| **No more wall magnet** | Walls no longer pull the hover up their face; they become an early warning instead | Untested |
| **Shared 3D navigation** | Altitude cost fields guide local trajectory plans; bounded recovery holds failed trips with Retry / Skip and T T (clear) | Untested in game; offline checks pass |
| **Environment-aware traffic and combat** | Traffic detours check the world map; combat positions consider cover and automated fire requires a clear-shot probe | Untested in game; offline checks pass |
| **Checked boarding and drop-off** | Vehicle footprint, slope, water, headroom, standing space, and stopped arrival are checked before boarding or exit | Untested in game; offline checks pass |
| **Flies under things** | Overhangs and bridges are flown under; ceilings cap the height | Untested |
| **Knows the area up front** | The game's own collision objects are read into the vehicles' map, so planning starts with the walls already known | Untested |
| **Night and weather** | Autopilots slow down and fly a little higher at night and in rain, snow or dust | Untested |
| **Bystanders react** | People near a crash flee or cower | Untested |
| **Passenger, taxi, take the wheel** | Ride with an NPC driver, let them taxi you, or take over | Untested |
| **Metered boost, view distance** | Hold Left Shift for a 1.75x boost in any gear using the native boost meter; vehicles stay visible farther away | Untested in game; offline checks pass |

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

Every key means one thing, and the same thing on foot and in the vehicle: no mode changes what a key does. **N, U and T are fixed**
(your choice of keys: no fallback moves them). If the game also binds one of them, the log says so (`Rover keys: … is also bound by the
game`). The **stance** key is the first free one of Y, O, P, B, K, J, G; it and the other free keys are written to the log at start-up
(`Rover stance key: …`, `Rover flight keys: …`, `Passenger: key …`). Holds are all 1 second.

| Key | Tap | Hold (1 s) |
|---|---|---|
| **T** | Go there (destination, or a queued stop) | On a person or vehicle: ram it. On a place: guard it (on foot) or fly there and hold (driving) |
| **T T** | Clear the route | |
| **N** | Summon / recall your vehicle | Your vehicle **and** every escort; also works while you drive (escorts only) |
| **U** | Cover: your vehicle follows and defends you by itself | A free follower drives your vehicle to cover you |
| **Stance** | Defend → Attack → Hold fire | |
| **]** | On foot: your empty vehicle's profile (Scout / Return home / Hold). Driving: Stop / Orbit / Patrol / Cruise | |

### On foot

| Key | Action |
|---|---|
| **N** (tap) | **Summon / Recall your vehicle.** It comes to you whatever it is doing: covering, guarding, holding, ramming or fighting. Far away, it is moved in behind you, out of sight; closer, it flies to you. Pressing N again sends it again; it never cancels. With a follower driving, they bring it over without firing, park beside you and get out so you can drive. Your escorts are left in their fights. |
| **N** (hold) | The same, and every escort drops its fight and falls back to you for 15 s. While driving, a hold recalls the escorts. |
| **U** (tap) | **Cover** on / off: your vehicle follows and defends you by itself (see [Your vehicle defends you](#your-vehicle-defends-you)). While it guards a point, U makes it cover you again. If a follower already drives it, U toggles their cover; turning it off makes them get out. |
| **U** (hold) | A free follower (within 60 m) gets in and drives your vehicle to cover you. No follower free: your vehicle covers you by itself. |
| **E** (Activate) | **Get in** (shown within 6 m of your vehicle). At an occupied vehicle: tap = passenger seat, hold = take the wheel / pull the driver out. |

The button names follow the context: N reads **Summon** when your vehicle is idle and **Recall** when it is busy; U reads **Cover** or
**Stop cover**.

### T: tap, double tap, hold (aim with the hand scanner, on foot or driving)

| T | Action |
|---|---|
| **Tap** | **Go there**, in every situation. The aimed place becomes the destination and starts navigation, up to ~1 km away (past the loaded area the distance is an estimate, shown with `~`). Driving, the autopilot starts at once. On foot, your vehicle is summoned and drives you there when you get in. **While a trip is on, a tap adds the point to a chain** (`+N queued`). A tap acts once the 0.3 s double-tap window has passed. |
| **Double tap** | Clears the route and holds the vehicle where it is (it stays hovering). |
| **Hold (1 s)** | **Acts on what you aimed at when you pressed.** A highlighted person or vehicle is **rammed** straight away by your driven vehicle, or by your own vehicle. A place: on foot, your vehicle **guards** it (it holds there even in a fight and defends from it; U makes it cover you again). Driving, you fly there and hold, replacing the route. Releasing a hold selects nothing. Outside the scanner a hold asks you to aim first. |

**Ram:** a straight line, as fast as possible. The vehicle turns its nose onto where the target is heading and goes full throttle in
top gear. There is no path planning, no detour and no slowing for traffic; only the minimum height above the ground is kept. It ends
on impact, when the target is lost, or after 15 s, and any movement key takes your vehicle back.

Chained markers swap automatically: about 4 s before your vehicle reaches the current marker, the next one takes its place and
the vehicle flies on without stopping. The final point keeps normal arrival braking.

### In the vehicle

| Key | Action |
|---|---|
| **Space ×2** | Hover on / off |
| **W / S** | Accelerate / brake (double-tap W: keep accelerating) |
| **Left Shift** (hold) | 1.75x boost in the selected gear. Uses the game's boost meter and the vehicle's own burn/recharge timings; release to recover after depletion. |
| **Alt** (tap) | Cycle gears: 1 → 2 → 3 → 4 → 1 |
| **Alt** (hold) | Descend after 0.3 seconds; releasing a hold does not change gear |
| **A / D**, mouse | Turn / roll; the mouse aims |
| **Q / Z** | Up / down |
| **T** | See [T: tap, double tap, hold](#t-tap-double-tap-hold-hand-scanner-open-or-while-driving). There is no separate Auto toggle: selecting a destination starts the autopilot. Any of W / A / S / D takes control back. |
| **]** (when free) | What the autopilot does at your destination: **Stop → Orbit → Patrol** (the route loops) **→ Cruise** (fly where you look at your gear's speed) |
| **N** (hold) | Recall your escorts |
| **Stance** | Defend / Attack / Hold fire for your escorts (your own seat never fires by itself) |
| *Shooting* | Always yours. A vehicle you drive never fires or aims by itself. |
| *Leave the vehicle* | The old destination and queued route are discarded. Set a fresh target for the next trip; an unattended Auto vehicle holds position. |
| **F7** | Move any on-screen button (Page Up / Down pick, arrows move, Ctrl ×10, Delete resets). Saved to `hoverui.ini`. |

### On-screen buttons

Added prompts use the game's native BasicButton style at a uniform 75% scale. There is no Auto button or Auto destination-mode UI. Ambient NPC notices and rows are suppressed; only your followers appear.

Buttons only show when they can be used:
- **On foot** (if you own a vehicle): Summon / Recall, Cover / Stop cover, Get in (near your vehicle), and a status box:
  *SUMMONING 120 m*, *COVER 42 m*, *COVER · DEFEND 42 m* (fighting), *COVER · ATTACK*, *COVER · HOLD FIRE* (the stance), *GUARD POINT*,
  *GUARD POINT · DEFEND*, *HOLDING*, or the empty vehicle's navigation profile (*SCOUT*, *RETURN HOME*). A stance change shows a notice.
- **Follower rows**, one per follower in a vehicle: an active order shows alone (*Sarah  RECALL  85 m*, *COVER*, *GUARD POINT*,
  *HOLDING*, *Sarah  RAMMING*); otherwise the navigation profile, the combat profile during a fight, and *HOLD FIRE* only when
  the weapons are held (*Sarah  PATROL  120 m*, *Sarah  PATROL · ATTACK  120 m*, *Sarah  HAULER · DEFEND · HOLD FIRE  200 m*).
- **In the vehicle:** W / A / S / D, Alt hold to descend / Alt tap for gear / Hover, the gear box, the trip line (trip states,
  *AIM + T* with no destination, *RAMMING*) and the follower rows.
- **Target:** the scanner readout uses the former Gang box's saved position, with the same native style and size. Select **Target** in F7 to move it. The Gang summary is removed.
- **Route display on the HUD/scanner:** native status boxes for the destination and queued stops, with native **Retry / Skip** key prompts and *T T: clear route*. Select **Route** in the F7 editor to move the group.

---

## Current fixes and controls (0.61.0)

- Hover and ground-gear forces bind to the native chassis only after its full body ID matches a collision body owned by that reference. Nearby parked vehicles never inherit your controller. Collision recovery uses signed world-up attitude, without learning reversed controls from impacts. First-person rover pitch is measured relative to the chassis, with separate camera baselines.
- Flight rates, acceleration, braking distance and swept clearance use each vehicle's width, length, height and live scale. Long hulls turn/pitch more slowly; wide hulls roll more slowly; faster turns are limited by lateral acceleration. Valid large bounds are retained. This applies to compatible native vehicles without a model-name list; missing bounds use a conservative fallback.
- Only your own vehicle follows you. Other NPC vehicles keep their schedule/combat behavior and hold their own position between fights instead of forming up behind you.
- Traffic checks the actor's native path before boarding: interior points, changes of worldspace, paths near doors, short walks, and unresolved path context stay on foot.
- **Ram:** hold T (1 s) while the scanner highlights a person or vehicle. Your driven vehicle, or your own vehicle, rams that exact target in a straight line, flat out (no planned path). The target comes from the scanner's own pick data (the public `MonoclePickRefData` provider), never from a guess based on a viewing cone. It ends on impact, when the target is lost, or after 15 s; it no longer depends on your combat flag. Refusals are logged with their reason. Movement controls take over as usual.
- Scanner target range reads m/km and includes elevation; a selected place uses the number displayed by the scanner. Exiting a driver/passenger seat clears the route and consumes the previous target until a new selection is made.
- Native NPC dismount requests are allowed. Repeated requests preserve their first timeout; unsafe or failed exits release mod control instead of retrying forever. The near-goal trail escape loop found in the organic-play log is also corrected.

Release builds and offline regression checks pass. These fixes have not yet been verified in a running game. Persistence remains paused, and no in-game test controls are added.

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

### Your vehicle defends you
Under **Cover** (U) or on a **guard point** (T hold on a place), your **empty** vehicle fights for you:
- **Enemy awareness:** twice a second it looks for your enemies within 120 m of you (or of the guard point), enemies in vehicles first, and keeps its target out to about 156 m.
- **Stance:** Defend (the start) shields you, Attack closes to 25 m from the enemy (never more than 120 m from you or the guard point), and Hold fire keeps the weapon quiet.
- **Defend positioning:** under Cover it moves to a spot between you and the threat (12 m from you, never past the enemy). On a guard point it stays at the point. Either way it then picks the nearby spot with the best cover and line of fire. A parked vehicle lifts off to fight, and it does not park while a fight is on.
- **Its weapon:** at its spot it turns to face the threat and fires short bursts, only with the nose on target, within 80 m and with a clear line of fire.
- **Never while you drive it,** and not while a follower drives it (they fight as escorts). The fight ends 8 s after the last enemy, and the trigger is always released.

### Profiles: Navigation, Combat, Weapons
Each vehicle holds one profile of each class, so choosing where it goes never replaces how it fights or when it shoots:

| Class | Profiles |
|---|---|
| Navigation | escort, hold, return home, patrol, hauler, courier, scout, cruise |
| Combat | attack, defend (a ram is a one-shot order, not a profile) |
| Weapons | free fire (default), opportunistic (shoots only what is in front, never turns for a shot), hold fire, never |

When a fight starts, the combat profile follows from the navigation profile: escort, scout, cruise, hauler, courier and return home
**defend**; patrol and hold **attack**. It ends again 20 s after the last enemy. Your orders sit above the profiles for as long as they
run: **Recall** has no combat and holds fire; **Cover** defends without chasing and shoots opportunistically; a **guard point** holds the
point and defends from it; a ram holds fire while ramming. The player's seat is always "never": a vehicle you drive
never shoots by itself.

The **stance** key sets the Combat and Weapons classes of your vehicle and every follower's vehicle: Defend, Attack, or Hold fire
(defend, weapons held). Everyone else's vehicles keep the defaults from their navigation profile.

### Catch from below
When you are more than 4 m above the ground (jumping off a ledge, jetpacking, falling), your following vehicle flies to a point about
7 m under where your fall will take you, so it can catch you. It moves smoothly (no wobbling) and goes back to normal following once
you land.

---

## Followers

Followers are read from the game's own follower list.

### Follower drivers (crew)
If a follower is free (on foot, within 60 m) when you **hold U** or set a **scanner marker**, your vehicle is brought to you and
**the follower gets in and drives it**. **N** never hands your vehicle over, because N means you want to drive.

| You press | The follower… |
|---|---|
| U hold | drives your vehicle after you and defends you without chasing enemies away; picks up other followers as passengers |
| T hold on a place | holds that guard point, even in a fight, and defends from it |
| Scanner marker | drives to your marker and stops there |
| N | brings your vehicle to you without firing, parks within 10 m and gets out so you can drive |
| U again | gets out (cover off) |

No follower free? The buttons drive your empty vehicle instead. Your vehicle's own autopilot leaves it alone while a follower drives.

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
Hold **N** to recall them for 15 s.

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
comes down. Friendly fire does not add to the damage counter, but shooting another person's vehicle still counts as aggression toward its occupants/owner. It heals after 15 s without hits.

---

## Everyday traffic (outside fights)

When nobody near you is fighting, NPCs within 200 m use vehicles on their own (at most 2 at a time, so fights always have vehicles free):

| Behaviour | What happens |
|---|---|
| **Transit** | An NPC walking somewhere over 300 m away, with a vehicle it may use within 30 m, drives there, parks and walks the rest |
| **Shift change** | When an NPC's daily schedule moves them on to a far place, they set off by vehicle straight away |
| **Shared ride** | Another NPC going the same way rides along |
| **Offer a ride** | A driver passing you on foot stops for 10 s - tap the vehicle to ride along |
| **Outpost patrol** | An owner with a suitable outdoor path may drive two laps round the outpost and park again |
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

**Previously tested in game:** older hover flight, gears, HUD buttons and summon/follow builds. The current changes below still need organic gameplay verification.

**Built, not yet tested in game** (everything else), including: the T tap / double tap / hold controls, the straight-line ram and its
scanner pick data, N and U tap / hold, the stance key, the ] autopilot modes, guard points, your empty vehicle's defence, the Navigation / Combat / Weapons profiles and their row
texts, the Target readout in the former Gang slot, Auto and Cruise, scanner markers
and chains, catch from below, follower drivers, every escort and enemy behaviour, the hit counter, everyday traffic, vehicle-to-vehicle
right of way, the wall warning, night and weather, bystander reactions, passenger / taxi / take the wheel, the seat rules and forced
exits, drive mode, metered boost, view distance and the vehicle registry.

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

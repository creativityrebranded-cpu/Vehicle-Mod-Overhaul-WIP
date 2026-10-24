# Architecture

Flying Vehicle Overhaul (FVO) is one SFSE plugin (`FlyingVehicleOverhaul.dll`, Starfield 1.16.244 only). It runs standalone; the optional
SeamlessPlanets core DLL, when present, supplies the per-frame callback instead of the plugin's own task. Everything below runs
on the game's main thread unless a section says otherwise.

## One frame (`main_vehicle.cpp`, `Frame()`)

```
modscene::NewFrame()      the shared scan of loaded references starts over
world::Publish()          the shared vehicle list (position, velocity, size, who uses it)
routeplan::Frame()        the lane planner's copy of that list
vehicleflight::Tick()     your vehicle: hover, autopilot, summon / follow, avoidance
navmap::Tick()            shared map: eviction, 10 s stats line
shipvehicle / realvehicles::Tick()
npcvehicles::Tick()       NPC jobs (boarding, driving, exit), riders, follower HUD rows
passenger::Tick()         riding as a passenger / taxi / take the wheel
traffic::Tick()           destination probe
profiles::Tick()          profile keys
context::Tick()           the rules engine (below)
viewdist::Tick()          view distance setting
```

Other threads: the vehicle physics step (`vehicleflight` StepDetour, a hook), the boost update (BoostDetour), the ship-system
update (`vehicle_probe` Detour: ground probes, forward rays, hull sweeps) and the HUD movie advance (`hoverui`). State shared with them is
passed through small locked structs or atomics (see `prefetch.h`).

## Modules

| Module | Owns |
|---|---|
| `vehicleflight.*` | Hover flight model, autopilot (`Fly`), terrain assist / avoidance corridors, N summon / recall, U cover and guard points, the empty own vehicle's cover combat (`CoverFightTick`), straight-line ram orders, drive mode, flip recovery, native-meter boost, `PilotFly(pilot, vehicle, Goal)` |
| `npcvehicles.*` | NPC jobs: walk to a vehicle (quest package), instant seat, follow / orbit actions, park / relaunch, attack run and vehicle weapon, exit service, riders (passengers), leash, occupied-vehicle rules (tap / hold E), forced exits |
| `context.*` | The rules engine: who boards what, which profile, when to get out |
| `vcontract.*` | The contract between profile code and the owners: `Goal`, `Profile`, one assignment per vehicle **per class** (Navigation / Combat / Weapons), `SeatProvider`, `ExitRequest` |
| `profiles.*` | The profile table. Navigation: escort, hold, return home, patrol, hauler, courier, scout, cruise. Combat: attack (the former pursuit goal), defend. Weapons: free fire, opportunistic, hold fire, never |
| `overrides.h`, `profileclass.h`, `coverfight.h`, `targetinput.h`, `ramlogic.h` | Pure rules (offline-tested): the player's N / U / T overrides, class defaults, the weapons gate and row texts, own-vehicle cover combat, the T gestures, ram arming / ending |
| `navmap.*` | Shared world map (4 m cells: ground, floors, solid, free, contacts), `PathClear`, heading sampler |
| `vehicle_probe.*`, `prefetch.h` | Ground probes, forward rays and hull-sized shape sweeps run inside the game's update; request / result slots |
| `routeplan.*` | 3D lane planner: predicts every vehicle about 10 s ahead and picks a lane / side / speed so they do not meet |
| `vehicleworld.*` | The shared vehicle list (`world::Vehicles`) |
| `realvehicles.*` | Vehicle registry, Parked Vehicles integration, duplicate cleanup |
| `passenger.*` | Passenger / taxi / take the wheel |
| `pilotquest.*` | Drives the quest aliases of `FVO_NPCPilots.esm` (the walk-to-vehicle package) |
| `vehicleaim.*` | Aims NPC vehicle shots at launch |
| `hoverui.*` | The on-screen buttons (Scaleform), the F7 layout editor, the gang line and vehicles box |
| `controlmap.*` | The game's key bindings, and the mod's one key table (`Claim` / `Note` / `LogTable`) |
| `viewdist.*` | View distance (fade multipliers) |
| `crash.*`, `log.*` | Crash report with plugin breadcrumbs; the log and its verbose switch |
| `modscene.*` | Loaded-reference scan, shared once per frame |

## NPC vehicle jobs (`npcvehicles.cpp`)

`Job` phases: `Idle -> Boarding -> Following -> Settling -> Leaving -> Idle`. At most 6 at once (pilots 1 to 6 of `vehicleflight`).

1. **Boarding.** `BoardNpc(actor, vehicle, asDriver)` takes a free slot. The NPC is given the vehicle's seat keyword and walks (or runs in combat or when the
   vehicle is over 40 m away) with a quest package from `FVO_NPCPilots.esm`. When it stands within about 3.5 m it is seated with the game's instant
   seat (`kAnimatedEntry` is off). A passenger job (`seat >= 1`) frees its slot once seated and is kept in `g_riders`.
2. **Following.** Each frame the Navigation profile gives the goal (`vcontract::GoalFor`; escort is a follower's default). A hostile pursuer has no
   navigation profile; its Combat profile `attack` gives the goal instead. When `PickFoe` finds an enemy and no Combat profile is set, one is chosen from
   the navigation profile (`profileclass::DefaultCombat`: patrol / hold attack, the rest defend) and cleared 20 s after the last enemy. `attack` runs
   `PursueGoal` and attack runs; `defend` runs `DefendGoal`; `CombatPosition` refines both. The crew orders (Recall, Cover, Guard, Hold, Marker;
   `overrides.h`) sit above the classes: Recall drops the fight and holds fire, Cover defends without chasing, Guard holds its point over a fight.
   The Weapons profile (and the override) decides `MayFire` / `MayAim`. Hostility comes only from `ApplyProfile` (escort / pursuit switch), never from
   a navigation profile. This file keeps the actions: park when you stand still, relaunch, attack run, aim and burst fire, re-aggro.
   `SubmitFire` is the single weapons gate: a vehicle the player drives never fires by itself.
3. **Leaving.** The exit service (`RequestExit`, `UnloadVehicle`, `ExitStateOf`): the game's animated dismount, then the instant exit. The leash (600 m
   from where the NPC started) and going indoors also end following.

A fight is any NPC with the combat flag (vtable `+0xB58`) or a combat target (`+0x298`).

## Context engine (`context.cpp`)

About every 0.5 s (the actor scan every 1 s, up to 8000 loaded references):

- **Fight:** an NPC in combat within 80 m of you, not in a vehicle, with a free vehicle within 40 m of it (never your own rover) is sent with `BoardNpc`.
  Followers become escort, everyone else pursuit. A follower with no free vehicle near boards a vehicle an NPC drives as a passenger.
- **Profile:** once a job is following, `SetVehicleProfile` switches escort / pursuit through `ApplyProfile`, which assigns the classes (pursuit: Combat `attack`, no escort navigation).
- **Calm:** the player and the driver not in combat, and the vehicle under 2 m/s for 60 s: `UnloadVehicle` (everyone out).
- Off switch: `settings.ini [context] enabled=0`.

## Contract (`vcontract.h`)

A profile is a goal source: `bool goal(Context&, Goal&)`, registered once, assigned per vehicle (`Assign(vehicleId, profile, homeX, homeY)`).
`Goal` carries the target point, height, stop distance, hold, nav settings (`NavParams`: speed limit, margin, squeeze, priority, ...) and the
flight treatment (`keepGoing`, `timeoutMs`, `cruise`). `GoalFor(user, pilot, vehicle, player, out)` is the hook the owners call: the NPC hook in
`npcvehicles`, the player hooks in `vehicleflight` (2a: you driving with Auto on; 2b: your unattended vehicle). `SeatProvider` (board / exit / unload / state)
is implemented by `npcvehicles` and registered at install.

## Navigation and the terrain assist

Both modes use one avoidance controller in `vehicleflight.cpp` (`Corridor`, then the assist states: clear, climbing over, turning left or right, stopped).
Corridors are cast along the autopilot's heading plus its detour. Evidence per frame: three ground samples per direction (12 to 40 m scaled by speed),
thin forward rays, one hull-sized sweep, and the shared `navmap` (ground, floors, solids, bumps). When the way ahead is blocked the autopilot tries, in
order: your trail, the heading sampler (`navmap::ChooseHeading`), then the old stepped detour. `routeplan` adds lane / side / speed so vehicles
do not meet. Known gap: the assist and the autopilot still react to each other frame by frame; see `KNOWN_ISSUES.md`.

## Files the mod reads and writes (`Documents\My Games\Starfield\SFSE\Logs\`)

`FlyingVehicleOverhaul.log` (+ `.prev.log`), `FlyingVehicleOverhaul\settings.ini` (`[log]`, `[context]`, `[keys]`, `[view]`), `FlyingVehicleOverhaul\nav.ini` (`[nav]`
`sampler`, `shape`, `rays`), `FlyingVehicleOverhaul\hoverui.ini` (HUD layout), `FlyingVehicleOverhaul\vehicles.tsv` (registry).

## Data plugins

`FVO_ParkedVehiclesPatch.esm` (`tools/make_vehicles_real.py`): overrides Parked Vehicles so each prop becomes the real vehicle.
`FVO_NPCPilots.esm` (`tools/make_pilots_esm.py`): one quest with 6 pilot aliases, 6 vehicle aliases and 6 "activate the vehicle alias" packages;
the plugin fills the aliases with the engine's own `ForceRefTo`.

## 0.61.0 integration

Physics callbacks have no default player owner. Each controller receives a bounded set of collision body IDs from its vehicle's scene; a matching native callback claims the chassis, and all subsequent force writes check the full generation-bearing ID. Chassis origin offsets and decorative collision bodies do not permit proximity-based ownership.

`hulllogic` derives attitude rates, lateral acceleration, thrust scale and braking distance from validated hull dimensions. MPC sweeps both length and width and limits candidate turns to achievable rates. `boostlogic` updates the actual native charge/delay fields using vehicle configuration timings while hover supplies its own force. Left Shift buys a 1.75x boost. `altcontrol` distinguishes a released short Alt tap (cycle gears 1–4) from a 300 ms hold (descend), cancels pending taps on focus/vehicle changes and prevents a hold release from changing gear. Right Shift has no mod action.

`traffic::Destination` carries a conservative vehicle-suitability decision from the counted native path: every point must resolve to the current exterior world, without a nearby door. Context and the transit entry point both enforce it. Vehicle projectile intersections notify native driver/owner combat and interrupt civil tasks, with only the explicit aggressor prioritized for retaliation. Ambient behavior is logged; HUD notices/rows require an actual follower.

`targetsession` invalidates consumed marker coordinates/handles at seat exit; new scanner selection re-arms them. A T hold (`targetinput`, 1 s) on the scanner-picked actor (`MonoclePickRefData`) issues a bounded ram order (`RequestRam`, `ramlogic`). `Fly` then drives it through the command override: the nose on the led point, full throttle, top gear, no strafe or brake, and no stuck recovery; only the height hold stays with the autopilot. N / U use `keygesture` (tap on release, hold once at 1 s). The stance key (`npcvehicles::CycleStance`) keeps the Combat / Weapons classes of the own vehicle and every follower's vehicle on Defend, Attack or Hold fire (`ApplyStance`). Native exits are forwarded and queued for bounded cleanup; polling cannot restart a deadline.

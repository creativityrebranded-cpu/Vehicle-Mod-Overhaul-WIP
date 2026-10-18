# Architecture

SeamlessVehicle is one SFSE plugin (`SeamlessVehicle.dll`, Starfield 1.16.244 only). It runs standalone; the optional
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
npcvehicles::Tick()       NPC jobs (boarding, following, exit), riders, HUD gang rows
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
| `vehicleflight.*` | Hover flight model, autopilot (`Fly`), terrain assist / avoidance corridors, summon / follow / Auto, drive mode, flip recovery, boost pin, `PilotFly(pilot, vehicle, Goal)` |
| `npcvehicles.*` | NPC jobs: walk to a vehicle (quest package), instant seat, follow / orbit actions, park / relaunch, attack run and vehicle weapon, exit service, riders (passengers), leash, occupied-vehicle rules (tap / hold E), forced exits |
| `context.*` | The rules engine: who boards what, which profile, when to get out |
| `vcontract.*` | The contract between profile code and the owners: `Goal`, `Profile`, assignment per vehicle, `SeatProvider`, `ExitRequest` |
| `profiles.*` | The profile table: escort, pursuit (goal code), hold, return home, patrol, hauler, courier, scout, cruise |
| `navmap.*` | Shared world map (4 m cells: ground, floors, solid, free, contacts), `PathClear`, heading sampler |
| `vehicle_probe.*`, `prefetch.h` | Ground probes, forward rays and hull-sized shape sweeps run inside the game's update; request / result slots |
| `routeplan.*` | 3D lane planner: predicts every vehicle about 10 s ahead and picks a lane / side / speed so they do not meet |
| `vehicleworld.*` | The shared vehicle list (`world::Vehicles`) |
| `realvehicles.*` | Vehicle registry, Parked Vehicles integration, duplicate cleanup |
| `passenger.*` | Passenger / taxi / take the wheel |
| `pilotquest.*` | Drives the quest aliases of `SeamlessPilots.esm` (the walk-to-vehicle package) |
| `vehicleaim.*` | Aims NPC vehicle shots at launch |
| `hoverui.*` | The on-screen buttons (Scaleform), the F7 layout editor, the gang line and vehicles box |
| `controlmap.*` | The game's key bindings, and the mod's one key table (`Claim` / `Note` / `LogTable`) |
| `viewdist.*` | View distance (fade multipliers) |
| `crash.*`, `log.*` | Crash report with plugin breadcrumbs; the log and its verbose switch |
| `modscene.*` | Loaded-reference scan, shared once per frame |

## NPC vehicle jobs (`npcvehicles.cpp`)

`Job` phases: `Idle -> Boarding -> Following -> Settling -> Leaving -> Idle`. At most 6 at once (pilots 1 to 6 of `vehicleflight`).

1. **Boarding.** `BoardNpc(actor, vehicle, asDriver)` takes a free slot. The NPC is given the vehicle's seat keyword and walks (or runs in combat or when the
   vehicle is over 40 m away) with a quest package from `SeamlessPilots.esm`. When it stands within about 3.5 m it is seated with the game's instant
   seat (`kAnimatedEntry` is off). A passenger job (`seat >= 1`) frees its slot once seated and is kept in `g_riders`.
2. **Following.** The vehicle's profile gives the goal each frame (`vcontract::GoalFor`). Escort and pursuit goals live in `profiles.cpp`; this file keeps the
   actions: park when you stand still, relaunch, attack run, aim and burst fire (pursuit), re-aggro.
3. **Leaving.** The exit service (`RequestExit`, `UnloadVehicle`, `ExitStateOf`): the game's animated dismount, then the instant exit. The leash (600 m
   from where the NPC started) and going indoors also end following.

A fight is any NPC with the combat flag (vtable `+0xB58`) or a combat target (`+0x298`).

## Context engine (`context.cpp`)

About every 0.5 s (the actor scan every 1 s, up to 8000 loaded references):

- **Fight:** an NPC in combat within 80 m of you, not in a vehicle, with a free vehicle within 40 m of it (never your own rover) is sent with `BoardNpc`.
  Followers become escort, everyone else pursuit. A follower with no free vehicle near boards a vehicle an NPC drives as a passenger.
- **Profile:** once a job is following, `SetVehicleProfile` assigns escort or pursuit through `vcontract`.
- **Calm:** the player and the driver not in combat, and the vehicle under 2 m/s for 60 s: `UnloadVehicle` (everyone out).
- Off switch: `seamless.ini [context] enabled=0`.

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

`SeamlessVehicle.log` (+ `.prev.log`), `SeamlessVehicle\seamless.ini` (`[log]`, `[context]`, `[keys]`, `[view]`), `SeamlessVehicle\nav.ini` (`[nav]`
`sampler`, `shape`, `rays`), `SeamlessVehicle\hoverui.ini` (HUD layout), `SeamlessVehicle\vehicles.tsv` (registry).

## Data plugins

`Vehicles - Real.esm` (`tools/make_vehicles_real.py`): overrides Parked Vehicles so each prop becomes the real vehicle.
`SeamlessPilots.esm` (`tools/make_pilots_esm.py`): one quest with 6 pilot aliases, 6 vehicle aliases and 6 "activate the vehicle alias" packages;
the plugin fills the aliases with the engine's own `ForceRefTo`.

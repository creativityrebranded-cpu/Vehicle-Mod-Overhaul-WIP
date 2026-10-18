# SeamlessVehicle — whole-design structure (proposal, 2026-10-17 evening)

Written for every session working on the vehicle mod. Builds on `VEHICLE_PROFILES_PLAN_20261017.md` (profiles = goal +
height + fire rules on one autopilot) and `NPC_DRIVING_BEHAVIOR_PLAN.md`. Nothing here is built; it is the target shape
and the order to get there without a rewrite.

## Where we are (2026-10-17 20:4x)
| File | Lines | Holds today |
|---|---|---|
| `vehicleflight.cpp` | 3013 | physics-step hook + flight model + ground rays + assist/avoidance + contact policy + player input + autopilot (pilots 0-3) + summon/follow + taxi + HUD readout |
| `npcvehicles.cpp` | 1900 | NPC search, boarding, jobs, profiles (escort/pursuit), goal points, aim point, fire submit + input gate, death stop, gang |
| `hoverui.cpp` | 1450 | Scaleform buttons, layout, F7 editor |
| `realvehicles.cpp` | 624 | vehicle-form discovery, 2 s registry, vehicles.tsv |
| `passenger.cpp` | 290 | player as passenger (**not in CMakeLists**) |
| `vehicleaim.cpp` | 115 | Projectile::Launch hook → `npcvehicles::AimFor` |
| `flightmodel / shipmodel / shipvehicle / flighttune / camtune` | | ship-flight leftovers from SeamlessPlanets (built, mostly unused by the rover) |

Problems this shape causes (all seen today):
1. **Two files own everything.** Two sessions edited `vehicleflight.cpp` within minutes (contact fix, taxi) and had to
   hand the file back and forth.
2. **No single owner per vehicle.** Player input, pilots 0-3, NPC jobs, passenger steering and taxi each decide on their
   own who flies a vehicle; conflicts are found by test.
3. **Three ideas of "what is a vehicle".** realvehicles (form list — counted 827 chairs until 20:2x), npcvehicles
   (`FUN_140EC72A0`), vehicleflight (the driven ref). Avoidance has no list of nearby vehicles at all.
4. **Policies buried in the model.** The contact rule (what ends a flight), the speed cap and the "ground ahead loading"
   label sat inside the 3000-line step code; one wrong line ended every low flight.
5. **Threads by convention.** Physics step (Havok thread), Projectile::Launch (any thread), HUD Advance (render) and the
   frame task each read shared state with their own ad-hoc locks.

## Target structure: five layers, dependencies point down only
```
 5  ui/        hoverui (HUD + F7 editor), notify, readouts            reads 4 snapshots, never writes game state
 4  behaviour/ jobs, profiles (goal/height/fire rules), seat broker,  decides WHAT each vehicle should do
               crime rules, taxi, summon/follow, gang
 3  control/   controller table (one owner per vehicle), player pilot, decides HOW: produces a Command per vehicle
               autopilot (goal -> Command), assist/avoidance, contact policy, speed policy
 2  sim/       flight model in the physics step (Command -> forces),   applies; no decisions
               weapon (fire submit + launch aim), seat/exit/crime actions
 1  world/     VehicleWorld: per-frame list of nearby vehicles (pos, vel, base, seats, occupants, owner),
               registry (2 s, persisted), sensors (ground rays, casts), snapshot publisher
 0  engine/    addresses, hooks, typed wrappers (Ref, Actor, Vehicle, Camera, LaunchData), no state, no policy
```

### The few contracts that matter
- **`world::Vehicles()`** — the one vehicle list, rebuilt every frame on the main thread from loaded cells, keyword-gated
  (pool of 20 today). Avoidance, NPC search, registry, HUD all read it. The 2 s registry/tsv becomes a consumer of it.
  - Each entry carries the vehicle's **box half-extents from its live Havok body AABB**, not the FURN OBND (OBND is right
    for REV-8 2.5x4.6x2.0, Moon Jumper 4.0x5.5x3.3, Deimog 2.7x6.2x3.4, but bogus for mods: V-47 reads 3.3x0.6x3.3,
    another 0). `engine/` offers a box-cast query that `sim/flight` and `assist` use for shape-aware passability.
  - **Keyword gate stays strict**: only bases with the vehicle keyword; the FURN list's 1054 chairs/beds/markers were the
    New Atlantis bug.
- **`control::Owner(vehicle)`** — exactly one of `Player`, `Autopilot(job)`, `Passenger(job)`, `None`. Taking control is a
  request that the table grants or refuses (and logs). Any W/A/S/D from the player while aboard = request for Player.
- **`control::Command`** (exists in `vehicleflight.h`): throttle, strafe, vertical, yaw rate, pitch, roll, gear, hold. The
  player pilot and the autopilot both only produce a Command; the step only consumes one. Contact and speed policy sit
  between them, in one small file each (`contact_policy.cpp`: grace 2 s / lift-off, push-up = floor, side crash >= 20 m/s).
- **Profile** = `{ goal source, height rule, fire rule, target ref, park rule }` (as the profiles plan says); a job is
  `{ vehicle, driver, profile, owner-of-control token }`. Escort, pursuit, taxi, guard, flee, patrol, convoy, scout, traffic
  are rows, not code paths.
- **Seat broker** — every get-in / get-out / hand-over / pull-out goes through one function per vehicle, one change at a
  time, rule-checked and logged (`Seat: who, seat, rule, outcome`). Boarding code in npcvehicles and passenger.cpp move here.
- **Weapon** — `weapon::FireAt(vehicle, targetRef)`: fire submit (input gate), nose gate fallback, and the launch-aim
  snapshot (`AimFor` today) keyed by target ref, not "the player".
- **Snapshots across threads** — the main thread publishes immutable per-frame snapshots (`VehicleState`, `AimTarget`,
  `HudState`); the physics step, Launch hook and HUD Advance read only snapshots (one pattern, one lock helper).

### Data side
- `Vehicles - Real.esm` (generated by `scripts/make_vehicles_real.py`) owns "parked props are real vehicles"; the DLL
  never creates vehicles at run time. Per-vehicle tuning profiles (thrust, top speed, hover height, turn rates) in one ini
  keyed by base form, REV-8 the default.

## Session ownership (to stop file collisions)
One session owns a layer's files at a time; cross-layer changes go through the header contract and a message.
| Owner | Files |
|---|---|
| Flight session — **taken by "Seemless last activity"** (20:5x) | `sim/flight*`, `control/*` (today: `vehicleflight.cpp`, `vehicle_probe`, prefetch rays); also builds the controller table and seat broker itself (others send ideas, not edits) |
| NPC / behaviour session — **taken by "Seemless last activity"** | `behaviour/*`, `sim/weapon*` (today: `npcvehicles.cpp`, `passenger.cpp`, profiles; `vehicleaim.cpp` after the fork hands it over) |
| World / data session — **taken by "Seemless last activity (fork)"** (20:5x) | `world/*`, `engine/*`, `scripts/make_vehicles_real.py`, `Vehicles - Real.esm`, `realvehicles.cpp`, read-only engine research (shape casts, body AABB, health, credits); hands `vehicleaim.cpp` to behaviour once the +0x2C heading fix is confirmed in game |
| UI | `ui/*` (`hoverui.cpp`) — whoever, announced first |
Builds and installs: announce, build from the shared tree, back up the DLL, say what the build contains.

## Migration order (each step builds and is testable alone; no big-bang)
1. **Test the current build first** (contacts, launch aim, taxi T1, the 52 parked vehicles). Bugs before structure.
2. **world/VehicleWorld**: per-frame keyword-gated list; realvehicles' registry reads it. Small, used at once by step 4.
3. **Split `vehicleflight.cpp` without changing behaviour** — *only after one clean test of the current build* (agreed
   20:5x: uncommitted, untested Taxi Auto / forward obstacle sensor / contact grace live in it; a move now would bury the
   test results). Done by the flight owner.: `flight_step.cpp` (physics hook + model), `ground.cpp`
   (rays), `assist.cpp` (avoidance + speed policy), `contact_policy.cpp`, `pilot_player.cpp`, `autopilot.cpp`,
   `summon_follow.cpp`. Pure moves + header; one session, one sitting, logs unchanged so test runs compare.
4. **Vehicle-to-vehicle avoidance** in `assist.cpp` from VehicleWorld (15 m + 1.5 s of speed; player first, then faster).
5. **Controller table** (`control/owner.cpp`), then move player/job/passenger/taxi to request control through it.
6. **Seat broker**, folding npcvehicles boarding and passenger.cpp in; passenger.cpp joins the build here.
7. **Profiles as data** (goal source enum, target ref) — the profiles plan's shared groundwork 1-4.
8. **Weapon module** with target ref (Guard/Scout need it).
9. Research tracks that can run beside 2-8: engine shape/linear casts for overhead clearance (`hknpVehicleLinearCastWheelCollide`,
   layer mask 0x22006 misses platforms), vehicle health, credits add/remove.
10. Retire or isolate the ship-flight leftovers (`flightmodel/shipmodel/shipvehicle/flighttune/camtune`) from the vehicle DLL.

## Rules carried over (do not lose them in the split)
- Never learn gravity/drag from per-step velocity deltas (hover runaway, memory note).
- Contact policy as above; "slowed: ground ahead loading" only when the ground ahead is really not loaded.
- Decompile first, then build; complete instrumentation; one log prefix per module.
- Short on-screen labels; exact key names; no key conflicts.

# Shared vehicle systems — 2026-10-20

Schedules, packages, profiles, crew orders, and combat targets choose destinations. They use the same world observations and navigation services to carry them out. Save/load persistence is paused at the user's request: this build adds no serialization or restore hooks. Loading a save resets transient jobs, fields, and probe results as before.

| Consumer | Shared services and integration |
|---|---|
| Player Auto, follow, summon flight | Collision-seeded nav map, altitude cost field, MPC, vehicle prediction, terrain/ceiling reactions, bounded recovery, common arrival/status. The explicit far summon relocation also requires a validated landing site. |
| NPC drivers and follower crew | `PilotFly` preserves the full goal contract, including height, hold, cruise, speed, and pass-through. Movement uses the same flight controller and recovery as player Auto. |
| Civil traffic and native travel schedules | `traffic` supplies package/path destinations through the existing context and NPC job owners. Transit, return home, patrol, supply, parking, shared rides, and rescues use the common pilot. Parking and unloading require site validation. |
| Profile goals | Escort, pursuit, courier, hauler, scout, cruise, hold, and return home use the same pilot and navigation parameters. Patrol waypoints use the common height/data checks while allowing passage at speed. Hauler waits start after settled arrival and a validated site. |
| Vehicle-to-vehicle avoidance | Predicts the shared vehicle snapshot. Proposed lanes and side steps now also check the known environment over the predicted manoeuvre, including hull height and thin decks. Newly blocked held manoeuvres are cancelled; no usable detour means braking. |
| Combat movement | Existing role/enemy selection supplies the nominal goal. Nearby alternatives are scored for mapped cover, outgoing visibility, occupancy, and field reachability. Close attack aiming changes yaw/pitch only near the goal at low speed; it no longer replaces planned throttle or strafe. |
| Automated weapons | A fresh shared ray must establish a clear shot or hit the intended target. Pending evidence, intervening walls, and unrelated actors block firing. The test includes obstacles without form IDs. |
| Taxi and passenger steering | Marker height and hold semantics reach the common pilot. Taxi completion requires settled arrival and safe exit space. Look-to-steer remains a goal source. |
| Boarding, seat swaps, NPC/player exits | Shared site probes verify dry support over a nine-point vehicle footprint, slope/edges, overhead clearance, vehicle occupancy, a standing position, and a path beside the hull. Moving or airborne vehicles cannot unload. Instant fallback helpers use the same gate. Missing proof waits or refuses. |
| Scanner routes | Existing selected place names remain destination context. Failed routes hold with Retry/Skip/Clear controls; the route chain cannot advance from an unreachable state. |

## Navigation and lifecycle

The field covers 125 × 125 horizontal cells at 4 m spacing, with nine altitude layers 8 m apart. Rasterization also samples intermediate heights at 2 m spacing so thin decks cannot become vertical connections. Unknown columns cannot authorize climbs. Local rollouts sample both hull sides and its upper/lower extent along each trajectory segment, and test other vehicles at candidate altitude.

Field copying is serviced on the main thread with a default 2 ms budget. A single owned worker inflates the copied grid and performs Dijkstra without touching engine objects or the live hash map. Only complete results are published. Nearby compatible goals share up to eight cached fields, refreshed after about one second. Slot reuse clears unrelated old costs. The precise height controller handles final height adjustment between field layers.

Recovery permits two stationary escape attempts at 20-second intervals, then marks the third interval unreachable and brakes/holds. Missing terrain or physics does not consume attempts; vertical movement counts as progress. A new goal or explicit Retry resets the budget. A ten-minute target budget also bounds circling. Old stuck-recovery teleport code has been removed.

Site and firing probes use five spare ray slots without changing the core API layout. Probe results must match the submitted endpoints; load reset discards cached proof. Collision boxes, contacts, ground evidence, and fresh rays remain the common environmental inputs.

## Boundaries and verification

Manual driving, engine physics, seat animations, schedule generation, enemy selection, and ship flight retain their owners. These do not independently choose autonomous rover obstacle paths. Cover scoring describes mapped geometry, not material penetration or a guarantee against every enemy's shot. Physical probes remain the final site/weapon checks.

Offline validation: Release DLL build and 69 assertions across field, environment, recovery, trip/route, scanner-context, and lane-planner executables. The tests use synthetic maps and mocked probe results; they do not validate engine offsets, physical ray masks, animation timing, frame rate, or full gameplay. This build still needs the live cases in TESTING.md. Persistence and controller/settings work are outside this build.

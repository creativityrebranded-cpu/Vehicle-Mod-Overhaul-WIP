# Cleanup and restructuring proposal (merged from Profile Fork, Driving Behavior, Animation Fork) - 2026-10-18

Nothing here starts before the test round confirms what works. Each item names the owner of the file it touches.

## 1. Delete / park after the tests confirm (own files, in parallel)
| Owner | Files | What goes |
|---|---|---|
| Animation Fork | npcvehicles.cpp/.h, passenger.cpp, pilotquest.*, ESM | 9-step test (TestRun/TestTick/TestStart, kRunTestOnWalk), manual probes (Probe/RunProbe/ProbeKeys/kManualProbes, unused ApplyKind kinds), DIRECT `/` key, MountAsFaux, `=` control test, `\` Enter key, follower-command path (CommandUse/PathTo), old `,` force key if `;` replaces it, kAutoInstantSeat, gang leftovers; pilotquest Diagnose/Retarget/Tick. Keep: EnsureSeatKeyword, Travel quest (radius 0.5), Activate quest, instant seat after 6 s within 8 m, calm + ForceRun in combat, no-path give-up, vcontract hooks, exit service, BoardNpc. Seat-check trace and per-second state lines go behind a debug flag. |
| Profile Fork | vcontract, profiles, traffic, hoverui scanner code | traffic probe log lines and its own cell scan (keep DestinationOf), merge Hold and Return home, delete dead Mono* scanner code in hoverui. Keep vcontract and the 7 profiles. |
| Driving Behavior | navmap, sensing/avoidance in vehicleflight.cpp, vehicle_probe | old +-35 deg detour stepping / wall-following if the sampler is used far more than it falls back; thin-ray override in Corridor() if the hull sees all the ray sees; research scaffolding (28-value raw hit record per RayHit, NameBlocker, NoteCompare counters). Keep navmap, sampler, hull sweep, sweep ring, tests/navmap_test.cpp. |
| World fork | vehicleworld, realvehicles | its own list (see 3) |

## 2. Shared seams (one at a time, announced, one owner each)
1. **One goal type and one drive call**: `PilotFly(pilot, vehicle, const vcontract::Goal&)` with the nav settings inside. Removes PilotNav, the second Goal struct in vehicleflight, and the positional PilotFly arguments. Hook sites call one helper. (Animation Fork edits call sites, Driving Behavior the flight side.)
2. **One profile table**: Escort / Pursuit (npcvehicles kProfiles) move into vcontract; the player's Summon / Follow / Auto / Hold / Taxi become vcontract profiles too, so hook 2b, unassign-on-summon and the profile keys collapse into one assignment path (Profile Fork + flight owner, sequential, they share the hook sites).
3. **One loaded-refs snapshot** published once per frame (`world::Actors()` next to `world::Vehicles()`), replacing modscene::LoadedRefs(8000) scans in npcvehicles (several), passenger, traffic probe (5 s), realvehicles (2 s), pilotquest.
4. **One output adapter** for PilotCommand channels and DriveInput (desired heading, climb rate, speed). Later: a sensor scheduler that spends rays by need.
5. **One boarding / exit API** (BoardNpc / RequestExit) used by Fetch, Commuter, taxi, the E-press hook, hijacker; delete the parallel paths (SeatNpc, TakeSeat, Leave, SeatPlayerNow) once they call it.
6. **One key table** in controlmap (owner, context, default, free check), logged once at start and shown in one HUD help line (about 12 keys over 4 sessions: N U B `,` `.` `;` `/` Insert `=` `\` `[` `]` F7).
7. **Logging**: one verbosity setting; rate-limit or drop "Control map ... bindings read" (71 lines in one log), the long 2 s pilot position line, per-second NPC state lines, Traffic probe lines.

## 3. Build / install
- One integration owner does the single build and install per window; every other session compiles into its own folder (build-nav, build-traffic). One script: checks the game is closed, builds, copies, writes the board line, takes a lock file.

## 4. Order
1. Test round (list given to the user).
2. Dead code in each owner's own files, in parallel.
3. Shared reference snapshot (world fork), everyone switches.
4. PilotFly(Goal), then the single profile table (sequential: they share the hook sites).
5. Key table and logging setting.
6. Split vehicleflight.cpp (3600 lines) and npcvehicles.cpp (3100 lines) last, pure moves, one owner, own build, after one clean test. Proposed split of vehicleflight: flightstep (StepModel, boost), flightsense (probes, corridors, rays, navmap feeding), flightpilot (Fly, drive mode), flightplayer (PlayerTick), flightapi.

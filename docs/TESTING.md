# Testing

Set `Logs\SeamlessVehicle\seamless.ini` `[log] verbose=1` for the chatty lines, and copy `SeamlessVehicle.log` BEFORE relaunching (only one `.prev.log` is kept).
The log timestamps every line; a crash prints `CRASH?` with the stack and the plugin's breadcrumbs.

## Status as of this version (2026-10-18)

### Controls, overrides and profile classes (2026-10-20, later)

The Release DLL builds with the revised control scheme (every key one meaning: T tap go / hold ram or guard, T T clear, N and U tap / hold, the stance key, `]` autopilot modes), the straight-line ram, the Navigation / Combat / Weapons classes, and the empty own vehicle's cover combat. Eight offline executables pass **215 assertions**: navfield 15, scannercontext 12, triplogic 17, environment 14, recovery 12, routeplan 7, vehiclelogic 101 (Alt, T and N/U gestures, straight-line ram steering and ending, override rules, labels) and `profileclass_test` 37 (class isolation against the real `vcontract.cpp`, combat defaults, the weapons gate, row texts, cover and attack-spot rules). None of it has been tested in game.

Things to watch in a normal session (no test keys are added):
- `Scanner native pick provider: ready` (the actor pick), then `Target selection (hold)` and `Ram order: ... -> highlighted actor ...` after a T hold on someone. A refusal or end line gives the reason; the run should be straight with no detours.
- `Rover stance key: ...` at start-up, `NPC vehicles: stance -> ...` and `Profiles: vehicle ... combat -> attack/defend` when you press it.
- `Rover recall: N pressed` and `Rover cover: on/off`, plus `Rover guard: ... guards (x, y)` after T while covering.
- `Rover cover: enemy ...`, `lifting off to defend` and `weapon burst ...` when enemies come near while your empty vehicle covers you. `weapon refused (the player drives it)` must only appear if something tried to fire your driven vehicle.
- `Profiles: vehicle ... combat -> defend/attack` when a follower's fight starts, then `combat -> default` about 20 s after it ends.

### Shared-system update (2026-10-20)

The current Release DLL (0.63.0) builds successfully from `source/` alone. Nineteen offline executables run with every build (`build.ps1` stops on a failure): navigation and routes (`navfield_test`, `environment_test`, `recovery_test`, `triplogic_test`, `scannercontext_test`, `routeplan_test`), vehicle logic (`vehiclelogic_test`, `profileclass_test`, `placementlogic_test`, `travelmode_test`), combat (`manoeuvre_test`, `combat_steer_test`, `combat_bfm_test`, `combat_tactics_test`, `combat_brain_test`, `combat_ai_test`, `combat_relations_test`, `weapondamage_test`) and the native integration rules (`native_integration_test`). All pass in the latest run. The 0.61.0 assertions assertions include exact full body ownership/generation, chassis discovery beyond the first scene body, roll beyond 90 degrees, attached-camera pitch, marker consumption, outdoor-path eligibility, hull-dependent limits, precise projectile hull intersections, native boost timing, and explicit ram selection/termination.

These changes have **not been tested in game**. The older gameplay results below apply to older builds. Persistence is paused; no save continuity is added. See [the integration audit](SHARED_SYSTEMS_AUDIT.md) for ownership and limits.

Live cases still needed: Auto into/out of a U-shaped pocket; a rooftop marker; crossing a finite wall and passing under a thin deck; two opposing vehicles under an overhang; cover selection and firing with a wall or third actor between combatants; supply/hauler arrival; taxi/rescue boarding at a platform edge; both exit sides blocked; and a deliberately unreachable route showing Retry/Skip/Clear. Record frame time with several active vehicles while fields rebuild. Use a disposable test save; no live save was selected or modified for this update.

| Area | State |
|---|---|
| Hover, gears, summon (N), follow (U) | **Tested in game** (13:11 to 13:13 run, no crash) |
| Auto (T) to a map marker | **Tested**: 473 m flown, arrived 9 m from the marker |
| Forward rays, hull sweeps, heading sampler | **Tested**: ~3,700 sweeps and ~26,000 rays per 10 s on autopilot, no faults, hull sweeps see obstacles the rays miss |
| Fight -> NPC boards a free vehicle -> pursuit -> fires | **Tested once** (one NPC, one run) |
| Escort (follower trails you, parks, never fires) | Not tested |
| Passenger seat (`BoardNpc(asDriver=false)`) | Not tested. Risk: an actor that still holds the driver keyword may be seated as the driver |
| Calm exit (60 s calm and parked) | Not tested |
| 600 m leash | Not tested |
| HUD gang line and vehicles box, F7 editor entry "Vehicles" | Not seen in game after the redraw |
| View distance (`[view] objectFade`, default 3) | Not tested; watch the frame rate |
| Metered Left Shift boost | Offline meter depletion/recharge checks pass; native meter animation needs a game check |
| Lane planner, drive mode, flip recovery, registry | Tested in earlier versions, not re-run |

## In-game checklist

1. **Own vehicle:** N (summon), U (follow, then stand still: it parks), hover (double-tap Space), gears, T with a map marker, any of W A S D takes control back.
2. **Fight rule:** start a fight with an NPC that has a free vehicle (not yours) within 40 m. Expect log lines `Context: fight: NPC ... -> vehicle ...`, `aboard after`, `Profiles: vehicle ... -> pursuit`, then `weapon burst`.
3. **Pursuit:** it circles at about 30 m, 6 m above you, and fires only when its nose is on you. Check the damage per burst.
4. **Escort / passenger:** a follower (companion) near a fight. With a free vehicle: escort. With none but a driven one: `ally ... as passenger`, `rides vehicle ... as passenger`.
5. **Calm exit:** end the fight and stand still for a minute: `calm and parked for 60 s -> N get out`.
6. **Two fights at once:** two NPCs, two vehicles.
7. **No vehicle in range:** nothing happens, no errors. Fight next to only your own rover: nothing happens.
8. **Leash:** an NPC vehicle follows past 300 m from you; it stops at 600 m from where the NPC started (`stopped following (too far from its origin`).
9. **HUD:** the gang line and one vehicles box under it; F7, Page Up/Down to "Vehicles", arrows move it, F7 saves.
10. **Save / reload during a fight:** no crash, no vehicle left stuck.
11. **Parked small vehicles hopping:** stay near one; the log names any parked vehicle that moves by itself (`Vehicle hop:`).

## Switches for a bisect
`[context] enabled=0` (rules engine), `[nav] rays=0` (all engine ray and hull-sweep queries), `[nav] shape=0` (hull sweeps only), `[nav] sampler=0`
(heading sampler), `[view] objectFade=1` (the game's own fade), `[keys] debug=1` (manual test keys: board `;`, exit `Insert`, escort / pursuit `.`, NPC profile cycle `[`).

## Standalone test
`source/tests/navmap_test.cpp` checks the map and the heading sampler against a simulated world, no game needed (build line in `source/tests/NAVMAP_TEST.md`).

## Organic reports addressed in 0.61.0

The supplied log repeatedly refused a native dismount for NPC `0018B2B7` for several minutes, and repeatedly enabled/disabled a trail near its endpoint. Both loops are fixed in source. The same log contains first-chance exceptions attributed to CassiopeiaPapyrusExtender; this does not establish their cause or a crash in this plugin.

Live verification remains needed for first/third-person switching, hover beside a parked vehicle, recovering from a side impact, normal NPC exits, scanner selected-reference access, native boost-meter animation, and different modded hulls. These are organic gameplay checks: no Test/Test Cycle buttons or save-writing automation is installed.

# Known issues and next steps

## Open bugs and suspects
- **Friendly damage / NPC air ejection need live verification (2026-10-21):** allied mod-applied shots, ram damage and driver health share are blocked. A signature-checked native actor damage hook also blocks attributed allied damage (direct hits and explosions using that path); its startup success/failure and first 12 blocks are logged under `Friendly fire:`. Unattributed damage and self-damage remain native. Wrecked NPC occupants and the airborne exit key use a separate eject path, with wide/rolled-hull placement covered offline.
- **Enemy-vehicle damage needs retesting (reported 2026-10-21):** previous live builds did not reduce enemy-vehicle health. The latest build checks the previously omitted launch-to-first-sample segment and captures native projectile impact positions, with duplicate projectile/vehicle hits suppressed. Startup logs `native projectile impact hook installed`; confirmed contacts log `Vehicle hits: native impact ... vehicle ...`. The normal miss log stops after 15 samples; enable Detailed log for a full run. A live fix is not yet confirmed.
- **Boarding and attack aggression need live verification:** an NPC's pre-boarding enemy is restored even if StopCombat cleared current hostility, with a 60-second recovery window and retries independent of other fighters. Attacks on a vehicle provoke occupants, nearby owners and faction allies; neutral witnesses use assault/flee context. Shot provocation also runs when vehicle damage is disabled.
- **First-person climbing (reported 2026-10-21):** your vehicle climbed from about 40 m to 178 m in 20 s in first person, also while the view pointed slightly down. Not diagnosed yet: it needs a run with the detailed log on (the flight model's height and lift lines).
- **Vehicle names:** when the game gives no name the line says "Vehicle"; the log line `NPC vehicles: no name for ...` shows why (first 6).
- **0.63.0 needs live verification:** hover or drive (NPC vehicles and your own on autopilot), the enemy vehicle health bar and its driver targeting (log: `Vehicle hits: enemy bar ...`), the Settings menu category (log: `Hover UI: settings: ...`), and the fixes for vehicles stuck in towns and for getting into an NPC's vehicle.
- **0.62.0 needs live verification:** the combat AI and native integration, the parked-vehicle patch's in-place type change on live vehicles, the streamlined HUD (status card, hint hiding, J toggle), exit-while-hovering, and the fight-boarding rule. The patch's first run changed types as designed; shaking near Waggoner Farm came from an old duplicate and is now cleared.
- **Parked-vehicle patch limits:** Parked Vehicles' procedural (GPO) placements have no baked spot; without *Vehicles - Real* its props stay static. A save made during the 1-3 s of a type change keeps that vehicle disabled.
- **Frame spikes after loading.** The worst frames (29-144 ms) fell in the first minute after a start or a save load, once together with four parked-vehicle type changes; normal driving peaks at 6-10 ms (0.5-1.5 ms average, mostly navigation). Type changes are now spaced 500 ms apart and timed, and the minute line names each section's worst frame; the cause is not confirmed yet.
- **Enemy vehicle health bar:** the game lists only enemies in combat with you, so the bar under the driver's appears once the driver is fighting you; before that the status card shows the vehicle's health. The gap between the two bars is estimated from the game bar's height.
- **Current fixes need a game restart and live verification.** Exact chassis ownership, impact attitude recovery, first-person pitch, native prompt refresh, metered boost, the T gestures and the straight-line ram, N / U tap and hold, the stance key, the ] autopilot modes, the scanner pick provider (`Scanner native pick provider: ready/unavailable` in the log), N recall / U cover / guard points, the profile classes, the empty own vehicle's cover combat, and bounded native exits have offline coverage but are not live-verified. Missing body/path identity fails closed.
- **Hull sweep on the parked rover.** At autopilot start the sweep reports a hit about 3 to 5 m ahead with 0 hits filtered as "own body". Probably the rover's own neighbour or its own body;
  it clears once the rover moves. Not investigated.
- **Push-ups.** "pushed up 216 to 268 m/s: something under it the ground rays miss" appeared on autopilot and in manual flight. The existing handling keeps the vehicle flying.
- **Assist flip-flop.** In manual flight near large terrain the assist alternates "turning left" / "turning right" within a few hundred milliseconds, and sometimes stops ("no way forward") at 25 m/s.
- **Passenger seat.** An actor that already holds the driver keyword may be seated as the driver.
- **Sweep cost.** About 400 hull sweeps (about 0.7 ms each) and 2,800 rays a second while on autopilot, roughly a quarter of a core on the probe thread. Rate-limit if the frame rate suffers.

## Remaining limitations
- **Terrain assist and autopilot do not plan together.** They only react to each other (the autopilot casts corridors along its own heading plus detour; the assist climbs or turns from the corridor verdicts).
  Next step: both ask one predicted path (the heading the vehicle will really fly, with its yaw lag and detour) against `navmap`, get back a required altitude profile and the first blocked point, and commit to a choice for a while (hysteresis).
  Details in `ARCHITECTURE.md` ("Navigation and the terrain assist").
- **The game's own leash / home location.** The 600 m leash is measured from where the NPC stood when the job began, not from the NPC's editor placement.
- **The player's seat never fights by itself.** While you drive, combat stays yours: the vehicle never aims or fires on its own.
- **Cover faces the threat.** The approved Cover rule is opportunistic fire. Your empty vehicle still turns its nose on the threat once it holds its defend spot; NPC drivers under Cover never turn to aim.
- **A ram ignores traffic.** It flies straight and flat out by design: bystanders or vehicles in the line are not avoided; only the ground clearance is kept.
- **Weapons profiles beyond the stance.** The stance sets free fire or hold fire; opportunistic and never are only used by overrides and the player's seat.
- **Animated vehicle entry.** `kAnimatedEntry` is off; the quest-package walk works, the Activate-package animation only in some spots, so the instant seat is used.
- **Companion seats for the player's followers beyond the first passenger** are not tested.

## Practices that paid off
- Check a "working" sensor by its hits in the log, not by the absence of complaints (the forward ray faulted silently for three runs).
- Decompile the game's own use of an engine call before copying the pattern (the world lock, the embedded hknpWorld, the refcount).
- Keep one install per game run; copy the log before relaunching.

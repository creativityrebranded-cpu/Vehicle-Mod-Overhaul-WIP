# Flying Vehicle Overhaul 0.63.1

For Starfield 1.16.244 (Steam) and SFSE. Early work in progress.

- Driving is preferred unless terrain, combat, altitude or a hover profile calls for flight.
- NPC firing gates allow 25 degrees, widening to 40 degrees at close range, with launch re-aim.
- Projectiles lost before the first tracking frame get launch-line hit checks. Registered shots are capped at 20% of vehicle health.
- Fast shots also check the launch-to-first-sample segment. Native projectile impact events provide a second vehicle hit path, with duplicate damage suppressed.
- Chassis health and damage work regardless of occupancy: empty, parked and occupied ground vehicles use the same hit path. Only driver damage sharing and occupant ejection depend on a seated actor.
- Health displays use vehicle names capped at 14 characters; the text health bar is removed and the default HUD layout is updated.
- Wrecked vehicles eject the player and cannot be boarded until repaired above 25% health.
- Friendly-fire protection covers allied vehicle shots/rams, driver health share, and attributed allied native actor damage. The native hook logs installation and blocked damage under `Friendly fire:`.
- Wrecked NPC occupants can eject in flight. The NPC exit command also ejects while airborne, outside the actual hull at its current altitude.
- Combat resumes after boarding; changing the driver to escort no longer calms an occupant.
- Boarding restores each NPC's recorded enemy even when nearby NPCs are no longer fighting, with bounded retries rather than an unconditional hostile profile.
- Vehicle attacks provoke pilots/occupants and nearby owners/faction allies. The native crime system handles assault alarms; neutral witnesses can flee or cower. Shot provocation does not depend on vehicle health damage being enabled.
- Repairs pause during combat and recent attacks/firing, including after a pilot ejects. Damage records remain with the chassis across driver changes and no longer evict older damaged vehicles at a fixed record count.
- Every accepted shot/ram logs damage and remaining health. Physical ram detection is independent of the AI tactics switch; an empty ram victim can still provoke nearby owners and faction allies.
- Vehicle first-person look allows 80 degrees up/down while driving, restoring the original limits on exit (stock downward limit was 15 degrees).
- NPC air ejection waits briefly for native seat cleanup, then uses the native character-controller warp once to resume physics at the exit point. A delayed height log supports live verification of falling.

- Cover now uses the 25–40 degree firing cone, three-second bursts and 400 ms gaps. Valid targets stay selected; firing positions stay put for at least four seconds, with early changes for danger or a moving protectee. Clear-shot tests recognize the intended target chassis.
- Pursuers and attack-mode NPCs acquire targets to 700 m and retain them to 840 m. The pursuit fight zone is at least 700 m; weapon ranges still come from native weapon data.
- On foot in combat, tap T on a hostile actor/occupied vehicle to focus allied vehicle fire. Double-tap T clears the explicit focus and restores automatic selection. Hold-T commands remain available.
- Wrecks are excluded from summon relocation, pickup recovery and automatic upright placement. Hover release preserves wreck momentum. Forced ejection avoids native freeze-on-exit and runs the native on-foot furniture-state cleanup to clear seated pose/controller restrictions.
- Authoring mode records scanner T taps as native vehicle placement candidates. It writes exact worldspace identity, coordinates and heading to `placement-markers.tsv`, with matching log entries; estimated targets are refused and duplicate positions are ignored. The standalone source includes an exporter to the mod's own placement dataset.

## Validation and known issues

All 19 offline vehicle test suites pass. The standalone source ZIP is built and tested independently before publication.
The native friendly-fire hook's prologue matches the installed Starfield executable.

A live log confirms enemy vehicle damage and a wreck after 75 hits. It also showed automatic repair during an ongoing fight;
the combat repair pause, re-entry behavior, ejection controller synchronization and wider first-person look need a live retest.
Normal miss diagnostics stop after 15 samples; enable Detailed log when reproducing. Accepted damage always logs.

Friendly-fire protection, NPC air ejection and the latest aggression changes still need live verification. Unattributed damage and self-damage retain native behavior.
First-person climbing and occasional navigation stalls remain under investigation; see [known issues](KNOWN_ISSUES.md).

## Downloads

- `FlyingVehicleOverhaul-0.63.1.zip`: extract into the game's `Data` folder; includes `SeamlessPilots.esm` and `SFSE/Plugins/SeamlessVehicle.dll`.
- `FlyingVehicleOverhaul-0.63.1-source.zip`: standalone CMake/MSVC source with bundled SFSE interface header and all 19 test suites.
- `FlyingVehicleOverhaul-0.63.1-SHA256SUMS.txt`: checksums for both ZIPs.

The optional `Vehicles - Real.esm` patch remains in the repository's `Data` directory; the main ZIP does not require it.

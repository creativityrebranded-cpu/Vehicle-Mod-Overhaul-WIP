# Flying Vehicle Overhaul 0.63.1

For Starfield 1.16.244 (Steam) and SFSE. Early work in progress.

- Driving is preferred unless terrain, combat, altitude or a hover profile calls for flight.
- NPC firing gates allow 25 degrees, widening to 40 degrees at close range, with launch re-aim.
- Projectiles lost before the first tracking frame get launch-line hit checks. Registered shots are capped at 20% of vehicle health.
- Fast shots also check the launch-to-first-sample segment. Native projectile impact events provide a second vehicle hit path, with duplicate damage suppressed.
- Health displays use vehicle names capped at 14 characters; the text health bar is removed and the default HUD layout is updated.
- Wrecked vehicles eject the player and cannot be boarded until repaired above 25% health.
- Friendly-fire protection covers allied vehicle shots/rams, driver health share, and attributed allied native actor damage. The native hook logs installation and blocked damage under `Friendly fire:`.
- Wrecked NPC occupants can eject in flight. The NPC exit command also ejects while airborne, outside the actual hull at its current altitude.
- Combat resumes after boarding; changing the driver to escort no longer calms an occupant.
- Boarding restores each NPC's recorded enemy even when nearby NPCs are no longer fighting, with bounded retries rather than an unconditional hostile profile.
- Vehicle attacks provoke pilots/occupants and nearby owners/faction allies. The native crime system handles assault alarms; neutral witnesses can flee or cower. Shot provocation does not depend on vehicle health damage being enabled.

## Validation and known issues

All 19 offline vehicle test suites pass. The standalone source ZIP is built and tested independently before publication.
The native friendly-fire hook's prologue matches the installed Starfield executable.

Previous live builds failed to reduce enemy-vehicle health. Native impact capture and first-segment checks address gaps in that path,
but the latest changes still need a live retest. Normal miss diagnostics stop after 15 samples; enable Detailed log when reproducing.

Friendly-fire protection, NPC air ejection and the latest aggression changes still need live verification. Unattributed damage and self-damage retain native behavior.
First-person climbing and occasional navigation stalls remain under investigation; see [known issues](KNOWN_ISSUES.md).

## Downloads

- `FlyingVehicleOverhaul-0.63.1.zip`: extract into the game's `Data` folder; includes `SeamlessPilots.esm` and `SFSE/Plugins/SeamlessVehicle.dll`.
- `FlyingVehicleOverhaul-0.63.1-source.zip`: standalone CMake/MSVC source with bundled SFSE interface header and all 19 test suites.
- `FlyingVehicleOverhaul-0.63.1-SHA256SUMS.txt`: checksums for both ZIPs.

The optional `Vehicles - Real.esm` patch remains in the repository's `Data` directory; the main ZIP does not require it.

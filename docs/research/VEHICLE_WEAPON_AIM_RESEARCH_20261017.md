# Vehicle weapon aim — engine research (Starfield 1.16.244, 2026-10-17)

Static, read from the decompile (notes/decomp) and Starfield.exe. Nothing here is tested in game yet.

## Why NPC shots are not aimed today
Run 2026-10-17 19:56-19:58 (`notes/SeamlessVehicle-run-20261017-2004-contact-stops.log`, earlier part): every
"shot fired by FF018A8B (the vehicle)" line says **0 projectile(s) aimed**, and the first one logged
"projectile layout differs (direction length 0.00, speed 0.0)". `AimProjectiles` reads the CommonLibSF 1.14 offsets
+0x158 / +0x164, which are wrong for this build (and the vector may not be filled yet at the fire event).

## The fire event list is read correctly
`FUN_14131D2C0` (vehicle weapon-fired sink, recoil): event +0x10 = shooter ref, event +0x18 = `uint* list`,
`list[0]` = count, array of refs at list + 0x10. Each item is a reference (it calls `FUN_140B4D140(item, &matrix)`,
the generic ref → rotation matrix getter, and needs item+0x98). Same layout `AimProjectiles` uses.

## Projectile in 1.16.244
- RTTI type descriptor RVA 0x5AA7B50 → COL 0x1451E5A70 → **vtable 0x144CD0D98**. MissileProjectile ctor FUN_141B31E90.
- Base ctor `FUN_141B3A150`: two 12-byte vectors set to a default at **+0x148** and **+0x154** — very likely
  movementDirection / velocity (CommonLibSF's +0x158 / +0x164 shifted down by 0x10). Not verified by a read in game.
- Init from launch data `FUN_141B3A990(proj, launchData)`: the orientation comes from the launch angles
  (launchData +0x24, +0x28, +0x2C) through `FUN_142BCE350` (Euler → matrix) and `FUN_140B4E410` / `FUN_141B51090`.
  The direction / velocity vectors are not written here, so they are filled later (first update). That explains zeros
  at the fire event.

## Projectile::Launch = FUN_141B3BD50 (the place to aim)
It logs "Missile Projectile Launched %08x from (%x, %y, %z) angles x: %.2f, z: %.2f" with:
| Launch data offset | Meaning |
|---|---|
| +0x00 (3 floats) + 0x0C index | origin (Location; index into the planet offset table DAT_1461FAE88) |
| **+0x24** | angle X (pitch; Bethesda: nose down +) |
| +0x28 | angle Y (0 in every player shot) — printed as "z" by the game, but it is not the heading |
| **+0x2C** | **heading** (confirmed 20:36-20:39: equals the camera heading on the player's shots) |
| +0x30 | projectile base (BGSProjectile) |
| **+0x38** | shooter ref |
| +0xC8 / +0xD0 (`[0x32]`, `[0x34]`) | speed / range multipliers copied into the projectile |

Prologue `48 8B C4 48 89 58 18 48 89 48 08 55 56 57 41 54 ...`: clean 14-byte boundary (after `push rdi`), hookable.
Callers: 141B0DDA0, 141878660, 14175DB40, 140AA5280, 1405A2FA0, 1405A0520.

## Proposed approach (no projectile offsets needed)
1. Hook `FUN_141B3BD50`. While the PLAYER fires the rover weapon (shooter +0x38 = the player's vehicle), log the launch
   angles +0x24 / +0x28 / +0x2C and the origin next to the camera's pitch and heading. One test confirms which angle is
   heading and the sign of pitch (the user can aim freely in the vehicle, so the player's own shots are ground truth).
2. For a job's vehicle (shooter = job vehicle or its NPC): before calling the original, write +0x24 (pitch) / +0x2C (heading) from the
   origin to the led aim point (player chest + velocity × flight time). The game then builds the projectile pointing
   there, the same way it does for the player's aim.
3. Optional check: read +0x148 / +0x154 one frame after launch to confirm direction / velocity, for reference only.

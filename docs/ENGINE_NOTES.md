# Engine notes (Starfield 1.16.244)

Addresses are RVAs from `Starfield.exe` (image base 0x140000000), found in the decompile. `FUN_xxxxxxxxx` names are Ghidra's, for the same address.
Everything here was read from code or observed in a log; a note says when it is only inferred. Verify before relying on one.

## Actors and combat
- A reference is an actor when the form type byte at `ref + 0x2E` is `'K'`. (An RTTI-name test for "Actor" misses ordinary NPCs, which are class `Character`.)
- `IsInCombat` = actor vtable `+0xB58`. Combat target handle at actor `+0x298` (the invalid-handle constant is read from `base + kInvalidHandle`).
  Dead = vtable `+0x868` (argument 0). In every log so far the combat flag read 0 for NPCs that were not fighting; with a fight it was set.
- Start combat: `FUN_1418C10A0(combatManager *(base+0x05E71728), actor, target, 0)`. Stop combat: console handler `FUN_140C256C0(0, 0, actor)`.

## Seats and boarding
- Vehicle base seat table: count `+0x270`, mask `+0x264`, array `+0x278`, entries 0x28 bytes, the seat keyword at `+0x10`. Index 0 is the driver's seat.
  The seat check (`FUN_1412B2C40`) asks the ACTOR whether it carries the seat keyword; without it the check returns null and boarding is refused. Give it with
  `FUN_140B42710(ref, keyword)` (the core of AddKeyword).
- `FUN_141951FB0(actor, vehicle)` mount; `FUN_14063AF00` can-mount (seat check); `FUN_141995460(actor, vehicle, &link)` builds the mount interaction; `FUN_141993710(actor, vehicle, snap)`
  seats (snap 1 = instant). `FUN_141952530(actor)` dismount (refused when `actor + 0x378` bit 1 is set, no seat interaction, or "getting on/off" bit 3 set).
  `FUN_141A9C530(actor)` ends the interaction (instant exit). `FUN_14219BE30(vehicle)` seats the PLAYER (the game's only vehicle seat path; hooked so NPC
  jobs' vehicles are refused).
- The quest package route: `SeamlessPilots.esm`'s quest holds aliases; the plugin fills them (`ForceRefTo`) so the NPC runs a quest package (outranks its schedule).
  In combat the get-in package never runs, so the NPC is calmed first and set to force-run (`actor + 0xF8` bit 26, `FUN_140C3A040`).

## Physics queries (forward rays, hull sweeps)
- The bsWorld contains the hknpWorld EMBEDDED at `bsWorld + 0x20`: its address is `this` and the first qword is its vtable. (An early version loaded `*(bs+0x20)` and
  called through the vtable pointer, so every ray faulted and never hit anything.)
- **Take the world read lock around a direct query.** The game's own wrapper (`FUN_140583CB0`) calls `FUN_1422CC350(bsWorld + 0xB20)` before and decrements
  `*(bsWorld + 0xB24)` after (waking a waiter when the old count was 1 with waiters registered). Writers (the physics step) hold bits `0xFFC00000`.
- `FUN_1430CBEF0(bsWorld + 0x20, query, collector)` = castRay. `FUN_142362840(bsWorld, query, orientation, hits, startHits)` = castShape (the vehicle's own shape, `*(body + 0x78)`).
  The hit record is 0x70 bytes: position `+0`, **surface normal `+0x10` (a unit vector, confirmed on 80 to 100 % of hits per 10 s)**, body id `+0x48`, layer `+0x5C & 0x7F`, query type `+0x68` (1 = ray).
- **Do not resolve a hit's owner with `FUN_140B3CF40` + `FUN_140384600`.** It was run for the first time on the first ray hit; the game then crashed three times out of three
  (a deferred engine task, `FUN_1418CE700`, called through a destroyed object: execute address `0x400000002`). The refcount was not ours to release. Removed.
- World lookup for a point: `RayWorld(tes, at)` in `vehicle_probe.cpp` (cell at point, then `*(world + 0x50)` = bsWorld). Ground height ray: `kGroundRayCast` `0x00583B70`.

## Vehicle flight hooks
- Physics step `0x023CB260` (hooked: velocity applied inside the step so Havok still resolves collisions). Boost update `0x013211F0` (component `+0x238` = boost charge; the
  mod pins it to 1.0 for vehicles in use). Graph bools `VehicleIsBoosting` / `VehicleIsVertBoosting` via `kSetGraphBool` `0x00F0A370` on `ref + 0x60`.
- Ship-system update `0x02169100` is the per-frame hook the probes run in.
- The player's driven vehicle: `0x0219BB40(actor)`.

## Fade distance
- Fade category of a 3D object (`FUN_140583230`, from the BASE form type byte `+0x2E`): types `0x1d 0x22 0x23 0x26 0x28 0x30 0x31 0x35 0x36 0x3b 0x3d` = items (2), `0x32` (NPC_) = actors (3),
  `0x27` = 1 or 2, everything else = 1 (objects). Vehicles are furniture (type `0x2F`), so they use the objects multiplier.
- The per-category multiplier table is at `0x1458D5B70` (4 bytes each), rebuilt from the INI settings by `FUN_140573D40`:
  `fFadeOutMultObjects` (value at `0x145E86F70`) fills entries 0, 1, 4, 5, 8; `fFadeOutMultItems` entry 2; `fFadeOutMultActors` (value at `0x145E86F30`, times a per-player factor) entry 3.
  Fade distance is the object's distance DIVIDED by the multiplier, so it scales linearly with it; a multiplier of 0 or less switches fading off for that category (`FUN_14241A810` / `FUN_14241A5F0`).
  I found no upper clamp. Not tested in game.
- The three `fLODFadeOutActorMult...` settings: only their constructors were found; nothing that reads them. Left alone.

## Other
- Loaded references: `modscene::LoadedRefs(limit)` walks the attached grid cells (`FUN_...` for each reference in a cell). A prefix of a larger scan equals a smaller scan; 2000 was too few to
  reach an NPC far down the list, so scans that look for NPCs use 8000.
- Crash handler: `crash.cpp` prints the fault, registers, an unwound stack and the plugin's last 24 breadcrumbs (`crash::Crumb`).
- `Vehicles - Real.esm`: runtime ids of the light-master vehicles are `FE001033` (REV-8), `FE004001` (Moon Jumper), `FE005005` (Deimog); vehicle keyword `FE001017`.

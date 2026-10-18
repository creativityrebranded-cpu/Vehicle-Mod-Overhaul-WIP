# NPC vehicles, stage 1 research (board / leave / drive to point) — 2026-10-17

Static reading of the decompile (`scripts/engine_library.py show --source`). Not yet verified in game.

## Vehicles are actors
- A vehicle ref is an Actor (form type byte `+0x2E == 'K'`) with `actor+0x378` bit 1 set ("mountable").
- Rider and vehicle are linked by a RefrInteraction object (ctor `FUN_141A9C150`): rider handle `+0x10`, vehicle handle `+0x14`.
  Subclasses: `MountInteraction` (driver) and `PassengerInteraction` (seat, `+0x20` = seat record).
- The actor's current interaction lives in extra data **0xA5** on `actor+0xC8` (`FUN_1402C5C90(extra, 0xA5)` = "is mounted").
  The vehicle's own 0xA5 extra holds its occupied seats.
- On mount the rider's process (`actor+0x228`) high data (`+8`) gets the vehicle handle at **`+0x428`** (`FUN_141A08780`).

## Entry points (in order of usefulness)
| What | Function | Notes |
|---|---|---|
| Board, walking/animated | `FUN_141951FB0(actor, vehicle)` | Console `MountActor` (`FUN_140C58300`). High process: builds a temporary TESPackage type **0x29** (mount) targeting the vehicle and pushes it (`vtable 0xB38`), so the NPC walks over and gets in. Low process: snaps via `FUN_141993710(…, 1)`. |
| Board, instant | Papyrus `Actor.SnapIntoInteraction(vehicle)` → `FUN_141FCA990` → `FUN_141799E50(actor, vehicle)` | Needs 3D on both; for a mountable actor calls `FUN_141995460` then `vtable 0x9D8`. |
| Board core | `FUN_141993710(actor, vehicle, snap)` | Builds/starts interaction, sets process mount state (`FUN_141A03F70`), sends the mount ActionInput. |
| Interaction factory | `FUN_141995460(actor, vehicle, out)` | **Not the whole mount** (old lead was wrong): only creates Mount/PassengerInteraction (passenger if vehicle has keyword `DAT_145E89158`) and starts it (`FUN_141A9C350`). |
| Can board / free seat | `FUN_14063AF00(actor, vehicle)` | vehicle `vtable 0x200` ok, `FUN_14047FCE0(vehicle)`, a seat matching the actor's keywords (`FUN_1412B2C40`, fallback `FUN_1412B2A90`) not already taken in the vehicle's 0xA5 list. Use this as "is this vehicle free for this NPC". |
| Leave | `FUN_141952530(actor)` | Console `DismountActor` (`FUN_140C58480`). Precondition: `+0x378` bit 1 clear and 0xA5 present. Returns success. |
| Re-mount own vehicle | `FUN_141956A70(actor)` | Restores an actor into "their" vehicle (extra 0x74 handle, else linked ref keyword `DAT_145E89260`, else manager `DAT_145FD9B88`). Load/restore path, not the AI. |

## Driving
Vanilla has no NPC driver AI: NPCs only ride as passengers ("VehiclePassengerEnterVehicle"). Driving is ours.
Our autopilot (`vehicleflight.cpp`) is single-vehicle (`g_ownRover`, one `g_sh` state) and assumes nobody aboard
(line ~198). Stage 1 needs: per-vehicle autopilot state, and a check of what the game's vehicle step does with an
NPC in the driver seat (input source, whether it still wipes horizontal speed).

## Still open
- Which keyword `DAT_145E89158` is (decides driver vs passenger interaction) and whether REV-8 has it.
- Game vehicle step with an NPC driver (see above).
- Mount package 0x29 behaviour with the vehicle hovering / far away (likely needs it parked).
- From the earlier list: turret fire function, NPC combat target, vehicle health.

## Plan for stage 1 code
1. Board: `FUN_141951FB0(npc, vehicle)` after `FUN_14063AF00` passes; fall back to SnapIntoInteraction path.
2. Confirm aboard: npc 0xA5 present and process `+0x428` == vehicle handle.
3. Drive to point with a per-vehicle autopilot instance.
4. Leave: `FUN_141952530(npc)` once parked.

## Stage 1 built (2026-10-17, installed, not yet tested in game)
- `vehicleflight.cpp`: state is per pilot (`g_shs[4]`, `g_gs[4]`); pilot 0 = the player's (unchanged behaviour), 1..3 NPC.
  The physics step routes a vehicle to the NPC pilot whose `Shared::vehicle` matches (never the vehicle the player drives).
  Frame logic is `Fly(idx, vehicle, Goal, focused)`; the player's Tick builds its Goal (summon / follow) and calls `Fly(0, ...)`.
  NPC pilots have their own probe slots (`prefetch::kSlotPilot0`, 16 each). API: `PilotFly / PilotActive / PilotEndReason / PilotStop`.
- `npcvehicles.cpp`: test key (first free of L, H, G, I; logged). Press 1: nearest living NPC within 40 m boards the nearest
  free vehicle within 80 m of them (MountActor). Aboard: press 2 flies it to where you stand (stop 15 m). Press while boarding
  or flying cancels; after landing it waits 2.5 s and dismounts. Log prefix `NPC vehicles:` / `NPC rover 1:`.
- Open, to read in the first test log: does the vehicle step still wipe speed with an NPC aboard (`NPC rover 1: pos ... body matched`, speed),
  does the NPC stay seated while it flies, is vtable 0x868 really the dead check.

## Correction (same day): vehicles are furniture, not actors
- Every vehicle base (REV-8 FE001033, Deimog, Moon Jumper, T-47/V-47/S-47, the SW pack) is a **FURN** record (checked in the ESMs),
  in the data handler's form list type 0x2F alongside chairs and beds. The actor-mount reading above ('K' + 0x378 bit 1) is for
  actor mounts only; the console MountActor refuses vehicles.
- The game's vehicle test: `FUN_140EC72A0(ref)` = ref vtable +0x200 and `FUN_14047FCE0(ref, DAT_145F04028)` (HasKeyword, the
  vehicle keyword). Followers board with `FUN_14192A3F0`: vehicle test, `FUN_14063AF00`, then `FUN_141951FB0(actor, vehicle)`.
- `npcvehicles.cpp` now finds vehicles with `FUN_140EC72A0` (any vanilla or modded vehicle) and boards with `FUN_141951FB0` directly.
- `realvehicles.cpp`: pool = every form in that list whose keyword array holds the vehicle keyword (logged at start as
  `Real vehicles:   pool ...`); REV-8 spots pick one per spot. Spots already swapped in a save keep their vehicle.

## Auto chase, any NPC (2026-10-17, installed, untested)
User: no key, use any NPC for now; add the engine's own aggro check later. `npcvehicles.cpp`: every 2 s, a living NPC on foot 20-80 m
from the player with a free vehicle within 25 m boards (`FUN_141951FB0`); once aboard, pilot 1..3 follows the player
(`PilotFly(..., keepGoing=true)`, holds 20 m). Player on foot within 18 m for 3 s, over 300 m, or indoors: lands, NPC gets out,
30 s cooldown. Up to 3 at once. On/off key: first free of Insert, Home, End, ., ,, /, Delete (on at start).
Next: replace "any NPC" with the engine's combat target check.

## Test findings 2026-10-17 15:06-15:26 (logs in notes/SeamlessVehicle-run-20261017-*.log)
- Seats are keyword gated: VehicleDriverSeat FE001019 / VehicleCompanionSeat FE00101D / VehicleVascoSeat FE00101C; NPCs lack them.
  Giving the NPC the vehicle's seat-0 keyword (FUN_140B42710 = ObjectReference.AddKeyword core) makes FUN_14063AF00 pass. S-47 pack T-47 (06000808) has 0 seats.
- FUN_141951FB0 returns 1 but most NPCs never walk (1 of ~8 walked; one reached 2 m and still did not get in). Unsolved: copy how vanilla seats companions.
- FUN_141993710(npc, veh, 0) returned 1 on the seatless T-47 (15:24:53, 15:25:49); the user reported a vehicle appearing and camera/movement affected around then.
  Now gated on FUN_14063AF00; untested on a vehicle with a free seat.
- Rover fling: on a city platform the centre ground ray started under the floor (ground 243 m too low), the hover dived into the platform and Havok launched it
  (108-282 m/s up); the gravity estimate also drained to ~0 while resting on the floor. Fixed: side rays as floor, gravity only airborne, >12 m/s/step upward kick refused, vz clamp.
- Current keys: '.' auto on/off (off at start), ',' force nearest NPC into nearest vehicle.

## How the game seats an actor in a vehicle (read 2026-10-17 ~15:40)
- Activate: `FUN_140B47920(target, {activator,...})` -> `FUN_14192A3F0(actor, vehicle)`: vehicle test (ref vtable +0x200 and vehicle
  keyword), seat check `FUN_14063AF00`, then `FUN_141951FB0(actor, vehicle)`. The Activate package procedure `FUN_140DB13D0` does the
  same (seat check then `FUN_141951FB0`) for an AI actor whose package target is a vehicle.
- `FUN_141951FB0` (high process) builds a temporary TESPackage type 0x29 and pushes it (actor vtable +0xB38). The procedure dispatcher
  `FUN_1419DAC80` case 0x29 walks the actor to the target; in range `FUN_1419E5CF0` sets state (vtable +0x9B8) and seats with
  `FUN_141993710(actor, vehicle, 0)`. So walking is the AI's job, and a raw `FUN_141993710` call skips the state setup (it returned 0
  every time with a free seat, even 2.5 m from the vehicle).
- Missing step in the plugin: the pushed package only runs when the actor re-evaluates its AI. `FUN_141914750(actor, 0, 0)` = console
  EvaluatePackage / evp (`FUN_140C33A30`). Now called right after `FUN_141951FB0`.
- Seat choice is keyword based (VehicleDriverSeat / VehicleCompanionSeat / VehicleVascoSeat). The NPC gets VehicleDriverSeat, so it
  takes the driver seat like the player, in its own vehicle.
- Removed: direct board and moving the NPC beside the vehicle. The user reported an NPC clipped into the player's position with the
  player unable to move (as if seated); not proven which call did it, both bypass the game's sequence.

## NPC boarding pulls the player in (2026-10-17 15:47)
- In game: ordinary NPCs ignore the get-in package (their own routine wins), but after the user shot them (combat) one boarded,
  and the player was put into a vehicle as well; afterwards the player's "own vehicle" link pointed at another vehicle.
- `FUN_14219BE30(vehicle)` (virtual, vtable 1486CA880, no direct callers): seats the PLAYER (`DAT_145F43230`) in `vehicle` as a
  PassengerInteraction, starts it (`FUN_141A9C350`), `FUN_141A9C8C0(player)`, and stores the vehicle's form id in player data.
  This is the coupling. Hooked: logs every call with its caller; refused when the vehicle belongs to an NPC job.
- `DAT_145FAFE70` (called at the end of `FUN_141993710`) is the engine-wide action dispatcher, not vehicle specific.
- Timeout lines now log the vehicle the game says the NPC is in and whether it has a seat link (0xA5), since the user saw an NPC
  get in while the plugin's check said "did not board".

## Combat calls and the hostile coordinator (2026-10-17 ~16:05, built, untested)
- IsInCombat = actor vtable +0xB58 (condition `FUN_140BECC90`); combat target = handle at actor +0x298 (`FUN_140C399E0`).
- StopCombat = console handler `FUN_140C256C0(_, _, ref)`: combat controller (actor +0x250) +0x1E1 = 1, `FUN_140512700(actor+0xC8)`,
  alarm bit 11 of +0x378 cleared. StartCombat = `FUN_1418C10A0(*0x145E71728, actor, target, flag)` (Actor.StartCombat native
  `FUN_141FCAB00`, from its disassembly).
- Dismount `FUN_141952530` hooked (prologue 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E8): refused for a job NPC that is
  seated (Chasing / Settling) unless the plugin's Leave() calls it. Player-seat `FUN_14219BE30` hooked (refused for job vehicles).
- Coordinator: every 2 s; NPCs in combat whose target is the player; vehicle with a driver seat closer to the NPC than the player,
  within 60 m; pairs closest first, one per vehicle, up to 3. Aboard: Calm every 1 s while in combat, vehicle pursues 40 m off.
  Exit (player on foot < 20 m for 3 s, > 300 m, indoors): land, Leave, StartCombat(npc, player).

## Why NPCs could not sit: the vehicle seat path is player-only (2026-10-17 ~16:15, from the 15:57 log)
- `FUN_1419F7100(_, actor, furniture, _)` is the sit action: `FUN_1419F6BE0(actor, furniture, ...)`, the action dispatch, and on
  success, if the furniture is a vehicle, `FUN_14219BE30(vehicle)`, which seats the PLAYER (actor hard-coded `DAT_145F43230`).
  So an NPC that walked up and sat seated the player instead (the earlier "NPC clipped into me, could not move", and 15:47).
  Log 15:57:15: the hook blocked it (caller 1419F7750 = inside `FUN_1419F7100`) and the NPC (FD000EA0, 1 m away) stayed unseated.
- Fix: for a job vehicle the hook seats the job's NPC instead: `FUN_141995460(npc, vehicle, &link)` (builds and starts the
  Mount/Passenger interaction for that actor) + `FUN_141A9C8C0(npc)` (completes it, interaction vtable +0x58), link released.

## Combat outranks the get-in package (2026-10-17 16:25)
- Log: the hostile coordinator paired 3 NPCs fighting the player with vehicles 7 / 15 / 44 m away (get-in -> 1), but all kept
  fighting / following the player and timed out unseated. Combat AI runs instead of packages while in combat.
- Built: calm first (StopCombat), then the get-in package + EvaluatePackage; while boarding, every 0.5 s, if the NPC is back in
  combat: calm + evaluate again (logged with a count). Boarding failure: StartCombat on the player again.
- Cleaner later option (research): the package flag that lets a package run during combat, by finding where the combat AI checks
  the current package before taking over. No editor flag names exist in the exe (searched).

## Hover could only climb (2026-10-17 16:3x)
- Log: gravity estimate 13-15 m/s^2; rover climbed at 23-31 m/s with lift 0 (85 -> 348 m above ground). The dz-per-step learning
  counts the game vehicle step's vertical damping as gravity while rising: positive feedback. Same model for NPC pilots.
- Fix: gEst is now an integral of the vertical speed error (gain 1.2 /s^2, 0..30) added to the P term; no dz learning.
- Player keys: Shift + Space = descend (also Z); Shift + Space no longer counts as a Space double-tap.

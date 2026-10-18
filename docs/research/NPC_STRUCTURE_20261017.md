# NPC (Actor) structure in Starfield 1.16.244 — 2026-10-17

Static reading only: CommonLibSF headers (`ref/CommonLibSF`, written for 1.14.70) checked against our 1.16.244
decompile. **Verified** = seen used that way in the 1.16 decompile; otherwise it's the 1.14 layout and unconfirmed.

## Actor (size 0x578 in 1.14)
An Actor is a TESObjectREFR (refs: base object `+0x98`, parent cell `+0xB0`, extra data `+0xC8`, form flags `+0x20`,
form id `+0x28`, form type `+0x2E == 'K'`) plus:

| Offset | What | Status |
|---|---|---|
| `+0x0D0` MagicTarget, `+0x0E8` ActorState, `+0x0F8` movement state store | base classes | 1.14 |
| `+0x208` boolBits | bit 11 dead, 14 searching in combat, 16 EVP buffered, 17 reset AI, 26 player teammate, 30 guard, 31 paralysed | 1.14 |
| `+0x228` currentProcess (AIProcess*) | | **verified** (`param_1[0x45]` in `FUN_141951FB0`) |
| `+0x250` combatController | | **verified** (StopCombat writes `+0x250 → +0x1E1`) |
| `+0x260` actor value storage | | 1.14 |
| `+0x294` dialogue target, `+0x298` current combat target, `+0x29C` killer | handles | `+0x298` **verified** (GetCombatTarget) |
| `+0x358` race, `+0x360` perks | | 1.14 |
| `+0x378` boolFlags | bit 1 is-a-mount, **bit 3 getting on/off a mount**, 11 angry with player, 15 attack on sight, 16 commanded actor, 18 essential, 19 protected, 25 moving into loaded area, 27 movement blocked, 28 instant furniture pop-in | bits 1, 3, 11 **verified** |
| `+0x37C` boolFlags2 | | 1.14 |

Useful actor virtuals (1.14 numbering): IsInCombat `0x16C` (we use `+0xB58` = 0x16B×8, **verified**), StopCombat `0x16E`,
dead check `+0x868`, push package `+0xB38`, `+0xB30` (taken when the actor is already in an interaction), state set `+0x9B8`.
Non-virtual: EvaluatePackage = `FUN_141914750(actor, immediate, resetAI)` (**verified**, console `evp`).

## AIProcess (actor+0x228, 0xF8 bytes in 1.14)
| Offset | What |
|---|---|
| `+0x00` middleLow, `+0x08` middleHigh, `+0x10` high | process data blocks; high is null for low-process actors |
| `+0x18` currentPackage (ActorPackage, 0x30) | the package running: `+0x08` TESPackage*, `+0x18` target, `+0x1C` procedure index |
| middleHigh `+0x38` runOncePackage | a run-once package wins over currentPackage (`GetActorPackageRunning`) |
| `+0x48 / +0x4C / +0x50` | package timing stamps (`FUN_141951FB0` writes them) |
| `+0xC0` followTarget, `+0xC4` target | handles |
| **`+0xE7` process level** | 0 high, 1 middle-high, 2 middle-low, 3 low (**verified**) |
| `+0xEB` ignoringCombat | candidate for letting a package run in combat (unverified) |
| `+0xF1` escortingPlayer | |

## How the game puts an NPC in a vehicle: it depends on the process level
`FUN_141951FB0(actor, vehicle)`, the get-in call followers use:
1. Already in an interaction (ActorState vtable `+0x1A0` non-zero): actor vtable `+0xB30`, return true.
2. **Level 0, high process (near the player, fully simulated):** a reachability check `FUN_141A05D40(process, actor,
   vehicle)` (fails: `FUN_1419F99A0`, return false). Otherwise a temporary TESPackage type **0x29** (mount) targeting the
   vehicle, pushed with actor vtable `+0xB38(actor, pkg, 1, 1, 0)`, `actor+0x378 |= 8` (getting on), and the mount
   timeout `fMountDismountTimeout:Animation`. **The NPC then has to walk there under its AI**, so combat, a scene or
   a stronger package can override it.
3. **Level 1, 2 or 3 (middle-high, middle-low, low):** no walk. It snaps the NPC in at once:
   `FUN_141995460(actor, vehicle, &link)` (builds and starts the Mount/Passenger interaction) then
   `FUN_141993710(actor, vehicle, 1)` (seat, **snap = 1**). Returns whether the vehicle has keyword `DAT_145E89158`.

What this explains:
- "get-in → 1 but the NPC never moved": a high-process NPC got the walk package, and its AI (combat, its own
  sandbox package) never ran it.
- Our earlier direct seat `FUN_141993710(actor, vehicle, 0)` returned 0: it was the **walk** variant without the
  package state. The game's own instant path is `FUN_141995460` first, then `FUN_141993710(…, 1)`. That exact
  sequence was never tried.

## Leads (unverified)
- AIProcess `+0xEB` ignoringCombat for keeping the get-in package running during combat.
- runOncePackage (`middleHigh+0x38`): whether `+0xB38` with these arguments puts the mount package there.
- Seat keyword `DAT_145E89158`: passenger vs driver interaction.

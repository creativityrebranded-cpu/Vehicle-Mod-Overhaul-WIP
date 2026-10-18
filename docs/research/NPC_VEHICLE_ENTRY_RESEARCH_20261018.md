# How actors get into a vehicle (1.16.244) - research 2026-10-17/18

Everything below is from the decompile + Starfield.esm; "UNVERIFIED" marks guesses.

## The NPC walk-in (what refuses the REV-8)
- `FUN_14192A3F0` (actor uses ref) -> `FUN_14063AF00` CanMount (passes for the REV-8) -> `FUN_141951FB0(actor, vehicle)`.
- Process level 0: `FUN_141A05D40` seat check; on failure `FUN_1419F99A0` and return 0 ("no reachable seat"). On success it builds a package of
  type 0x29 (mount), target = the vehicle, pushes it through the actor's vtable +0xB38 and sets actor +0x378 bit 3.
- Probe results (00:11 and 00:5x, scripted test): refused from every side, from 2 m, level and landed, as a temporary teammate, with or without
  our quest package. The check never reads follower/teammate flags (checked all helpers).
- Package types 0x29 (mount) and 0x2A (dismount) exist ONLY at run time. Every package in Starfield.esm is type 0x12 or 0x13 (3,426 / 122).

## The package executor
- `FUN_1419DAC80` runs the current package: switch on a procedure id taken from a table. Id 0x11 (Activate) -> `FUN_1419B7C80` + `FUN_1419E5CF0`;
  id 0x29 (mount) -> `FUN_1419E8FE0`.
- `FUN_1419E5CF0` (Activate procedure on a REFR target): vehicle keyword check, occupancy check, builds the seat link, then calls the ANIMATED board
  routine `FUN_141993710(actor, vehicle, 0)` FIRST and only falls back to the gated `FUN_141951FB0` if that returns 0.

## The player's get-in
- `FUN_14192A3F0` player branch: target is a REFR and `FUN_141797320` (enterable) -> `FUN_141A66BA0(player, vehicle)`.
- `FUN_141A66BA0`: sets player process +0x510 = 9, deselects (`FUN_141A5E990`), calls `FUN_1419BFC20(process, actor, 0x2B, vehicle base, vehicle)`
  = performs default-object action #0x2B = **ActionActivate** (AACT 00013009) on the vehicle (generic: any actor), falls back to `FUN_141A66E40`
  if refused, and registers a completion sink. The sink ends in the player seat routine `FUN_14219BE30`.
- `FUN_14219BE30(vehicle)`: seats the GLOBAL player (`DAT_145F43230`): `PassengerInteraction` via `FUN_141A9C150`, `FUN_141A9C350`, finish
  `FUN_141A9C8C0(player)` (the same finish passenger.cpp uses for an NPC), driver mark `FUN_1421994B0`, UI/camera. No animation inside it.
- "Fake player" (pointing the player global at an NPC) is not an option: camera, input, save state.

## Animation actions
- Default object table = `FUN_1404F07A0()` + 0x38 + index*8, 364 (0x16C) entries, same order as the DOBJ record in Starfield.esm.
  0x2B = ActionActivate. 0x8A (used by `FUN_141993710`, +0x490) = ActionNonSupportContact in DOBJ order (UNVERIFIED that the order is exact);
  0x8B = ActionInteractionEnter, 0x8C = ActionInteractionExit(Quick 0x8E, Alt 0x8D).
- `FUN_141993710(actor, vehicle, flag)`: flag 1 = use the existing interaction, no CanMount, immediate; flag 0 = CanMount, builds the interaction,
  performs the action through the interaction's vfunc +0x98 (animated). Returned 1 on the REV-8 but nothing visible followed (probe 7).

## Open questions
- What FUN_141A66E40 (the fallback) does; what the completion sink waits for (callers of the seat routine: `FUN_1419F7100`, `FUN_141A23540`).
- Whether the NPC animation graph has the enter event for this vehicle.
- Why the animated board returns 1 yet no 0xA5 interaction / getting-on flag is seen.

## Data route built (SeamlessPilots.esm, scripts/make_pilots_esm.py)
- Quest 0x800: pilot aliases 0-5 with a Travel package (MQ201_StarbornPkg clone, location = vehicle alias, radius 3 m): WORKS, any NPC walks to the vehicle.
- Quest 0x810: pilot aliases with an Activate package (SQ_ActivateAlarm clone): untested result.
- Fallback that works: instant seat after the walk (no animation).

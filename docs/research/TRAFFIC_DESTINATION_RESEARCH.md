# Traffic: where an NPC wants to go (research, 2026-10-17)

Read-only decompile work for the Traffic session's step 1. Goal: `Destination(actor, x, y, z, kind)`.

## Answer: the actor's current path (the engine's own debug command shows the chain)

`GetDistanceFromActorsPath` (console handler **FUN_140C6FDA0**) prints "Distance from point on path to goal of path".
It calls **FUN_1419240E0(actor, location, out)** -> **FUN_1419228E0(actor)** -> **FUN_141A9E3B0(mc, location, out)**:

| Step | Code | Meaning |
|---|---|---|
| 1 | `FUN_1419228E0(actor)` | the actor's movement controller: `actor + 0x230` (or, when an extra-data link points to another actor, that actor's) |
| 2 | `q = *(*(mc + 8) + 0x240)` | the controller's path holder; refcount at `q + 0xA8` (inc / dec around use; dec to 0 -> `vtable[0](q, 1)`) |
| 3 | `iface = FUN_140688B60(q[9] /* +0x48 */, &DAT_145F9D108)` | query interface for the path follower |
| 4 | `iface->vtable[+0x10](iface, &path)` | returns the current **path** (smart pointer: int refcount at `path + 0`; release = dec, at 1 -> `FUN_14269DD00(path)` + `FUN_1422C7560(path, 0xD8)`) |
| 5 | `*(int*)(path + 0x90)` = point count, `*(path + 0x98)` = point array, **stride 0x58** (FUN_14269DEC0) | the path's points |
| 6 | point + 0x00: location `{ float x, y, z; uint32 space }` (+0x18: space object) | world position = xyz + `DAT_1461FAE88[space * 0xC]` (offset table, count `DAT_1461FAE80`) when `space < count`, else xyz as is |
| 7 | point + 0x38..0x40 tangent, + 0x48 tangent scale, + 0x4C / + 0x54 extra | (spline data) |

**Goal = the last point** (`count - 1`). No path (`count == 0`, or step 3/4 null) = not travelling right now.

FUN_14269FC90(path, from, to) gives the distance along the path between two point indices (`to < 0` = the end): the
remaining trip length.

## Kind: a real trip or idle

- Current package: `process = actor + 0x228`, `process + 0x18` = ActorPackage, `+0x08` = TESPackage*, package data at
  `TESPackage + 0x38` (`+0x04` package type, CommonLibSF 1.14 layout; unverified on 1.16).
- Practical rule for traffic: a path whose goal is **over ~150 m away and outdoors** is a trip; sandbox / idle paths are
  short. The package type is logged alongside to learn which types travel.

## Not used (dead ends)

- Package location / target data (`GetWithinPackageLocation`, `GetNumericPackageData` = FUN_140BFD510): resolved
  through a per-thread package context (TLS +0x218 / +0x250) that only exists during the AI's own package evaluation.
- Combat pathing goal policies: combat only.

## Verification plan (needs one game run)

`traffic.cpp` probe (read-only): every 5 s, for actors within 150 m, log
`Traffic: NPC id at (x, y) -> goal (x, y, z) d m, path n points, package type t`. Check: walking NPCs show a goal
where they end up; idle NPCs show no path.

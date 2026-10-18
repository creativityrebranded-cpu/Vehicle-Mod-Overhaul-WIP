# Vehicle shape cast — engine research (Starfield 1.16.244, 2026-10-17)

Read-only, from the decompile (notes/decomp) and Starfield.exe. Not tested in game. Goal (the user): "get the vehicle
size and the env size to be considered so it knows where it can and cant pass, the shape I mean" — sweep the vehicle's
own collision shape along the path instead of rays.

## Entry points
| Function | What |
|---|---|
| `FUN_142362840(bsWorld, query*, hkRotation* orientation, hitCollector*, startCollector* or 0)` | **hknpBSWorld::CastShape** (timer "TthknpBSWorld::CastShape"); calls `FUN_1430CBFB0(bsWorld + 0x20, ...)`. Sibling of the generic ray path FUN_142362760. |
| `FUN_1430CBFB0(world, query, orientation, collector, startCollector)` | **hknpWorld::castShape** ("TtWorldCastShape"); broadphase vtable +0x90 with world+0x510. Neighbour of castRay `FUN_1430CBEF0` (the one vehicle_probe.cpp uses). |
| `FUN_14239F580` / `FUN_14239F490` | hknpBSShapeQueryInterface::castShape / castRay (shape-local queries, not needed). |
| `FUN_1430500C0(hknpInplaceShapeBuffer<176>*, vec a, vec b, radius)` | builds a **capsule** in place (vtable `hknpInplaceShapeBuffer<176>`, +0x10 data, +0x18 0xB0); the shape is written at buffer+8. The vehicle exit check FUN_14219AE90 sweeps one. Only needed for a synthetic shape. |

## Use the vehicle's own shape (exact, any vehicle incl. mods)
FindBody (vehicleflight.cpp) already has the hknpBody: `read = vehicle+0x28`, `body = read vfunc +0x70(bodyId = vehicle+0x20)`.
- **shape = `*(body + 0x78)`** (FUN_1423791A0 = bhkNPCollisionObject::GetShape returns `*(FUN_142376B30(colObj) + 0x78)`,
  and FUN_142376B30 returns the body).
- body transform: rotation = body+0x00 (3 × 16-byte rows, hkRotation), translation = body+0x30 (already used).
- body filter fields the game copies into a cast "as this body": `u16 body+0x8A`, `u32 body+0x8C`, `u64 body+0x98`
  (projectile sweep FUN_141B40150). body+0x98 is also the body's user object (see "hit → object").

## hknpShapeCastQuery (0x80, 16-aligned), as FUN_14219AE90 / FUN_141B40150 build it
| Off | Value |
|---|---|
| +0x00 | `world(+0x20 object) vfunc +0x18()` (same as the ray query) |
| +0x08 | filter*: the world's own `vfunc +0x10()` (projectile path), or a hknpBSCustomCollisionFilter with a layer mask (vehicle_probe's) |
| +0x10 | u16: 0xFFFF, or body+0x8A to cast as the vehicle |
| +0x14 | u32: 0xFFFFFFFF, or body+0x8C |
| +0x18 | u64: 0, or body+0x98 |
| +0x20 | u16 1 |
| +0x22 | u8 0xFB |
| +0x28 | **shape*** (`*(body+0x78)`) |
| +0x30 | start point (x y z, w 0) = body+0x30 |
| +0x40 | displacement (to - from, w as in the ray query) |
| +0x50 | 1 / displacement per lane (0 → the 0x144F065B0 constant), as the ray query |
| +0x60 | float 0.001 (DAT_144F03E40, accuracy) |
| +0x64, +0x68 | float 0, 0 |
| +0x6C | u32 0x100 |
| +0x70 | u64 0 |
| +0x78 | float FLT_MAX (DAT_144F05124) |
Orientation argument: the vehicle body's rotation (body+0x00); the game passes identity `DAT_14587A7E0` for symmetric
shapes (capsule, sphere).

## Hits (0x70 each, same collector as the ray: hknpAllHitsCollector with an external array)
- +0x00 position, +0x68 query type (**2 = SHAPE_CAST**; 1 ray, 3/4 closest points / point).
- **+0x48 hit body id** (u32). **+0x5C & 0x7F = the hit body's collision layer** (the vehicle check treats 0x0D and 0x11 specially).
- Fraction: not read here; likely near +0x20 (unverified) — or compute from the hit position.

## Hit body id → game object (FUN_14219AE90 after the cast)
1. `w = world +0x20 object`; `w vfunc +0x58(bodyId)` → non-zero if valid.
2. `body = w vfunc +0x70(bodyId)`; `obj = *(body+0x98)`; `node = obj vfunc +0x138()`; need `*(node+0x10)`.
3. `ref = FUN_140B3CF40(*(node+0x10), 1)` → TESObjectREFR*, **ref-counted** (+0x80000200001): release with FUN_140384600.
   No ref (terrain, static collision): fall back on the layer at hit +0x5C.

## Vehicle sizes (FURN OBND, for reference only)
REV-8 2.5 × 4.6 × 2.0 m, Moon Jumper 4.0 × 5.5 × 3.3, Deimog 2.7 × 6.2 × 3.4, T-47 6.2 × 6.9 × 1.8; mod OBNDs can be
bogus (V-47 3.3 × 0.6 × 3.3) — the body shape above avoids them.

## Ref -> hknp body id (2026-10-17 21:1x, partial)
From the game's own scene-graph visitors (FUN_140F44DD0, FUN_140F57900, FUN_140D5E870, FUN_1419692A0: functor(ctx, NiAVObject*)):
- **NiAVObject +0x110 = collision object**; `colObj vfunc +0x138()` non-zero = it is a bhkNPCollisionObject.
- **body id: `FUN_142376C70(colObj, uint32* out)`** (0xFFFFFF = none): `system = colObj+0x20`, index `colObj+0x28` into
  the system's hknpBodyId array (`*(system+0x20)`, count at +0x38).
- `FUN_142376B30(colObj)` = the hknpBody*; **body +0x8C & 0x7F = collision layer** (visitor tests == 2);
  `FUN_142376F60(colObj, out)` = filter info (low 7 bits = layer, 0x1E tested); `FUN_1423791A0(colObj)` = shape (body+0x78).
- **Ref -> 3D root: ref +0xB8 loaded data, +0x08 root** (already used in game by actors.cpp, shipmodel.cpp, recentre.cpp).
  NiAVObject: world rotation +0x80 (3 rows x 4 floats), **world bound sphere +0x100 (radius +0x0C)**, collision object
  +0x110, NiNode children +0x138 / count (u16) +0x142, vtable +0x20 "as node" (shipmodel.cpp). So ref -> body id =
  walk the 3D tree for node+0x110 with vfunc +0x138 non-null, then FUN_142376C70. Implemented in `vehicleworld.cpp`
  (unwired; compiles 21:2x).
- Open: live AABB (body +0x40 looks like hkAabb16 in broadphase integer space; or the shape's calcAabb — the null
  function table `hknpShapeNullFunctions::calcAabb` = FUN_1430B7890 was not found as a raw vtable pointer).

## Used (2026-10-18, Driving Behavior session) — untested in game
`vehicle_probe.cpp CastShapeNow` calls FUN_142362840 with the vehicle's own shape (*(body + 0x78)) and rotation (body + 0), the query laid out exactly as the
vehicle-exit check FUN_14219AE90 builds it (re-read 2026-10-18: +0x28 shape, +0x30 start, +0x40 displacement, +0x50 inverse, +0x60 float 0.001 (DAT_144F03E40),
+0x6C 0x100, +0x78 FLT_MAX; the game passes TWO collectors, a hit collector and a start-point collector, so we do too). Hits: type at +0x68 must be 2, position at +0,
body id +0x48, layer +0x5C & 0x7F; the distance is the hit point projected on the sweep (the fraction field is still not identified). The body is reached from the physics
instance (hknpBSVehicleInstance +0x28 read interface, +0x20 body id, vfunc +0x70), the same way FindBody does it.

## CORRECTION (2026-10-18 12:0x): the hknpWorld is EMBEDDED at bsWorld + 0x20
`this` for the world calls is `bs + 0x20` (an address); the qword stored there is the object's VTABLE. The game says so in both callers:
FUN_14219AE90 `plVar16 = (longlong*)(lVar23 + 0x20); (**(code**)(*plVar16 + 0x18))(plVar16)` and FUN_140583CB0
`(**(code**)(*(longlong*)(*(longlong*)(param_4 + 0x50) + 0x20) + 0x18))()`. vehicle_probe.cpp loaded `*(bs + 0x20)` and used the vtable pointer as the object,
so the first query called garbage: EVERY forward ray (and the hull sweep built on the same code) faulted on first use in all three runs (20:58, 11:38, 11:55) and
no forward ray ever returned a hit before this fix. Fixed in CastRayNow / CastShapeNow (`inner = bs + 0x20`, vtable and slot 3 checked to lie in the exe image first).

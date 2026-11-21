# Authored roads and mixed driving/flight routes

This update uses the existing Traffic Manager selection, Change buttons, trip callbacks, wheel controls, hover controller, driver handling and engine hooks. The new behavior connects route metadata, lane execution, road rules and connected itineraries.

## Authoring

Under a selected route's **Flow** group, **Travel → Change** cycles Drive only, Hover only and Both (prefer drive). **Lane width** sets the usable width of each lane, independently of the spacing between the two lane centrelines. New editor routes default to Both. Files without travel metadata retain their previous hover default.

Use the existing Ctrl+click multipoint selection to select adjacent points. **Selected stretches → Change** changes only stretches whose two endpoints are selected. Unselected gaps retain their rules. The point panel can also edit its next stretch. Overrides can inherit the route rule. Undo, redo, save, copying, reversing, splitting and merging carry the section rules with the geometry.

Road controls belong at stretch ends: uncontrolled, yield, stop or signal. Stretch speed and priority are editable using the same steppers. Signals default to a 60-second period with 30 seconds green; the phase button advances by 15 seconds. Their simulation clock pauses with covered gameplay and is recorded with traffic journeys. Place authored points at controlled road junctions. Use existing junction links for permitted turns; a geometric crossing alone does not authorise a turn. Crossings at different elevations do not form a road intersection.

Initial point placement still raises points onto detected surfaces. Subsequent deliberate edits through structures remain allowed with red warnings. A warning permits authoring; it does not disable physical vehicle collision.

## Execution

- Player Auto, Cruise, NPC route traffic and scripted journeys can receive the same sampled lane guidance. Existing API 13, read-only API 4, `FvoTripGoal`, `FvoTripClaim` and `NavParams` layouts remain unchanged.
- The lane follower replaces the general heading fan while a guided route is active. It tracks route progress, limits pursuit lookahead, brakes for curvature and uses measured vehicle width.
- Static-obstacle alternatives stay inside the assigned lane. Vehicles ahead form queues. Neighbor prediction also considers relative movement. A drive-only rule cannot escape into flight.
- Both chooses wheels on supported, sufficiently low routes. A vertical bypass requires measured ascent and elevated corridor clearance, respects ceiling limits, holds lateral movement during ascent, and retains an absolute bypass height. Stable clear supporting road permits landing and returning to wheels.
- Support queries distinguish a roof above the road from the surface below the lane. Existing collision queries are reused with a shared budget of 12 additional support rays per frame and a bounded cache. Corridor sensing and junction arbitration are throttled; steering and braking still consume current vehicle motion.
- Junctions use conservative whole-conflict-zone reservations, priority, waiting age, stop/yield/signal eligibility and exit occupancy. A vehicle already inside retains the ability to clear the junction. Physical sensing can still stop an authorised movement.
- Unknown immediate geometry causes a hold until measured; it is not treated as permission to drive through a wall. An impossible narrow route or blocked drive-only route remains blocked rather than changing its authored geometry.

## Additive file fields

```ini
[route.example]
type = path
points = 0 0 2, 100 0 2, 100 100 30
lanes = 1
travel = both
corridor = 7
sections = drive,12,0,2,1,60,30,0 | hover,20,0,0,1,60,30,0 | inherit,0,0,0,1,60,30,0
```

`corridor` is the usable width in metres of each lane. Existing `width` retains its original meaning: distance between opposite lane centrelines. One-way routes follow the authored centreline.

`sections` has one entry per authored point; the last entry of an open path is reserved for editing continuity. Each entry describes its outgoing stretch in source point order. The opposite lane uses the same physical stretch in reverse. Entries are separated by `|` because INI treats `;` as a comment.

Entry fields, in order:

1. Travel: `inherit`, `both`, `drive`, or `hover`.
2. Speed in m/s; zero inherits the route speed.
3. Priority, -100 through 100; higher receives preference.
4. Control: 0 uncontrolled, 1 yield, 2 stop, 3 signal.
5. Stop dwell in seconds, default 1.
6. Signal period in seconds, default 60.
7. Green duration in seconds, default 30.
8. Signal phase offset in seconds.

Rings support whole-route travel rules. Per-stretch overrides apply to authored point paths.

## Script integration

`routeguidance.h` declares an optional versioned lane-provider capability and script request/status structures. New core/addon pairs negotiate it through `FVO_RegisterLaneProviderV1`; older pairs retain their original interfaces and fallback behavior. Older cores cannot enforce newly added road rules. No existing export is removed or made mandatory.

The addon exports:

```cpp
int FVO_Traffic_PlanJourneyV1(
    const FvoJourneyRequestV1* request, FvoTripClaim* claim);
int FVO_Traffic_JourneyStatusV1(
    void* claimUser, FvoJourneyStatusV1* status);
```

Call from the game's main thread. Resolve exports optionally. Set the request's vehicle identity and current position, then provide destination stops and dwell times. Each stop may prescribe an ordered list of route identities; an empty list requests connected route planning. Every leg is validated before a claim is committed. The addon copies stop/route data, so caller buffers need not remain alive. Consume the returned claim through the existing trip mechanism. An invalid replacement request leaves the current journey intact; a valid replacement reuses the vehicle's claim.

Journeys preserve their active geometry and section rules until safe trip/route transitions. Marker journeys use direction-aware connected itineraries rather than taking an early off-road shortcut. Existing ambient destination selection, population admission, driver ownership and observation safeguards are retained.

## Save compatibility and verification

`journeys.state` lives in the editable working route folder and is captured with its game save's immutable traffic snapshot. The optional save-preparation callback records active claims before cloning. Restoring a claim uses the existing vehicle identity and checks its recorded base model when available; it does not create or relocate an actor. Missing journey records use legacy admission and do not invent historical traffic state. Invalid or unsupported recorded state is retained on disk and prevents publishing a replacement traffic snapshot; the existing save-error notification reports failure.

The new record stores route/lane progress, section policy, connected itinerary, script stops, dwell state and turn geometry. Loading a different or identical game save still branches a new working copy. Existing route/profile formats, ESM identities, old saved profiles and supported C interfaces remain readable.

Offline tests cover decisions, serialization, mixed DLL loading and a finite-turning vehicle replay. They do not establish actual Starfield steering, live probe correctness, driver continuity, streaming behavior or frame-time acceptance. The traffic continuity mandate remains an in-game acceptance requirement. This update does not replace the existing native lifecycle or introduce off-screen vehicle relocation.

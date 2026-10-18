# Testing

Set `Logs\SeamlessVehicle\seamless.ini` `[log] verbose=1` for the chatty lines, and copy `SeamlessVehicle.log` BEFORE relaunching (only one `.prev.log` is kept).
The log timestamps every line; a crash prints `CRASH?` with the stack and the plugin's breadcrumbs.

## Status as of this version (2026-10-18)

| Area | State |
|---|---|
| Hover, gears, summon (N), follow (U) | **Tested in game** (13:11 to 13:13 run, no crash) |
| Auto (T) to a map marker | **Tested**: 473 m flown, arrived 9 m from the marker |
| Forward rays, hull sweeps, heading sampler | **Tested**: ~3,700 sweeps and ~26,000 rays per 10 s on autopilot, no faults, hull sweeps see obstacles the rays miss |
| Fight -> NPC boards a free vehicle -> pursuit -> fires | **Tested once** (one NPC, one run) |
| Escort (follower trails you, parks, never fires) | Not tested |
| Passenger seat (`BoardNpc(asDriver=false)`) | Not tested. Risk: an actor that still holds the driver keyword may be seated as the driver |
| Calm exit (60 s calm and parked) | Not tested |
| 600 m leash | Not tested |
| HUD gang line and vehicles box, F7 editor entry "Vehicles" | Not seen in game after the redraw |
| View distance (`[view] objectFade`, default 3) | Not tested; watch the frame rate |
| Unlimited boost only for vehicles in use | Not tested; may or may not stop hopping parked vehicles |
| Lane planner, drive mode, flip recovery, registry | Tested in earlier versions, not re-run |

## In-game checklist

1. **Own vehicle:** N (summon), U (follow, then stand still: it parks), hover (double-tap Space), gears, T with a map marker, any of W A S D takes control back.
2. **Fight rule:** start a fight with an NPC that has a free vehicle (not yours) within 40 m. Expect log lines `Context: fight: NPC ... -> vehicle ...`, `aboard after`, `Profiles: vehicle ... -> pursuit`, then `weapon burst`.
3. **Pursuit:** it circles at about 30 m, 6 m above you, and fires only when its nose is on you. Check the damage per burst.
4. **Escort / passenger:** a follower (companion) near a fight. With a free vehicle: escort. With none but a driven one: `ally ... as passenger`, `rides vehicle ... as passenger`.
5. **Calm exit:** end the fight and stand still for a minute: `calm and parked for 60 s -> N get out`.
6. **Two fights at once:** two NPCs, two vehicles.
7. **No vehicle in range:** nothing happens, no errors. Fight next to only your own rover: nothing happens.
8. **Leash:** an NPC vehicle follows past 300 m from you; it stops at 600 m from where the NPC started (`stopped following (too far from its origin`).
9. **HUD:** the gang line and one vehicles box under it; F7, Page Up/Down to "Vehicles", arrows move it, F7 saves.
10. **Save / reload during a fight:** no crash, no vehicle left stuck.
11. **Parked small vehicles hopping:** stay near one; the log names any parked vehicle that moves by itself (`Vehicle hop:`).

## Switches for a bisect
`[context] enabled=0` (rules engine), `[nav] rays=0` (all engine ray and hull-sweep queries), `[nav] shape=0` (hull sweeps only), `[nav] sampler=0`
(heading sampler), `[view] objectFade=1` (the game's own fade), `[keys] debug=1` (manual test keys: board `;`, exit `Insert`, escort / pursuit `.`, NPC profile cycle `[`).

## Standalone test
`source/tests/navmap_test.cpp` checks the map and the heading sampler against a simulated world, no game needed (build line in `source/tests/NAVMAP_TEST.md`).

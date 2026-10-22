# Flying Vehicle Overhaul 0.65.0: source

The source of Flying Vehicle Overhaul for Starfield (SFSE plugin `SeamlessVehicle.dll`). It builds on its own: nothing else
from the author's project is needed, and the optional SeamlessPlanets core, Carfield and Parked Vehicles are not required.

This version includes driving as the default travel mode, a wider firing gate, launch-line hit checks, shorter vehicle names,
friendly-fire protection for vehicle and attributed native actor damage, NPC air ejection, native projectile-impact health checks,
and contextual retaliation against vehicle attacks. Launch-to-first-sample segments are checked, and each boarding NPC retains
its own enemy independently of nearby combat. A live log confirms damage and wrecking; the follow-up fixes below need live verification.
Vehicle health belongs to the chassis and does not require a pilot: empty, parked and occupied vehicles use the same damage path.
Driver health sharing and occupant ejection apply only when someone is aboard; allied ownership protection still applies.
Repairs pause during combat and recent attacks/firing, including after the pilot ejects. Chassis damage records survive driver changes
and are no longer evicted by a fixed record limit. Accepted shots and rams always log damage and remaining health; physical ram detection
does not depend on AI tactics. Vehicle first-person pitch extends to 80 degrees in either direction while driving and restores on exit.
Air ejection waits for native seat cleanup and synchronizes the character controller once, then leaves falling to native physics.

## Build
- Windows, Visual Studio 2022 Build Tools (MSVC, C++20) and CMake 3.24 or newer.
- `powershell -ExecutionPolicy Bypass -File build.ps1` builds `build/Release/SeamlessVehicle.dll` and runs all 19 offline test suites.
  Or: `cmake -S . -B build -A x64` then `cmake --build build --config Release`.

The plugin targets Starfield 1.16.244 (Steam) only: engine addresses are fixed for that build and the plugin switches itself
off on any other.

## Layout
- `src/`: the plugin. `main_vehicle.cpp` is the entry point (SFSE load, the per-frame task).
- `tests/`: offline checks of the pure logic (no game needed).
- `third_party/sfse/sfse/PluginAPI.h`: the SFSE plugin interface (MIT; notice in `third_party/sfse/LICENSE.txt`). Nothing else
  from SFSE is used.
- `tools/bake_placement_spots.py`: regenerates `src/placement_spots.h` from installed copies of the optional placement mods
  (Carfield, Parked Vehicles). It also bakes the grid of every cell of their worldspaces, which the patch uses to find a city
  laid over its planet. The baked header is included, so building needs neither mod.
- `tools/bake_own_spots.py` and `placements/*.tsv`: the mod's own placements (scanner T recordings), baked into
  `src/own_spots.h` (included).

## Placement recording

For an authoring session, create `Data/SFSE/Plugins/SeamlessVehicle.placement-recording` (an empty file).
On foot, open the scanner, aim at the intended parking surface and tap T. The scanner displays VEHICLE PLACEMENTS.
A successful tap records the physical surface coordinates and heading, places the native map marker, and does not start a trip.
Recording always uses the exact surface hit. Since 0.64.0, normal scanner destinations do too, wherever a real surface is hit.
The append-only recording is `Documents/My Games/Starfield/SFSE/Logs/SeamlessVehicle/placement-markers.tsv`.
Estimated surfaces are refused; repeat taps at the same spot are ignored. Remove the flag to restore normal scanner controls.
The authoring flag is not included in the release ZIP.

To make recordings into placements, copy `placement-markers.tsv` into `placements/`, run `python tools/bake_own_spots.py`
and rebuild. Taps under 2 m apart count as one re-tap; markers within 9 m form a row with one vehicle type and one heading.
Positions are kept in the planet frame the game's references use (the recorded `world_file` reads UNRESOLVED for a generated
planet worldspace; the spot is tied to the recorded cell instead). In game a spot spawns its vehicle when its cell attaches and
remembers it in `placement-spawned.tsv`.
`tools/export_vehicle_placements.py` still exports the raw candidates as JSON.

## Current combat and ejection follow-up

Cover uses a 25–40 degree firing cone, 3 s bursts/0.4 s gaps, stable targets and a minimum 4 s position commitment.
Chase acquisition is 700 m with 840 m retention; native weapon ranges remain independent.
On foot in combat, tap T at an enemy to focus allied vehicle fire; double-tap T returns to automatic targeting.
Wreck recovery relocation is blocked, release preserves momentum, and ejection runs native on-foot furniture-state cleanup.
These changes pass offline checks and need live verification.

# Flying Vehicle Overhaul 0.63.1: source

The source of Flying Vehicle Overhaul for Starfield (SFSE plugin `SeamlessVehicle.dll`). It builds on its own: nothing else
from the author's project is needed, and the optional SeamlessPlanets core, Carfield and Parked Vehicles are not required.

This version includes driving as the default travel mode, a wider firing gate, launch-line hit checks, shorter vehicle names,
friendly-fire protection for vehicle and attributed native actor damage, NPC air ejection, native projectile-impact health checks,
and contextual retaliation against vehicle attacks. Launch-to-first-sample segments are checked, and each boarding NPC retains
its own enemy independently of nearby combat. These latest fixes need live verification; previous builds failed to reduce enemy-vehicle health.

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
  (Carfield, Parked Vehicles). The baked header is included, so building needs neither mod.

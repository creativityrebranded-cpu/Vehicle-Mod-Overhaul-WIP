# Flying Vehicle Overhaul 0.64.0

For Starfield 1.16.244 (Steam) and SFSE. Early work in progress.

- **Own vehicle placements.** The mod now parks its own vehicles, independent of Carfield and Parked Vehicles. The first set covers 36 spots in New Atlantis: the spaceport and landing tarmac, the Embassy District, the Varuun embassy and Sanctum Universum fronts, and the Commercial District entrance. They were recorded in game with scanner T.
  - Spots placed side by side form a row. Each row gets one vehicle type, and the vehicles in a row are parked in parallel: square to the row when that was the intent, otherwise nose-to-tail.
  - A vehicle appears when its area loads, never within 20 m of the player. The vehicles are unowned, so taking one is not theft.
  - Each spot remembers the vehicle it placed (`placement-spawned.tsv`). While that vehicle exists anywhere, the spot stays empty, so vehicles don't pile up. A spot only gets a new vehicle once its old one is gone.
  - Shares the placement switch in the game's Settings menu (`[placement] enabled`).
- **Parked Vehicles patch works in cities.** Cities are laid over their planet when you play, shifted by whole cells. New Atlantis, for example, moves 7 cells west and 17 north. The patch was looking for the city's own worldspace, so none of Parked Vehicles' 208 city spots ever matched. Now each loaded cell's real grid is compared with its grid in the plugin, and spots are matched with that shift. City-parked vehicles get the patch's per-location vehicle mix for the first time.
- **Exact scanner destinations.** Where your aim hits a real surface (within the ~250 m of loaded collision), that exact point is the destination. The 40 m pull to building centres is gone. Map points only take over when the aim hits nothing, or for a marker selected in the game's scanner.
- **Focus fire says when it ends.** A focus set with T already dropped by itself when the target died, disappeared, stopped being hostile or went past 840 m. It now shows a notice when that happens, such as "Focus target down | automatic targeting", plus a log line.
- **Enemies mount up from further away.** Fighting NPCs now look for a vehicle up to 250 m from the player (was 80 m). With no vehicle within 40 m, they run to one within 150 m when the player is 60 m or more away or driving, as well as when an NPC driver is already fighting. Unowned vehicles out in the open are free to take. In an owned place (a settlement, town or camp), only that place's own people take them, and owned vehicles still only go to their owner's faction.
- **Placement recording** (authoring only): turned on by the `SeamlessVehicle.placement-recording` flag file, now described in the source README. Recordings are baked with `tools/bake_own_spots.py` into `src/own_spots.h`. Taps under 2 m apart count as one re-tap.

## Validation and known issues

All offline vehicle test suites pass. The standalone source ZIP is built and tested independently.
None of the 0.64.0 changes have been tested in game yet: own placements, the city fix for the Parked Vehicles patch, exact scanner destinations, the focus notice and the enemy boarding ranges.
Spawned vehicles are placed at the recorded surface height plus 0.3 m. If a vehicle you took is left in an area that has unloaded, its spot may place a second one.
With the scanner closed, tapping T on an enemy vehicle can report "Aim at a hostile target"; with the scanner open the highlighted vehicle works.
The 0.63.1 live-verification items still apply; see [known issues](KNOWN_ISSUES.md).

## Downloads

- `FlyingVehicleOverhaul-0.64.0.zip`: extract into the game's `Data` folder; includes `SeamlessPilots.esm` and `SFSE/Plugins/SeamlessVehicle.dll`.
- `FlyingVehicleOverhaul-0.64.0-source.zip`: standalone CMake/MSVC source with bundled SFSE interface header, the test suites, the placement bakers and the recorded placements.
- `FlyingVehicleOverhaul-0.64.0-SHA256SUMS.txt`: checksums for both ZIPs.

The optional `Vehicles - Real.esm` patch remains in the repository's `Data` directory; the main ZIP does not require it.

# Flying Vehicle Overhaul 0.71.0

For Starfield 1.16.244 (Steam) and SFSE. Early work in progress.

- **Every vehicle gets its physics (game bug fixed).** When more than 8 vehicles load at once, the game loses most of them: they are visible but have no physics, so you can't drive them, they never move and NPCs can't use them. The cause is the game's list of vehicles waiting for their 3D. It holds 8 entries, and the game empties the whole list when a ninth arrives (`FUN_141313400`). Only vehicles still on the list get set up when their 3D loads. The mod now keeps its own list next to the game's, with no limit, so every loading vehicle is set up through the game's own path. Verified in game: in New Atlantis, 6 of 22 vehicles got physics before this fix, and all 22 do now.
  - A fallback re-runs the game's own setup for any vehicle near you that is still without physics after 2 s.
  - A static chassis found during vehicle setup is made dynamic, the same way the game handles a keyframed one.
- **Launch guard.** A parked vehicle whose physics starts while it is partly inside the ground (or another vehicle) can be thrown tens of metres. For 10 s after a parked vehicle's first physics step, sideways speed over 4 m/s is removed and upward speed over 2 m/s is stopped. Falling is left alone, and the guard turns off as soon as someone gets in. In game it caught every launch at the end of the load screen, and nearly all vehicles stayed put after it let go.
- **Own placements are plugin references.** The New Atlantis vehicles now come from `FlyingVehicleOverhaul - Placements.esm`, the way Carfield and Parked Vehicles place theirs, instead of being created at run time into your save. With the plugin loaded, the mod places nothing at run time, and vehicles it created in earlier versions are removed once. The plugin's worldspace and cell overrides are written the way the Creation Kit writes them. An earlier build copied Starfield.esm's internal indexes into these overrides and could crash during cell loading. 35 REV-8s in 8 cells. A spot within 4 m of a Carfield or Parked Vehicles vehicle is left out.
- **Random vehicle type per row again.** The plugin parks REV-8s (a plugin can only name vehicles from its masters); in game, each one is matched to its recorded spot and swapped in place to its row's type: seeded per location and row from every vehicle in your load order, the same choice runtime spawning made before. Rows that roll the REV-8 keep it. A vehicle is swapped only when you are more than 10 m away.
- **Wrecked vehicles: occupants ride it down.** NPCs ejected from a hovering wreck used to hang in mid-air where they were placed (the game's exit is only made for a vehicle on the ground). Now the occupants stay in the wreck while it falls and get out once it has been at rest for 0.5 s (at most 6 s, then they get out anyway). An ejected NPC that still hangs is put on the ground beside the vehicle once it offers a safe spot. The airborne exit command still drops NPCs at once.
- **Wreck glide.** A shot-down hover vehicle no longer drops in one frame: for 2.5 s its fall builds at 4 m/s² instead of full gravity and its sideways speed bleeds off, so it sinks and slides down; then normal gravity takes over.
- **Controls toolkit.** Every action is set in `Documents/My Games/Starfield/SFSE/Logs/SeamlessVehicle/bindings.ini`: keys, mouse buttons, XInput controller buttons and chords. Changes apply while the game runs. Hover flies on RT / LT / the left stick by default, and the file includes a commented controller layout.
- **Combat boarding.** Enemies board through the game's combat movement (an activate request), fall back to the get-in package (now flagged Ignore Combat), and are calm only for the moment of seating. At most 4 enemies board or drive at once, taking turns, with a 30 s wait after a failed turn. Entry only needs the vehicle to be at rest.
- **Exit fixes.** A player who was stood up is no longer moved. The hull-side warp could put you 161 m up and fling you kilometres away. Exit warps further than 8 m from the actor are refused, and the seat is cleaned up in place instead.
- **Diagnostics.** The log follows each vehicle's physics: component updates, physics instance, physics steps, and how each native vehicle build ended. It also logs the motion-type changes that freeze bodies, and the game's pending list (`Vehicle pending list: ...`).

## Validation and known issues

All offline vehicle test suites pass, including the new physics-setup suite (23 checks). The standalone source ZIP is built and tested independently.
Verified in game (2026-10-21, New Atlantis): all 22 loaded vehicles get physics at load, and the launch guard catches the load-time launches.
Known: the placed REV-8s still stand 0.5 m in the ground. Their origin is placed 0.3 m above the recorded surface, but a REV-8 reaches 0.81 m below its origin. That is what throws them when their physics starts. The guard holds them, and a placement height fix is next. Vehicles the mod does not place, your own vehicle included, can also be pushed at the end of a load screen; the guard covers them too.
Combat boarding, the controls toolkit, the exit fixes, the per-row vehicle types, the wreck ride-down and the wreck glide have not been tested in game yet. The 0.64.0 and 0.63.1 items still apply; see [known issues](KNOWN_ISSUES.md).

## Downloads

- `FlyingVehicleOverhaul-0.71.0.zip`: extract into the game's `Data` folder. It includes `SeamlessPilots.esm`, `FlyingVehicleOverhaul - Placements.esm` and `SFSE/Plugins/SeamlessVehicle.dll`. Enable both ESMs in your load order. The placements plugin is optional, but without it no vehicles are parked in New Atlantis.
- `FlyingVehicleOverhaul-0.71.0-source.zip`: standalone CMake/MSVC source with the bundled SFSE interface header, the test suites, the placement tools and the recorded placements.
- `FlyingVehicleOverhaul-0.71.0-SHA256SUMS.txt`: checksums for both ZIPs.

The optional `Vehicles - Real.esm` patch remains in the repository's `Data` directory; the main ZIP does not require it.

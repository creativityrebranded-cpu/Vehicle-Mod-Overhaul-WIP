Flying Vehicle Overhaul 1.2.1, with Traffic Addon 0.3.1

OLDER SAVE, OLDER TRAFFIC SETUP
Each game save now captures its own routes, route groups, city settings, schedules, vehicle rules, places, zones and remembered traffic registry. Loading an older save restores its snapshot. Later route edits no longer overwrite that save's traffic files. Reloading the same save discards later unsaved traffic edits; saving a branch creates a separate snapshot.

Save / Ctrl+S in the Traffic Manager applies route edits. Save the game to keep them in that game save. A normal save retains the active working directory and does not reset the live fleet. The game restores vehicle positions; this patch does not serialize an exact copy of every autopilot trip's internal progress.

ROUTES THROUGH STRUCTURES: RED WARNING, NOT A REFUSAL
Routes may intentionally run inside or through buildings, hangars and covered structures. Mapped solid intersections show a persistent red clearance warning and remain editable and saveable. Points and sampled path/ring segments show clearance warnings; optional Raise fixes remain your choice. Known terrain violations and invalid coordinates retain their separate validation.

BACKWARDS COMPATIBILITY
Existing v1.2 saves, routes, route groups and profile folders are preserved and remain readable. The ESM field and API 13 are unchanged; no fresh playthrough, route reset or interface bump is required. Prior supported API 13 consumers and the read-only API 4 integration are retained. The new addon uses an optional file-lock export and continues operating with the prior core's existing behavior; the patched core provides save snapshots.

v1.2 did not record individual historical traffic files. Its older saves retain the preserved shared setup available at migration. Exact traffic-file history begins with saves made after this patch; files never captured by an older version cannot be reconstructed.

SAVING AND BACKUPS
Saved snapshots are never edited by the new Traffic Manager. Loads create a separate working copy under TrafficProfiles/Working/<identity>/routes. Before saving the game, a complete snapshot is created and its identity is stored in the save. Modern route/group and addon registry writes share a lock. A failed copy leaves the previous valid identity intact and reports the failure on the main frame. Back up the complete TrafficProfiles folder with saves.

Standalone FVO uses the native SaveGame entry with checked original bytes. With Seamless core present, FVO uses that core's existing before/after-save callbacks instead of installing a competing save hook.

RELEASE VERSUS TEST BUILDS
Player downloads use FVO_RELEASE=ON: routine diagnostics remain disabled and crash reporting remains available. Diagnostic test builds retain logging with FVO_RELEASE=OFF. Installer options remain the same; routes and arrival options are defaults for new playthroughs. Readme now explains route saving versus game saving.

VALIDATION AND LIMITS
This remains a prerelease. Save branching, same-save reload, v1.2 migration, snapshot failure, concurrent writes and clearance policy are tested offline. Mixed prior/current DLLs receive compatibility checks. Actual in-game save serialization and the updated warning display still require live verification.

Known issues remain: city hitches, missing local-map terrain/buildings while flying, lost vehicle physics and shots at traffic that may not register. The reviewed November 1 logs did not identify a new fatal crash; their first-chance faults alone do not establish a crash cause.

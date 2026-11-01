Traffic setups restore with the individual game save

Each game save captures its own routes, route groups, city settings, schedules, vehicle exclusions, landing areas, zones and remembered traffic registry. Loading an older save restores its earlier setup. Later edits do not overwrite its files. Loading the same save again discards later unsaved traffic changes; saving a branch creates a separate snapshot.

Save / Ctrl+S in the Traffic Manager applies route edits to the active setup. Save the game to preserve them in that game save. Unsaved editor drafts are not committed automatically. An ordinary game save does not reset live traffic or change the active working directory. The game restores vehicle positions; exact internal autopilot-trip progress is not serialized by this feature.

Building and solid intersections are red clearance warnings, not route-placement, editing or saving refusals. Intentional routes through structures are allowed. A proposed Raise fix is optional; routes are not automatically moved out of buildings. Invalid coordinates and proven terrain violations retain separate validation.

Backwards compatibility: v1.2 saves and profile folders remain readable and are preserved as migration sources. The existing ESM field (0xD01), API 13 and read-only API 4 remain unchanged. The new addon treats the file-lock export as optional on prior cores, retaining their existing behavior. Install the patched core for save snapshots. No fresh playthrough or reset of route/group settings is required.

v1.2 used one shared mutable setup and never recorded individual historical files. Its older saves retain the preserved shared setup available at migration; exact per-save history starts with saves made after this patch. History that was never captured cannot be reconstructed.

Open the local map, press F7 and click Readme beside City for instructions. Readme does not commit unsaved edits or reset the introduction.

Files live under Documents/My Games/Starfield/SFSE/Logs/FlyingVehicleOverhaul/TrafficProfiles. Numbered folders directly under that directory are saved snapshots or preserved legacy profiles. Working/<identity>/routes is the active editable copy. Use the Traffic Manager instead of manually editing saved snapshot folders. Back up TrafficProfiles with your saves: the game save stores the identity, while the files live separately.

A missing snapshot pauses traffic instead of silently resetting it. A failed snapshot copy keeps the previous valid identity and reports the error on the main frame. Installed Data/SFSE/Plugins/FVO_Traffic files remain defaults for new playthroughs; changing them does not overwrite existing saved setups.

Follow DEVELOPMENT_MANDATE.md: all updates must preserve prior saves/data and supported integrations. Offline validation does not establish in-game acceptance.

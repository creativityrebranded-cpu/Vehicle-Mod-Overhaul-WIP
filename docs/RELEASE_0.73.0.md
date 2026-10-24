# Flying Vehicle Overhaul 0.73.0 (WIP)

The controls previously shared command handlers, lost unrelated contextual bindings during rebinding, and could leave keyboard defaults unbound. This update gives 52 commands/tool actions their own rows and evaluates their device, context and gesture centrally.

- Separate gear, descend, summon, escort recall, follow, crew driving, Auto, destination, route clear, guard, ram, focus, passenger and throttle-lock commands.
- Rebinding rejects real overlaps without stealing another command. Keyboard/controller slot clearing and defaults preserve the other device. Both settings interfaces use scoped slot operations.
- Deterministic keyboard defaults; automatic layout 4 migration with a previous-file backup and retention of edited rows.
- Controller and keyboard both enter the landing-before-driver-exit path. Configured ground motion, boost and vertical boost feed the native vehicle input submission.
- Modal UI suppresses mod gameplay input. Button ownership follows a press through its release across context changes. Scanner Activate no longer invokes occupied-vehicle seat commands.
- Controller HUD editing: scanner View/Create double tap enters; Circle/B saves and exits. Editing has its own controls, including motion, selection, size, spacing and reset.
- Placement editing and area summon points from the post-0.72.0 installed build are included.

The production command resolver's device/context/gesture matrix passes 388 checks. Full standalone build and suite results are recorded in the accompanying repair report. These are offline checks, not live gameplay certification. The new input hooks, menus, driver exit and HUD editor require a fresh in-game test.

Known issues remain: partly buried REV-8 placements, parked small-vehicle hopping, momentum-aware AI planner tuning, and no atmospheric lift/stalls. Physical flight remains experimental and off by default.

## Files renamed

Every file now carries the mod's name, Flying Vehicle Overhaul (FVO), and says what it is:

- SeamlessVehicle.dll -> FlyingVehicleOverhaul.dll (the core mod)
- SeamlessPilots.esm -> FVO_NPCPilots.esm (NPC walking and boarding)
- FlyingVehicleOverhaul - Placements.esm -> FVO_CityPlacements.esm (optional parked vehicles)
- Vehicles - Real.esm -> FVO_ParkedVehiclesPatch.esm (optional Parked Vehicles patch)
- SeamlessVehicle.log -> FlyingVehicleOverhaul.log
- Logs\SeamlessVehicle\ -> Logs\FlyingVehicleOverhaul\ (settings folder), and seamless.ini -> settings.ini
- Internal names: the NPC quest and package editor IDs (FVO_NPCPilotsQuest, FVO_PilotGetIn0..5 ...), the plugins' author field, the placement-recording flag file (FlyingVehicleOverhaul.placement-recording; the old name still works)

When updating, delete the old DLL from Data\SFSE\Plugins (both together would load the mod twice) and swap the plugins in your load order. The first start moves the settings folder and renames seamless.ini for you, so settings, HUD layout and bindings stay. If the old placements plugin is still enabled, the mod still recognises it, and the log warns if both are enabled.

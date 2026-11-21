Flying Vehicle Overhaul 1.3.0, with Traffic Addon 0.4.0 (prerelease)

STATUS
Offline checks only. Nothing in this release has been run in game yet. Treat it as a test build and keep a save from before
installing it.

WHAT THIS RELEASE IS
1.3.0 is built from the main development tree. It carries the 1.2.4 fixes (menus keep their keys, loading a save from a running
game, vehicles without a chassis after a reload, saved hover, passenger control) and the main-tree work since 1.2.1. The traffic
system was rebuilt around one population system, and the systems it replaced were removed.

TRAFFIC: ONE POPULATION SYSTEM
- The physical fleet: every traffic car is a real vehicle with a real driver. The core moves it on a rail path or with the existing
  autopilot, whichever fits where the car is. The add-on fills empty route positions under the loading screen and the X panel, and in
  play replaces real shortfalls near you. Every route position counts, not the old 32-car live cap.
- Rings of visibility order the work: cars and empty positions out of your reach are served first. The rings never refuse a car.
- Removed: the factory bay, runtime and below-world stock, landing spots, the dot world (virtual traffic), the spare-reuse teleport
  and every fallback for older cores. Everyday NPC trips through the Traffic Addon were already stripped in the development tree on
  2026-11-05 and are not in 1.3.0 (1.2.x and Traffic Addon 0.3.1 still have them).
- Fixed: loading a save now clears the model refusal lists and the population inspection list (that code never ran before). The live
  traffic log reports the real cap.

UPDATE THE MAIN MOD AND THE TRAFFIC ADDON TOGETHER
- Traffic Addon 0.4.0 needs core 1.3.0. With an older core it still loads but creates no traffic, and says so once in the log
  (event=refused reason=core_without_fleet_rail_exports).
- Core 1.3.0 keeps the version-13 traffic table and the read-only version-4 API, so the published Traffic Addon 0.3 and 0.3.1 still
  load and attach to it.

OLD SAVES AND FILES
- Readers stay for old saves, traffic profiles, journeys, fleet snapshots and settings; missing fields get defaults.
- Rows that older versions wrote for the removed systems (factory_v1_, factory_v2_runtime_, landing_) are cleaned up once per load:
  a car the public fleet already holds keeps that owner; a car with a saved journey joins the normal fleet; any other car that is
  still ours is retired out of sight; the row is then deleted. A car that is not loaded keeps its row for a later load.
- An old FactoryBay.ini in the routes folder is skipped, so a factory bay's private road never becomes a public route.

OFFLINE CHECKS (2026-11-21)
- Diagnostic build (FVO_RELEASE=OFF): 139 of 139 runnable test programs pass, including 5 new checks for the old-save cleanup
  (traffic_rules_test). Not run: worldtrust_test (blocked by Windows Defender, as on earlier runs), four dump/route tools that need
  input files, and traffic_compatibility_test, which takes DLL arguments and was run separately.
- Mixed DLL loading (traffic_compatibility_test): new core + new add-on with the fleet ABI; new core + installed earlier add-on;
  earlier core + new add-on; new core + published Traffic Addon 0.3.1; new core + published Traffic Addon 0.3; and the player
  builds: player core + player add-on, player core + Traffic Addon 0.3.1 and 0.3, 1.2.4 player core + player add-on. All pass.
- Player core exports the fleet rail, birth and road clearance functions; the published add-ons 0.3 / 0.3.1 look up none of the
  exports that were removed.

IN GAME, STILL TO CONFIRM
- Traffic appears on a busy route after a load and under the X panel; the log shows traffic.fleet event=path_result and
  traffic.live event=grant, and no event=refused.
- A save from an older version with factory or stock cars: traffic.census event=retired_system_rows on load, and those cars go.
- Save, reload: the same fleet comes back without duplicate cars.

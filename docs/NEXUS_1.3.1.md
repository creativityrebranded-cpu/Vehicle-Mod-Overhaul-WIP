Update/Installation Notice:
Replace your old installation with the new files. Update FVO 1.3.1, Traffic Addon 0.4.1 and the included FVO_NPCPilots.esm together. Traffic Addon 0.4.x needs the FVO 1.3.x core: with an older core it loads but creates no traffic. FVO works with mod managers. Works best with new saves; older saves, routes and settings still load.

Features

* Hover flight: four gears, boost, climb, descend and terrain following.
* Physical flight: momentum and banking for you and every AI pilot, with flight assist, camera roll and an attitude instrument, on autopilot too. Off by default.
* Autopilot: summon, follow, cover, guard and scanner destinations, with chained stops, K-turn recovery and an energy planner that follows the route as its plan.
* Seats and passengers: ride with NPC drivers, tap G in the passenger seat to drive or fly the vehicle yourself, pull a driver out and take the wheel, and followers ride along.
* Combat: improved vehicle combat AI, squads and morale, Defend / Attack / Hold fire and focus fire.
* Damage and wrecks: vehicles and occupants take damage, wrecks come down, and occupants climb out.
* Traffic Manager: press F7 on the local map (or F8 for photo mode) to create and edit traffic routes, hazard boxes, places and links. Readme and Help explain every screen. Autopilot near a traffic zone follows its path.
* Script mode (new): one button in the Traffic Manager. Vehicles follow the route path with no object detection; only the hazard boxes you place, other vehicles and people are obstacles. For roads the normal autopilot does not handle well.
* Traffic Addon 0.4.1: one physical fleet. Every traffic car is a real vehicle with a real driver, created out of sight and kept moving along its route, with models from your whole load order. Each playthrough keeps its own routes, route groups and traffic settings.
* City Placements: parked vehicles in nine cities.
* Saved hover: a save made while hovering comes back hovering. Saved Auto / Cruise resumes on load.
* Controls and HUD: full keyboard and controller support with rebinding, and a movable, resizable HUD.

Requirements

* Steam Starfield 1.16.244 and its matching SFSE. Game Pass and Microsoft Store are not supported.
* Carfield and Parked Vehicles are optional.

Downloads

* FlyingVehicleOverhaul-1.3.1.zip — main mod.
* FlyingVehicleOverhaul-TrafficAddon-0.4.1.zip — optional: city traffic and its routes.
* FlyingVehicleOverhaul-1.3.1-ExtraVehicles.zip — optional: City Placements.
* FlyingVehicleOverhaul-1.3.1-ParkedVehiclesPatch.zip — optional: makes the Parked Vehicles mod's props drivable.
* FlyingVehicleOverhaul-1.3.1-FOMOD.zip — all-in-one installer for Vortex/MO2. Choose your cities, traffic defaults and features. Use it instead of the separate ZIPs.

Installation

* Install the FOMOD with a mod manager, or extract the main ZIP and any addons into Starfield\Data.
* Enable FVO_NPCPilots.esm and any optional ESMs. Launch through SFSE.
* When updating, replace the old installation. Remove SeamlessVehicle.dll, FVO_CityPlacements.dll, FVO_CityTraffic.esm and Vehicles - Real.esm if present.
* Your saves keep their own traffic setup. Loading a save from an older version cleans up cars from removed traffic systems once, out of sight.

Settings

* Settings > Gameplay Options > Flying Vehicle Overhaul. Bindings are under Settings > Controls. Script mode is switched in the Traffic Manager.
* Config files are in Documents\My Games\Starfield\SFSE\Logs\FlyingVehicleOverhaul. Back up its TrafficProfiles folder with your saves.

Main controls

* Keyboard: V hover or land, Space climb, C descend, Shift boost, T Auto, E exit. On foot, N summons and U is follow or cover. Hold E at an occupied vehicle to pull the driver out. In the passenger seat, tap G to take or give back control. F7 opens the HUD editor, or the Traffic Manager on the local map; F8 opens it in photo mode.
* Controller: L3 hover or land, Y climb, X descend, RB boost, D-pad Left Auto, B exit.

Known issues

* Prerelease: this version has been checked offline only. Keep a save from before installing it.
* Shots at traffic may not register yet.
* City hitches remain; the local map can miss terrain or buildings while flying.
* Some NPCs give up walking to their vehicle.
* Pulling out a neutral driver counts as theft.
* More live vehicles cost more CPU.
* Getting out when the game refuses the exit can still set you on the vehicle's roof with a short loading screen.

Reporting

* Include your versions, location and steps. Player downloads disable routine logging but retain crash reporting. After a crash, include FlyingVehicleOverhaul.log from Documents\My Games\Starfield\SFSE\Logs and last-crash.dmp from its FlyingVehicleOverhaul folder, if available.

Credits

* The SFSE team. SarthesArai (Carfield, Parked Vehicles).
* Source: https://github.com/creativityrebranded-cpu/Vehicle-Mod-Overhaul-WIP

Making your own routes

* Install the Traffic Addon, open your local map and press F7. Create or edit a route; Readme and Help explain the controls.
* Place Hazard boxes on anything solid the vehicles should avoid. In Script mode they are the only fixed obstacles.
* Turn on Show traffic routes in the settings to see the lanes.
* Active .ini files are in TrafficProfiles\<identity>\routes under the config folder. Files in Data\SFSE\Plugins\FVO_Traffic are defaults for new playthroughs.
* HOW-TO-MAKE-ROUTES.txt explains every option, with examples. Existing playthroughs keep their own setup when installer defaults change.

Changelog (from 1.2)

1.3.1 (FVO) / 0.4.1 (Traffic Addon)
* New: Script mode in the Traffic Manager. Vehicles follow the route path with no object detection; only hazard boxes, other vehicles (kept apart by their own size) and people are obstacles. A message explains it each time it is switched on.
* Fixed: traffic cars could jump 20-160 m in view when the game woke a parked car's physics. A stopped car now stays where its physics body is and is woken there; only cars with no physics body are moved along the route.
* Fixed: a car taken back onto the route could jump to where it had been carried before.
* Traffic spawns on every route the same way, including hover-only routes; Prepare traffic says the real reason when a route gets no positions.
* Autopilot: K-turn recovery in tight places, planner limits derived from each car's own speed, braking and turning instead of fixed numbers, and adoption of cars under the loading screen.
* Your saved Auto / Cruise resumes when you load the save.
* Hazard boxes: a Hazards tool in the Traffic Manager. Vehicles plan round them, or stop short.
* The Traffic Manager's first-time message and Readme use its own panel (one pointer, E closes it).
* Traffic Addon 0.4.1: background flow (every route position is a moving car), batched creation under the loading screen and X panel, performance pass (fewer, cheaper planning passes for cars out of view).

1.3.0 (FVO) / 0.4.0 (Traffic Addon)
* Traffic rebuilt around one population system: the physical fleet. Every traffic car is a real vehicle with a real driver; the core moves it on the route or with the autopilot, whichever fits where it is.
* Removed: the factory bay, below-world stock, landing spots, virtual traffic dots and the spare-reuse teleport.
* Loading a save clears the model refusal lists; old-save rows from removed systems are cleaned up once per load.

1.2.4
* Menus keep their keys (ship builder G, local map, Controls page rebinding).
* No crash when loading a save from a running game.
* Vehicles without wheels or hover after a reload are repaired.
* Saved hover; drive from the passenger seat (tap G).

1.2.1
* Each game save keeps its own routes, groups and traffic settings; loading an older save restores them.
* Routes through structures show a red warning instead of being refused.

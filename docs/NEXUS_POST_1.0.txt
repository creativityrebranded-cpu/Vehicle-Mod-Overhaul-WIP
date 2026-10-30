Flying Vehicle Overhaul 1.0, with Traffic Addon 0.1

This release marks the mod's move from WIP to 1.0. Traffic becomes real, editable, and interactive, and the hitches in busy cities are addressed at their cause.

This is a prerelease: the newest traffic fixes are still being tested in game. Please report problems with your log (path below).

TRAFFIC MANAGER
Open your local map and press F7 to manage traffic routes for that planet. The routes you create decide where vehicles fly, and traffic is generated along them. Autopilot near a traffic zone follows the same path. A short message explains this the first time you open the local map in a playthrough. It is stored in your save, so a new game or character sees it again.

TRAFFIC ADDON 0.1
All traffic decisions now live in their own plugin, FVO_Traffic.dll, which replaces FVO_CityPlacements.dll. Never keep both. City Placements is now only FVO_CityPlacements.esm.

REAL TRAFFIC
Vehicles and their drivers are fully simulated rather than scenery. You can shoot at them, board them, or pull the driver out and take the vehicle, and a vehicle you take is yours: traffic lets go of it and fills its route position with another. Drivers who are attacked or caught in a fight react, and hostile drivers attack you.

ARRIVALS
Traffic is placed inside the game's own loading screen, so a city has traffic when you arrive, without any extra loading. Models are picked from every vehicle in your load order, seeded per route position. Up to 256 vehicles are supported; FVO_NPCPilots.esm now has 256 pilot slots and stays compatible with existing saves.

DRIVERS STAY WITH THEIR VEHICLES
A long list of fixes for drivers left floating or left behind. Vehicle and driver are now handled as one pair with one health record: a healthy pair keeps its distance exemption, a pair that goes wrong is recovered, and a pair that cannot be recovered is replaced only under a loading screen or map, or when it is far away and out of sight. Vehicles are kept visible at a distance by the game's own Full LOD, and past the loaded cells they keep flying.

PERFORMANCE
Wheel dust no longer piles up in the persistent cell. It was reaching about 100,000 references in 12 minutes and caused major stutters. A guard also stops the local map crash this caused in large cities. Many smaller optimizations are included.

REMOVED
Rams, scheduled traffic and the road-network (GHN) traffic.

INSTALL / UPDATE
Use FlyingVehicleOverhaul-1.0.zip plus any addon ZIPs (the Traffic Addon and City Placements are in FlyingVehicleOverhaul-1.0-ExtraVehicles.zip), or the FOMOD instead. Replace your previous FVO installation and do not keep two copies of the DLL. Delete SFSE/Plugins/FVO_CityPlacements.dll and FVO_CityTraffic.esm if you have them. Requires Steam Starfield 1.16.244 and its SFSE.

KNOWN ISSUES
- Some vehicles drop to the ground when the game builds their physics. The mod puts them back or brings the driver to them, and you may see this close up.
- The game sometimes takes drivers out of their seats; the mod re-seats them, and a vehicle may land if that keeps failing.
- A broken pair close to you stays visible until it can be replaced unnoticed.
- A freeze was seen once during testing, as a city loaded, with an experimental driver setting on. That setting is now off and the release was rebuilt, but the cause is not proven. If the game freezes, the log now records where it stopped; please send it.
- Drivers have no model past the loaded area, which is normal for the game; the vehicle keeps flying and the model comes back as you approach.
- Physical flight stays off by default and is experimental.

Logs for bug reports: Documents\My Games\Starfield\SFSE\Logs\FlyingVehicleOverhaul.log

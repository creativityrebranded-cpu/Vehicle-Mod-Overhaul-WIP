Flying Vehicle Overhaul 1.2, with Traffic Addon 0.3

TRAFFIC BELONGS TO YOUR PLAYTHROUGH
Each playthrough keeps its own routes, route groups, city settings, schedules, vehicle rules, landing areas, zones and remembered traffic. Returning to the same playthrough keeps its setup. Another playthrough starts from the installed Traffic Addon defaults and can have a completely different setup, across all its cities.

README IN THE TRAFFIC MANAGER
Open the local map, press F7, then click Readme beside City. It opens instructions in the same native game dialog as the introduction. You can reopen it without saving or discarding your current route edits.

UPDATE THE MATCHING FILES TOGETHER
Install FVO 1.2 with Traffic Addon 0.3 and the included FVO_NPCPilots.esm. Older generating addons are rejected by the new interface. The main ZIP and FOMOD both include the updated plugin file.

KEEPING YOUR CURRENT SETUP
Load your usual existing playthrough first. The first existing playthrough whose introduction was already shown imports the old shared setup once. Save the game after the profile is assigned; subsequent saves inherit that identity. Other playthroughs start fresh. Saves made before the identity was assigned do not contain it.

BACK UP TRAFFICPROFILES WITH YOUR SAVES
The save stores the setup's identity. Its files live separately under Documents/My Games/Starfield/SFSE/Logs/FlyingVehicleOverhaul/TrafficProfiles/<identity>/. Back up that folder with your saves. A missing setup pauses traffic rather than silently resetting it.

Installer route choices and files in Data/SFSE/Plugins/FVO_Traffic are defaults for new setups. Existing playthroughs keep their own copies. Edit an existing setup through the Traffic Manager. Reloading an older save with the same identity keeps that playthrough's latest route edits; this does not restore an exact snapshot of every vehicle's trip.

RELEASE AND TEST BUILDS
Player downloads use FVO_RELEASE=ON: routine and verbose logs, freeze watching, stall/hang dumps, telemetry, map traces and debug keys are disabled. Crash reporting and crash dumps remain, along with minimal startup/installer and profile-error messages. Development builds default to FVO_RELEASE=OFF and retain diagnostics.

VALIDATION AND KNOWN ISSUES
This remains a prerelease. Both build modes compile, six relevant offline suites pass, existing plugin records are unchanged apart from the new identity global, and DLL load/API checks pass. Profile switching and the Readme popup have not yet been verified in game. This does not establish a performance improvement.

Known issues carried forward: local-map terrain/buildings can be missing while flying; city hitches remain; some vehicles lose physics and require recovery; shots at traffic may not register. The v1.1 traffic and crash fixes are included.

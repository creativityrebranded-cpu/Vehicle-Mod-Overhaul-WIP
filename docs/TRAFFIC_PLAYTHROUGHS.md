Traffic Manager setups belong to playthroughs

Each playthrough keeps its own routes, route groups, city settings, schedules,
vehicle exclusions, landing areas, zones and remembered traffic vehicles. Another
playthrough starts from the installed Traffic Addon defaults and can develop its
own setup. Reloading any save with the same playthrough identity keeps that
playthrough's latest route edits.

Open the local map, press F7, then click Readme beside City for instructions in
the game's native message dialog. Readme is always available and does not reset
the first-time introduction or commit unsaved edits.

Save your game after first using the updated addon: the saved identity is inherited
by subsequent saves. A save made before that identity was assigned has no identity
to restore. The first existing playthrough whose introduction was already shown
imports the old shared setup once. Later playthroughs start fresh.

Traffic files live under Documents/My Games/Starfield/SFSE/Logs/
FlyingVehicleOverhaul/TrafficProfiles/<identity>/. Its routes folder contains the
editable route files and VehicleGroups.ini. The game save holds the identity;
the full route files are stored separately. Back up TrafficProfiles with saves
when moving computers or restoring backups. A missing profile pauses traffic
rather than silently replacing its setup.

Installed Data/SFSE/Plugins/FVO_Traffic files are templates for future profiles.
Updating or editing those templates does not overwrite existing playthroughs.
Update FlyingVehicleOverhaul.dll, FVO_Traffic.dll and FVO_NPCPilots.esm together.

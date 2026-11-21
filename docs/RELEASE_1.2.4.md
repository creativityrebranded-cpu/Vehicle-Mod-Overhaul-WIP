Flying Vehicle Overhaul 1.2.4, with Traffic Addon 0.3.1 (unchanged)

MENUS KEEP THEIR KEYS (Nexus report: Controls page froze, G did nothing in the ship builder / local map)
FVO took its keys away from the game in every menu except the console. Now any menu gets every key and button back: the ship
builder's G adds modules, the local map keeps its keys, and rebinding on Settings > Controls takes any key. FVO's keys stay on in
normal play, the scanner, the vehicle and the ship. The list of menu contexts comes from the game's own code.

LOADING A SAVE FROM A RUNNING GAME (Nexus report: crash on load)
FVO drops everything it holds from the old world as soon as a load starts and runs its full update only while a world is ready.
Door, fast-travel and landing loading screens keep working as before.

VEHICLES WITHOUT A CHASSIS AFTER A RELOAD
After loading a save in an already running game, vehicles a plugin placed in a loaded cell (for example parked REV-8s, including
the one you sit in) could come back without their physics part: no wheels, no hover, falling or tumbling. The game only builds that
part when a cell attaches a reference, and a cell that stays loaded through the reload does not. FVO now asks the game's own attach
for those vehicles right after the load returns, and again later if one is still missing.
The game's 8-slot waiting list for vehicles is still covered by FVO's own list (many vehicles loading at once, such as City
Placements in New Atlantis).

SAVED HOVER
A save made while you hover keeps a small record next to FVO's settings; loading that save turns the hover back on at the saved
height and gear (speed is not kept). Older saves and saves made with the hover off load as before. FVO's core now owns the game's
save hook in every install, so this works without the Traffic Addon; traffic snapshots still run only with the addon.

DRIVE FROM THE PASSENGER SEAT
Riding with an NPC at the wheel, tap G to take control and tap G again to give it back. Hovering, it flies exactly like your own
vehicle (your keys, camera turning, climb/descend, gear, boost); on its wheels your keys go to the game's own driving. The hover key
switches between hover and wheels. No seat change: the NPC stays at the wheel. Leaving is the game's own hold E, as before.
The old hold-G take-the-wheel seat swap is removed (its key setting is kept so saved bindings still load).

INSTALLER
"Choose features" without the Traffic Addon no longer rejects the whole feature set because the route-beacon page was hidden.

BACKWARDS COMPATIBILITY
No save, settings, route or plugin format changes. API 13 and the read-only API 4 are unchanged; Traffic Addon 0.3.1 is unchanged.
New files: FlightStates\<save>.txt (saved hover records) in Documents\My Games\Starfield\SFSE\Logs\FlyingVehicleOverhaul.

RELEASE VERSUS TEST BUILDS
Player downloads use FVO_RELEASE=ON: routine diagnostics off, crash reporting kept.

VALIDATION AND LIMITS
Prerelease. Offline: command input 401 checks, saved hover 17, installer 22, bindings, mixed old/new DLL compatibility.
In game (diagnostic builds): ship builder keys, menu hand-back, six loads from a running game without a crash, missing-chassis
repair, saved hover after a load from the main menu, passenger control on wheels and in hover.
Not yet verified in game: the local map and Controls-page rebinding, the early re-attach on a quickload while seated, saved hover on
a quickload in a running game, hover flying from the passenger seat after the latest changes.
Known: getting out when the game refuses the exit can still set you on the vehicle's roof with a short loading screen.

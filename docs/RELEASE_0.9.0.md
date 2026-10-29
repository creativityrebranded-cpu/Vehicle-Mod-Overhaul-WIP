FVO 0.9 - city traffic in ten cities

This update is about traffic. City traffic now flies over ten cities, starts already in the air when you arrive, and keeps flying when it leaves the area the game has loaded around you.

CITY TRAFFIC IN TEN CITIES
Eleven routes: New Atlantis (a slanted ring round the city, high over the MAST and low over the spaceport, and a loop through its districts), Akila City, Cydonia, Eleos Retreat, Gagarin, HopeTown, Neon, New Homestead, Paradiso and Red Mile. Each has exits and landing places, and the routes stay clear of the rooftops. They are plain text files in Data\SFSE\Plugins\FVO_Traffic, so you can edit, add or remove them; HOW-TO-MAKE-ROUTES.txt explains every line.

TRAFFIC ALREADY FLYING
Vehicles with seated drivers start on the routes, so a city has traffic the moment you arrive. They are made once per save and reused on later visits, never spawned again and again. Calm NPCs still take parked vehicles onto the routes as well. The installer can turn the starting traffic off.

TRAFFIC FAR AWAY
The game keeps an invisible box round the area it has loaded (about 500 m across). Vehicles flying into it used to bounce back and lose their drivers, and past it they hung in the sky. FVO's vehicles and drivers now pass through it, stay loaded, and keep moving where you can see them. The box stays for everything else, because it is also the game's rescue for anything that falls through the world.

SHOT-DOWN VEHICLES
Occupants of a wreck, and drivers who get out to fight, now stand up and walk off, the same way a driver does when you pull them out.

CITY PLACEMENTS
Parked vehicles in nine cities instead of only New Atlantis, for NPCs and traffic to use.

ROUTE BEACONS
Beacons along every traffic lane, at its exits and at landing places, on by default. Turn them off in the installer or with Show traffic routes in the settings menu. They only show the routes; traffic flies either way.

INSTALLER
Choose city traffic route by route, choose whether traffic starts already flying, and choose route beacons. City Traffic and City Placements are now ticked by default. The default HUD layout is new.

INSTALL / UPDATE
Use FlyingVehicleOverhaul-0.9.0.zip plus any addon ZIPs, or the FOMOD instead. Replace your previous FVO installation; do not keep two copies of the DLL. Requires Steam Starfield 1.16.244 and its SFSE.
If you used the separate ExtraVehicles ZIP before, its new route files replace NewAtlantis.ini and add the other cities.

Logs for bug reports: Documents\My Games\Starfield\SFSE\Logs\FlyingVehicleOverhaul.log

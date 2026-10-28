# New Atlantis air traffic — local experimental build

Enable `FVO_AirTraffic.esm` with the matching FlyingVehicleOverhaul DLL. Start
Starfield through SFSE. The file uses the installed base game and SFBGS004.esm
(REV-8). Keep the retired `FVO_CityTraffic.esm` disabled. No new game is required
by the implementation, but save/load behavior still needs an in-game check.

Sixteen persistent vehicles are authored airborne. Each begins following its
current schedule position when its model loads. Native physics velocities move
the vehicles; the controller does not move their references every frame.
Initialization after loading a model, loading a save, or explicitly editing the
ring may reposition a vehicle once. Game calendar/time-scale data supplies a
shared simulation clock, so pausing does not advance the schedule.

The schedule also runs for unloaded vehicles. When an unloaded vehicle's
current cruise position comes within 300 metres of the player, one native move
places it there so streaming can load its model; admission resets beyond 400
metres. Models outside the loaded neighborhood can remain absent while their
logical schedule continues. Initial seeding waits for a valid native body.
Tracking uses reference coordinates for both current and target positions;
it does not assume Havok's local origin is near the reference-world origin.

Each rotation assigns eight vehicles to circulation and eight to staggered
landing, waiting and ascent. The cohorts exchange roles on the next rotation.
This means eight designated visit opportunities, rather than eight vehicles
descending simultaneously. A vehicle joining after its approach start stays
airborne until its next visit opportunity. Each visit uses a different recorded
placement. Three asynchronous clearance rays must finish before approach; an
occupied, blocked or unverified placement is skipped. These are basic corridor
checks, not a complete swept-hull survey or traffic conflict planner.

The two altitude-separated circulation rings retain the user's recorded route
from `ringroad.ini`; the limited in-game view did not establish a better route.
The route can still be edited with the existing ring controls. This build uses
16 recorded upper-level placements in New Atlantis. Covered lower-level pads
and other cities require additional route definitions and are not enabled.

The vehicle owns the schedule. Its paired actor gets one native instant-seat
assembly attempt after both models and the vehicle body are ready. There is no
walk-to-vehicle job or seating retry loop. If assembly fails, the actor is hidden
and vehicle motion continues; visible occupied vehicles are therefore still an
in-game acceptance requirement. A player-used, differently occupied or wrecked
traffic vehicle is released from traffic control for that load.

Seven source crowd-region definitions are overridden to 75% of their original
density. Their pools, conditions and other fields are retained. Pilots reuse
four generic New Atlantis crowd templates; named NPCs and quests are unchanged.
This density reduction is a population budget, not a measured one-for-one
replacement of 16 pedestrians. There is no runtime census.

Work per update is limited to four fleet slots, one ray submission and one
occupant assembly attempt, with a soft 0.75 ms scheduling budget between slots.
The native callback reads bounded atomic commands with full body generation,
load epoch and 500 ms expiry checks. Individual native calls can exceed the
soft budget. Offline checks do not establish frame-time improvement.

Look for `Air traffic:` in FlyingVehicleOverhaul.log. Acceptance still requires
a fresh SFSE launch in New Atlantis: occupied movement, several alternating
rotations, landing/ascent clearance, pause/resume, save/load and frame pacing.

To return to the preceding local build, close the game, disable the air traffic
plugin, restore the prior DLL from the installation backup, and load a save
made before enabling this content. In-save removal has not been validated.

This local DLL also updates prompt-device switching: a fresh native controller
button/trigger or keyboard/mouse button press selects that device, including
inside the HUD editor. Held buttons, mouse motion and stick drift do not switch
it. Native event order resolves simultaneous polling flags; Windows gamepad
virtual keys are excluded from keyboard detection. A connection alone does
not force a prompt-device change. In-game appearance still needs validation.

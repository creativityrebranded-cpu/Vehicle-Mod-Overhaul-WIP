# Dedicated NPC traffic timetable — local experimental build

This build reuses six existing FVO_CityPlacements.esm vehicles on the upper level of New Atlantis. It adds FVO_ScheduledTraffic.esm: 18 fixed NPCs, 24 invisible queue/walk-away markers, one quest and 18 travel packages. It adds no vehicle records and changes no native crowd-density records.

Three stops (A/B/C) have two allocated physical vehicle spaces each. Six original placements are allocated: local IDs 812, 813, 817, 819, 81B, 81D. All other original placements remain outside this service. Three traffic pilots can be active by default, sharing the existing six NPC-driving slots with other FVO jobs. Traffic waits when the shared slots are busy.

## Schedule

1. A specific NPC travels to its line marker. Ready actors are selected FIFO for that physical bay.
2. When the vehicle is stopped at a safe entry site, lease an idle driving slot, send the next actor to the vehicle, then issue one native Activate request when close enough. Confirm the seat before departure. There is no instant seating or repeated activate loop.
3. After a five-second departure dwell, verify route clearance and acquire the shared physical exit corridor. Climb over the bay, merge onto the recorded ring and follow its waypoint loop with the existing autopilot.
4. Remain in transit for at least 45 active seconds. At the destination exit, an enabled exit, free corridor and a reserved physically clear space are required. Otherwise continue around for another lap. Closing an exit during approach causes a go-around.
5. Land at a safe site, stop, request a normal native exit once and confirm the old pilot is out. The next pilot is not assigned while the previous one still occupies the vehicle.
6. The former pilot walks toward a departure marker, then resumes its inherited ambient behavior. After at least 60 seconds of rest, that actor returns on foot to its original line. The roster remains capped at 18; the system never spawns an endless stream of new actors.

The bay counts as occupied while boarding, parked, unloading or reserved by an inbound aircraft. It is only freed after verified physical clearance. Two physical spaces mean at most two occupied/inbound reservations, not two new landings regardless of what is already parked. Stops sharing a ring exit also share one crossing-aircraft lock.

Queue path deadline: 120 seconds. Boarding deadline: 60 seconds; on failure the actor rests and the vehicle waits 15 seconds before selecting a different ready actor. Walk-away path deadline: 45 seconds. Unresolved native exit: 20 seconds, then the affected vehicle halts. These timers use admitted active update time, excluding long menu/loading gaps.

## Configuration

Optional `scheduled-traffic.ini` belongs alongside `settings.ini` in `Documents/My Games/Starfield/SFSE/Logs/FlyingVehicleOverhaul/`. The shipped sample is documentation; absent settings use the same defaults.

```ini
[traffic]
enabled=1
maxActiveDrivers=3
[exits]
A=1
B=1
C=1
```

Driver limit is clamped to 3..6. An exit set to 0 prevents new departures/admissions at that stop; inbound aircraft go around. `enabled=0` stops new dispatch/admission; it does not erase the roster or teleport aircraft to parking. The existing `ringroad.ini` controls the ring geometry. Native FVO NPC vehicles must be enabled for new dispatch.

## Validation limits

This is an experimental local implementation, not a claim of successful in-game traffic. Native queue walking, seat activation, landing clearance, dismount and streaming still need fresh gameplay verification. The HTML is an interactive behavior model with illustrative movement timings, not a connection to the game.

The ordinary autopilot requires loaded physics. Traffic pauses when a vehicle unloads and can resume when it loads again; this build provides no virtual city-wide movement or forced cell loading. A full circuit crossing unloaded cells may pause until the player approaches. A geometric probe point does not prove a navmesh connection or clear vehicle door.

On save load stable IDs are resolved afresh. Confirmed seated roster members can regain an available driving slot. An unoccupied vehicle found away from its original bay is halted rather than teleported. Exact timetable-phase persistence and automatic recovery of halted services are not implemented.

Dead/missing drivers, wrecks, player takeover, lost leases and refused exits halt the affected vehicle and retain unresolved reservations. `FlyingVehicleOverhaul.log` reports phase transitions, the precise halt reason and occupied/reserved counts every 30 seconds. Do not interpret a successful package assignment as confirmed boarding.

## Installed dependencies

Requires SFSE, FlyingVehicleOverhaul.dll, the unchanged original FVO_CityPlacements.esm, FVO_NPCPilots.esm and their existing game/vehicle masters. Enable FVO_ScheduledTraffic.esm in addition to the existing set. FVO_AirTraffic.esm, FVO_CityTraffic.esm and InteractiveCover.dll remain absent from the installed game.

The independent content validator checks every group/record boundary, local ID uniqueness, the 36 aliases, 18 redirected package targets, record counts, and absence of new vehicles or crowd overrides. The timetable stress test verifies bay/crew/pilot ownership across thousands of completed services. Offline results do not establish native AI or streaming behavior.

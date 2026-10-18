# Known issues and next steps

## Open bugs and suspects
- **Parked small vehicles hop about.** Not reproduced in a log yet. Suspects: (1) a REV-8 and a V-47 sit 1 m apart at (657, 635) in every test save: overlapping physics bodies
  (the V-47 has a runtime id, probably left in the save by an older run-time swap); (2) the boost pin used to apply to every vehicle, parked ones included (now only vehicles in use).
  The log prints `Vehicle hop:` (form id, base, position) for a parked vehicle that moves 0.6 to 50 m by itself.
- **Hull sweep on the parked rover.** At autopilot start the sweep reports a hit about 3 to 5 m ahead with 0 hits filtered as "own body". Probably the rover's own neighbour or its own body;
  it clears once the rover moves. Not investigated.
- **Push-ups.** "pushed up 216 to 268 m/s: something under it the ground rays miss" appeared on autopilot and in manual flight. The existing handling keeps the vehicle flying.
- **Assist flip-flop.** In manual flight near large terrain the assist alternates "turning left" / "turning right" within a few hundred milliseconds, and sometimes stops ("no way forward") at 25 m/s.
- **Passenger seat.** An actor that already holds the driver keyword may be seated as the driver.
- **Sweep cost.** About 400 hull sweeps (about 0.7 ms each) and 2,800 rays a second while on autopilot, roughly a quarter of a core on the probe thread. Rate-limit if the frame rate suffers.
- **Pitch controller.** "pitch moved against the command; pitch spin reversed" logged twice in a flight.

## Not done
- **Terrain assist and autopilot do not plan together.** They only react to each other (the autopilot casts corridors along its own heading plus detour; the assist climbs or turns from the corridor verdicts).
  Next step: both ask one predicted path (the heading the vehicle will really fly, with its yaw lag and detour) against `navmap`, get back a required altitude profile and the first blocked point, and commit to a choice for a while (hysteresis).
  Details in `ARCHITECTURE.md` ("Navigation and the terrain assist").
- **The game's own leash / home location.** The 600 m leash is measured from where the NPC stood when the job began, not from the NPC's editor placement.
- **Per-vehicle HUD rows.** One box lists up to four vehicles, joined with " | ".
- **Player-vehicle behaviours** (a defensive / offensive mode for the vehicle you drive): dropped.
- **Animated vehicle entry.** `kAnimatedEntry` is off; the quest-package walk works, the Activate-package animation only in some spots, so the instant seat is used.
- **Companion seats for the player's followers beyond the first passenger** are not tested.

## Practices that paid off
- Check a "working" sensor by its hits in the log, not by the absence of complaints (the forward ray faulted silently for three runs).
- Decompile the game's own use of an engine call before copying the pattern (the world lock, the embedded hknpWorld, the refcount).
- Keep one install per game run; copy the log before relaunching.

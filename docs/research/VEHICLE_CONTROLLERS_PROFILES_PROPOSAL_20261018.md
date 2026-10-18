# Proposal: controllers and profiles (2026-10-18)

The user: "build a system to differentiate them and propose them, and then propose profiles unique to each".
Status: **proposal only**, nothing built. It is the board's step (4) "controller table, then seat broker".

## 1. One record per vehicle

```
VehicleControl
  id            vehicle form id (from the registry / vehicleworld)
  owner         Player | Faction(id) | None
  controller    None | Player | PlayerRemote | Npc        <- who decides where it goes
  driver        actor id or 0 (who sits in the driver seat)
  passengers    actor ids (incl. the player)
  profile       Profile id (what it is trying to do)
  pilot         autopilot slot 0..6 or -1 (manual / parked)
```

### Controllers (how they are told apart)
| Controller | Decided by | Means |
|---|---|---|
| **None** | nobody in the driver seat, no command | parked; the registry just stores it |
| **Player** | the player is in the driver seat | the player drives; profiles can assist or take over (Auto) |
| **PlayerRemote** | the player's own vehicle, driver seat empty, the player gave a command | the autopilot drives it for the player (Summon, Follow, Hold) |
| **Npc** | an NPC is in the driver seat (a job) | the autopilot drives for that NPC's profile |

The **seat broker** is the only code that changes `driver` / `passengers` (boarding, hand-over, hijack, pull-out, force-out,
passenger, take the wheel); `controller` follows from the seats plus ownership. Today those rules are spread over
`npcvehicles.cpp` and `passenger.cpp`.

### Profiles
A profile = a **goal source** (where to go each frame) + **parameters** (park when still, hostile, fire, orbit, formation
slot, speed limit, height) + **which controllers may use it**. The autopilot (`Fly()` with a `Goal`) stays shared by all.
Today the player's modes live in `PlayerTick`, the NPC ones in `kProfiles`, the taxi in `passenger.cpp`: one table
replaces the three.

## 2. Profiles

### Shared (any controller)
| Profile | Goal | Notes |
|---|---|---|
| **ToMarker** | the player's map marker | = today's Auto (player) and Taxi (NPC driver, player passenger) |
| **Hold** | a fixed spot | = today's hold-on-exit |
| **Follow(target)** | trail a target | = today's player Follow and NPC Escort, with a formation slot |
| **ReturnHome** | the vehicle's home spot (registry) | park where it was found |

### Player only (you drive)
| Profile | What it does |
|---|---|
| **Manual** | the game's own driving |
| **Hover** | today's hover flight with assist (gears, terrain, avoidance) |
| **Auto** | ToMarker for your own seat; any W/A/S/D takes over |
| **Cruise** | keeps heading and height; you only steer with the mouse (the earlier camera-heading Auto) |
| **ConvoyLead** | the gang forms up behind you (left / right / rear slots) and mirrors your get in / out |

### Player's vehicle, nobody aboard (PlayerRemote)
| Profile | What it does |
|---|---|
| **Summon** | to you; teleports behind you when far / unloaded |
| **Follow** | trails you, parks when you stop |
| **Hold / Guard spot** | stays where you left it; (later) fires at hostiles near it |
| **Fetch** | flies to a follower or a marker and brings them back (with an NPC passenger) |
| **Scout** | flies ahead to the marker and back, a look-out (no weapon) |

### NPC only (an NPC drives)
| Profile | What it does | Today |
|---|---|---|
| **Escort** | follows the player, parks, never fires | built |
| **Pursuit** | hostile, circles the player, fires | built |
| **Wingman** | fights the player's enemies (target from the NPC's combat target) | proposed |
| **Taxi** | drives the player (passenger) to the marker | built (`passenger.cpp`) |
| **Commuter** | flies to where its package sends it (`traffic::DestinationOf`), lands, gets out | research done |
| **Patrol** | loops a route (patrol package / waypoints) | proposed |
| **Hauler / Courier** | between two places (cargo, a message) | proposed |
| **Retreat** | low health / outgunned: flies away, lands out of sight | proposed (needs vehicle health) |
| **Hijacker** | takes a vehicle and flees | partly (seat rules) |

### Passenger combinations
| Driver | Passenger | Profile |
|---|---|---|
| NPC | player | Taxi, RideAlong (follows the player's chosen target), Gunner (the player fires while the NPC drives) |
| player | NPC | Gunner (the NPC fires the vehicle weapon at the player's enemies) |
| NPC | NPC | squad transport (gang members ride together) |

## 3. Keys (unchanged where they exist)
- On foot: N Summon, U Follow, `,` give the NPC you face a vehicle, `.` cycle the gang's profile (Escort -> Wingman -> Pursuit).
- In the vehicle: T Auto (ToMarker), Space x2 Hover, a new key cycles Cruise / ConvoyLead.

## 4. Order
1. The table and `VehicleControl` (no behaviour change: today's modes become rows).
2. The seat broker (moves the seat rules into one place).
3. New rows: Wingman, Cruise, ConvoyLead, Commuter; then the rest.

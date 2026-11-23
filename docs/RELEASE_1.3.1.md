Flying Vehicle Overhaul 1.3.1, with Traffic Addon 0.4.1 (prerelease)

STATUS
Offline checks only. Nothing in this release has been run in game yet. Treat it as a test build and keep a save from before
installing it. 1.3.0 (tag v1.3.0) was packaged on 2026-11-21 and never published; 1.3.1 is built from the main tree as of
2026-11-22 and replaces it. Nexus page text: docs/NEXUS_1.3.1.md.

NEW: SCRIPT MODE
- One button in the Traffic Manager (F7 / F8), next to Help and Readme. Saved in settings.ini ([traffic] scriptMode, default off).
- In Script mode a car on its wheels on a drive lane takes the route's line as its plan (planenergy::ScriptFollow): no candidate
  search, no avoidance arcs, no recovery routine (back up, K-turn, turn to lane). Speed = the lane speed (bend and crest caps),
  0 under a lane hold or stop line, and one stop rule: the first point along the line ahead where its body would touch a scene
  solid limits the speed to sqrt(2 x brake x distance left). Edge bands are not obstacles (the path is taken as perfect). Hover
  lanes keep the normal planner with the sensors off. Trace: plan.script.
- Every vehicle's World takes only the hazard boxes you place, the known positions of other vehicles (each one's
  own box is its avoid distance) and people. Ray hits, the coarse map's band cells and kept boxes, the clear-path sweep, the
  look-ahead probe rays, the field slice and volume beams, contact inferences and the radar are all off. Floors still come from
  the map's measured floor, so cars keep to the road surface.
- Each time it is switched on, the Traffic Manager's own message panel says what it does and that Hazard boxes are how obstacles
  are marked. Meant for roads the normal autopilot does not handle well.
- Traces: OBS system=world.script (per car, on/off), f7.dock script_mode, routeedit.message why=script_mode_on.

FIXED: TRAFFIC CARS JUMPING IN VIEW (2026-11-22 18:38 run: 8 jumps of 22-158 m, 45-190 m from the player)
- HJ-1: a stopped car's physics body goes to sleep, so it was "not stepping" and the virtual actuator moved its reference along
  the route, leaving the body behind; when physics woke, the car snapped back to its body. A car whose physics body is in the live
  scene (model loaded) now stays physical; the rail's existing wake (WK-1) starts it from where it really is. Only a car with no
  physics body is carried virtually. Trace: traffic.actuator gate=body_in_scene_stays_physical_wake, to=physical_body_waking.
- HJ-2: the virtual actuator resumed from the route station of an earlier virtual stretch. It now re-seeds from the car's real
  position whenever they differ by more than 3 m. Trace: traffic.actuator event=virtual_reseed stale_m=...

- HJ-3 (2026-11-22 22:35 run: FF1014A1 carried 162 m while still Proving, snapped 159 m back at the hand-off): a traffic car whose
  census life is Born or Proving is never moved by an actuator. Trace: gate=birth_not_handed_off_no_carry, to=birth_proving.

SINCE 1.3.0 (main tree, 2026-11-21/22)
- Traffic population: background flow (every route position is a moving car), batched creation under the loading screen and the
  X panel, loading-screen arrival fill, the road check skipped under cover when it cannot answer yet; births treat every route the
  same, including hover-only routes; Prepare traffic says the real reason for zero positions.
- Actuator: one gate for every lane car (physical the moment the engine steps it, carried along its path otherwise), plus HJ-1/2.
- Autopilot: held recovery routine and K-turns for traffic cars, multi-point turns, queued cars wait; planner values generated from
  the car (stopping look-ahead from speed and braking, speed rungs, switch times, manoeuvre speeds, gear thresholds).
- Hazard boxes: a normal Traffic Manager item (Hazards tool); the planner, the rail and the virtual actuator plan round them or stop.
- The Traffic Manager's first-time message and Readme use its own panel (one pointer; E closes it).
- Saved Auto / Cruise resumes on load; covered adoption of cars under the loading screen.
- Performance passes 1 and 2 (per-minute cost line in player builds; near cars out of view plan at 10 Hz; live fill capped at
  3 ms a tick).
- The lab (factory and stock system) stays in the source, switched off.

UPDATE TOGETHER
Traffic Addon 0.4.x needs the 1.3.x core's fleet exports; with an older core it loads but creates no traffic and says so once.
Core 1.3.1 keeps API 13 and the read-only API 4, so published add-ons 0.3 / 0.3.1 still load.

BACKWARDS COMPATIBILITY
No save, route, traffic profile or plugin format change. One new settings.ini key ([traffic] scriptMode, default off); old
settings files read as before. The FOMOD writes it off (a fixed, non-menu switch like Radar). NewAtlantis.ini ships with LF line
endings (same text; earlier packages had CRLF).

RELEASE VERSUS TEST BUILDS
Player downloads use FVO_RELEASE=ON: routine diagnostics off, crash reporting kept.

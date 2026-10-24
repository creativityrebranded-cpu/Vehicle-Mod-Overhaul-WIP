# Flying Vehicle Overhaul 0.72.0

For Starfield 1.16.244 (Steam) and SFSE. Early work in progress.

Everything below is new since 0.71.0. None of it has been tested in game yet; it passes the mod's offline checks (29 test suites). Please send your log if something goes wrong.

## New: physical flight (experimental, off by default)
- **Physical VTOL** (Gameplay Options): your hovering vehicle flies on thrust and torque using the game's own mass and inertia, instead of having its speed set directly. Speed builds and coasts, banking needs more lift, and a hit can knock it into a spin.
- **Flight assist** (on by default): keeps physical flight stable, and now gives coordinated turns: turning with the camera banks the vehicle, and banking with A / D turns the nose into the curve. It levels out near the ground and when landing. Off: the vehicle keeps whatever momentum and spin it has.
- **Physical VTOL for AI pilots**: the autopilot, summons and NPC-driven vehicles can fly the same way. Turns bank, speed builds and coasts. A vehicle whose speed the game keeps cancelling falls back to the standard flight by itself.
- **Free roll** (Gameplay Options, off by default): your hover can roll all the way round and turns about its own axes.
- Physics now runs on the game's own simulation timestep, so flight behaves the same at any frame rate.

## New: camera and HUD
- **Camera roll**: in physical flight the camera banks with the vehicle (smoothed), in both first and third person.
- **Attitude instrument**: a horizon with a pitch ladder and a pitch / bank readout appears during manual physical flight.
- The instrument and its pitch / bank text are separate elements in the F7 editor ("VTOL attitude" and "VTOL attitude text"), each moved and resized on its own.

## New: controller support
- A full controller map that changes with what you are doing (on foot, driving, hovering, in the scanner, as a passenger). No button combinations.
- When the mod uses a controller button, the game's own action on that button is switched off there, so one press never does two things. Fire (RT) and aim (LT) are never taken.
- Analog sticks: how far you push the stick sets how much roll and thrust you get.
- Button prompts switch between keyboard and controller only when you press a button on the other device, so they no longer flicker.
- The mod's own section in the game's Controls menu lists every action and lets you rebind it.
- Your bindings.ini is upgraded to the new controller map once. Lines you edited yourself are kept.

## Changed controls
- **Hover on / off** now has its own key (shown on the hover button; L3 on a controller). Space double tap no longer toggles hover.
- **Throttle lock**: double tap Boost (was double tap W).
- **Occupied vehicle**: tap Activate = ride as a passenger at once; hold Activate = pull the driver out (was double tap).
- "Facing an occupied vehicle" is measured from the vehicle's side, within 3 m, instead of from its centre.
- Ram (hold T in the scanner) is keyboard-only for now.

## Fixes
- First-person vehicle view was not recognised by the mod, which kept the camera roll and HUD off and misread first-person look limits.

## Known issues
- Placed REV-8s can start 0.5 m inside the ground.
- Parked small vehicles can hop.
- AI route planning still assumes the standard flight's grip; with physical flight for AI pilots on, expect wide turns and overshoots.
- No atmospheric lift or stalls yet in physical flight.

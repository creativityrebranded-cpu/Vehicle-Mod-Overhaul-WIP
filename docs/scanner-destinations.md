# Named scanner destinations

When the scanner highlights an outpost or another map marker, its displayed location name identifies the aimed destination. The name appears beside the scanner target distance, in the marker confirmation, and stays attached to queued stops.

The implementation reads the active `RightInfoDisplay_mc` MapMarker panel and its `MapMarkerInfo_mc.description_mc.description_tf` first line on the Scaleform movie thread. The nearby-area name is separate and does not select a destination. Snapshot data expires after 200 ms and clears on menu destruction or save load.

Captured planet map records include their copied names. Matching uses name, viewing direction, and the scanner range to distinguish places with repeated names. A selected named destination takes priority over a nearer collision ray. Ordinary aiming still respects foreground hits.

If planet map coordinates are unavailable (for example, before opening the star map), the game scanner range projects an estimated position along the camera view. Estimated positions retain the `~` indicator, including after marker placement. This projection is not an exact reference position.

Validation: release DLL build and twelve offline destination-selection/range cases. Live scanner display and travel still require an in-game check.

## Route panel and trip controls

The HUD and scanner show the current destination name, travel status, distance, estimated time when making progress, and up to three queued stop names (plus the remaining count). Long names are shortened only in this display. Full names remain in the queue. The route group uses the game's native BasicButton components, with single-line status boxes matching the existing Cover and vehicle status boxes, and separate native key/label prompts for Retry, Skip and *T T: clear route*. No custom text-field background, font or colors are drawn. At most six information rows and three action prompts are visible; Retry appears for an unreachable trip.

The group starts in the upper left of the 1920-by-1080 HUD layout. Use F7, Page Up/Down to select Route, then arrows to move the group (Ctrl moves ten pixels); Delete resets it. F7 saves its shared HUD/scanner position to hoverui.ini. An example appears when the Route group is selected in the editor without an active route.

Skip stop normally uses `[` and Retry `;`. The mod claims free keys from the shared binding registry and shows the actual keys on the panel if those defaults are occupied. Clearing the route has no key of its own: it is a **double tap of T** (`targetinput.h`: the second press within 300 ms). Skip advances to the next queued stop; skipping the final stop clears the route. Clear cancels pending scanner dispatch, drops queued stops, and holds an active player or crew-driven vehicle at its present position and height. Hover remains active. Selecting a new scanner destination resumes travel. Patrol automatically requeues completed stops; manually skipped stops are not requeued. A failed marker placement retains the previous destination and queue.

Arrival requires fresh terrain and physics, no initial climb in progress, horizontal distance within the stop radius plus 2 m, height within 2.5 m of the marker's reachable hover level, and total three-dimensional speed at or below 1.5 m/s. Two-dimensional markers use terrain following. Marker-driven profiles use the same check before starting destination wait timers. Intermediate seamless waypoint handoffs also wait for the required height; they do not claim final arrival.

Statuses include Travelling, Waiting for terrain, Blocked, Finding another way, Adjusting height, Arrived, Orbiting, and Waiting for driver. ETA appears only while moving toward the current destination. Route metadata clears on save load and when an externally replaced marker invalidates the route.

Validation: release build and 17 offline arrival, status, and queue-transition tests. Live HUD placement, bindings, and vehicle hold behavior still require an in-game check.

## Current selection rules

Native range text is parsed with its m/km unit and grouping separators. Selected places display the scanner's own range; ordinary hit distances include height. Exiting a seat clears queued names and invalidates the old marker for travel, while leaving the game's map marker itself available for reference. Explicitly selecting it again re-arms the destination.

## T gestures

`targetinput.h` turns the key into three gestures. A **tap** is reported only after the 300 ms double-tap window, and it uses the selection captured at its own press. A **double tap** clears the route. A **hold** of 1000 ms or more arms a ram (`vehicleflight::ArmRam`), and releasing it selects nothing. A pending tap is cancelled on focus loss, when the scanner opens or closes, at seat exit and on save load. A tap then routes by state:
1. **Armed for a ram:** only the exact highlighted actor counts (`vehicleflight::RequestRam`). A tap on a place keeps the vehicle where it is.
2. **Cover on:** the place becomes the guard point (`vehicleflight::SetGuardPoint`).
3. **Otherwise:** the place becomes the destination, or a queued stop.

The highlighted actor comes from the scanner's public `MonoclePickRefData` provider: `BSUIDataManager.GetDataFromClient`, then `.data.uFormID` / `.uHandle`. `CurrentPickRefHandle` / `CurrentFormID` are private MonocleMenu traits and are not read. The actor must resolve from that reference, never from a nearby-actor search. A ram does not depend on the player's combat flag and logs why it was refused or ended. All added prompts use native widgets at 75% scale; there is no Auto button and no Auto Stop/Orbit/Patrol control.

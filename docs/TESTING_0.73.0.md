# Live verification for 0.73.0

The new command matrix has offline checks. These live cases remain unverified; no checkbox should be marked solely from a build or unit test.

| Device / context | Test | Expected |
|---|---|---|
| Keyboard/mouse only | Leave controller untouched; try V, K, J, G, ], all orders and HUD editing | Every gameplay command has a usable key; no NONE FREE dependency |
| Controller only | Leave keyboard/mouse untouched; drive, fly, issue scanner orders and edit HUD | All supported gameplay and HUD editor controls work |
| Shared gestures | Tap/hold/double Circle, L3, Triangle, D-left, D-right and View/Create in scanner | Only the selected gesture's command fires |
| Rebinding | Rebind scanner Triangle; leave flight Triangle unchanged | Climb remains bound and works |
| Rebinding | Rebind Retry's D-left tap; leave profile hold unchanged | Profile hold remains bound and works |
| Device isolation | Clear/reset one keyboard slot; then one controller slot | Opposite device and other command slots stay intact |
| Modal menus | Open pause, Controls, inventory, map or console while a button is held | No mod orders fire behind UI; native UI buttons remain usable |
| Transitions | Hold a button while opening/closing scanner or HUD editor, entering/exiting a seat, and loading a save | No unintended new-context command or stuck native action; fresh press works |
| Driver exit | Hover in first/third person and press native exit on each device | Mod lands first, then native exit runs |
| Ground driving | Rebind mod throttle/brake/turn/boost/vertical boost and test | Rebound controls reach native ground input; old mod mapping does not keep controlling the vehicle |
| Passenger | Board, exit, take wheel, look-to-steer and marker taxi on each device | Separate commands work; scanner Scan does not hijack an occupied vehicle |
| HUD editor | Select, move, resize, space, reset and save with each device | HUD changes persist; controller editor input does not drive/fire the vehicle |
| Placement mode | Record, delete, set summon point; turn mode off | Only placement commands run while mode is on; normal targeting returns when off |
| Legacy configuration | Start with layout 2/3 defaults and with edited lines | Backup exists, untouched defaults upgrade, edits are retained and can be reviewed |
| Flight / NPC regression | Standard hover, physical flight, summon, Auto, NPC combat and escort trips | Input rewrite does not change autonomous vehicle behavior |

Use `[log] verbose=1`; retain FlyingVehicleOverhaul.log and bindings.ini with the test result.

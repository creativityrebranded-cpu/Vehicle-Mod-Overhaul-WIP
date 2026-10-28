# Flying Vehicle Overhaul 0.73.0 (WIP)

The controls previously shared command handlers, lost unrelated contextual bindings during rebinding, and could leave keyboard defaults unbound. This update gives 52 commands/tool actions their own rows and evaluates their device, context and gesture centrally.

- Separate gear, descend, summon, escort recall, follow, crew driving, Auto, destination, route clear, guard, ram, focus, passenger and throttle-lock commands.
- Rebinding rejects real overlaps without stealing another command. Keyboard/controller slot clearing and defaults preserve the other device. Both settings interfaces use scoped slot operations.
- Deterministic keyboard defaults; automatic layout 4 migration with a previous-file backup and retention of edited rows.
- Controller and keyboard both enter the landing-before-driver-exit path. Configured ground motion, boost and vertical boost feed the native vehicle input submission.
- Modal UI suppresses mod gameplay input. Button ownership follows a press through its release across context changes. Scanner Activate no longer invokes occupied-vehicle seat commands.
- Controller HUD editing: scanner View/Create double tap enters; Circle/B saves and exits. Editing has its own controls, including motion, selection, size, spacing and reset.
- Placement editing and area summon points from the post-0.72.0 installed build are included.

The production command resolver's device/context/gesture matrix passes 388 checks. Full standalone build and suite results are recorded in the accompanying repair report. These are offline checks, not live gameplay certification. The new input hooks, menus, driver exit and HUD editor require a fresh in-game test.

## Files renamed

Every file now carries the mod's name, Flying Vehicle Overhaul (FVO), and says what it is:

- SeamlessVehicle.dll -> FlyingVehicleOverhaul.dll (the core mod)
- SeamlessPilots.esm -> FVO_NPCPilots.esm (NPC walking and boarding)
- FlyingVehicleOverhaul - Placements.esm -> FVO_CityPlacements.esm (optional parked vehicles)
- Vehicles - Real.esm -> FVO_ParkedVehiclesPatch.esm (optional Parked Vehicles patch)
- SeamlessVehicle.log -> FlyingVehicleOverhaul.log
- Logs\SeamlessVehicle\ -> Logs\FlyingVehicleOverhaul\ (settings folder), and seamless.ini -> settings.ini
- Internal names: the NPC quest and package editor IDs (FVO_NPCPilotsQuest, FVO_PilotGetIn0..5 ...), the plugins' author field, the placement-recording flag file (FlyingVehicleOverhaul.placement-recording; the old name still works)

When updating, delete the old DLL from Data\SFSE\Plugins (both together would load the mod twice) and swap the plugins in your load order. The first start moves the settings folder and renames seamless.ini for you, so settings, HUD layout and bindings stay. If the old placements plugin is still enabled, the mod still recognises it, and the log warns if both are enabled.

Known issues remain: partly buried REV-8 placements, parked small-vehicle hopping, momentum-aware AI planner tuning, and no atmospheric lift/stalls. Physical flight remains experimental and off by default.

## Contextual HUD follow-up

Replaced the broad command guide with short context-specific hints attached to original mod elements. Shared inputs regroup by the actual binding; native game controls gain no duplicate help. Target remains named information without an artificial binding. Placement replaces destination gestures, saved-spot and real-surface prerequisites gate authoring hints, and the editor previews only its selected hidden element. Existing positions and bindings are preserved. Offline checks pass; live appearance remains unverified.

## Automatic city placements

Startup city rebuild and stable spare-pool assignments completed. Validated persistent IDs, embedded recovery keys, retired tombstones, safe failure/backup ordering, native-use guards and idle-only reconciliation. Mandatory Python reference comparison and actual filesystem startup tests added. CityTraffic replaces the old CityPlacements option in the local package; live behavior remains unverified.

City vehicle startup fix: prepare the validated native vehicle listener before persistent city references register, and capture the pending-vehicle list from the start of data loading. This addresses the startup null-listener access violation recorded on 2026-10-24. Executable signatures and offline tests are checked; a fresh in-game launch and gameplay/save-load checks remain pending.

Stutter mitigation: vehicle discovery now resumes within dense cells with a 256-reference limit and a soft 0.5 ms budget, including the first sweep. Ring-editor beacon work is spread across frames; unchanged rings no longer reposition every beacon on routine refresh. Failed city-pilot seating attempts are limited and back off between retries. These changes preserve enabled mods and settings. Offline checks pass; the reduction in live stuttering still needs an in-game measurement.

Performance follow-up: navigation captures stable form IDs once per pass and resolves bounded slices without retaining raw references across frames. Registry maintenance visits one attached cell per frame. Native reference visitors stop at the requested limit. Native binding lookup is indexed and unchanged maps are retained; unchanged feature settings skip repeat INI reads. Gameplay log flushes are batched for up to 250 ms or 32 KiB, while startup and fault output remain immediate. Offline checks pass; live frame-time improvement remains unmeasured. Enabled mods, settings and texture quality are preserved.

Connection refactor: plugin records remain tied to their originating files through named local-form references, including light plugins. Pilot and parked-vehicle connections no longer guess load-order slots. Save/world reset drops cached pilot quests and diagnostics. Shared reference/cell access, explicit vehicle-service connections and city content definitions replace repeated setup and city-specific branches. Current plugin records, capacities, saved formats and settings are preserved. Offline checks pass; live gameplay remains to be verified.

Live-sampling performance follow-up: removed the synchronous main-thread reference mass visitor. Combat mass now uses a recent native chassis observation from a matching full-generation scene body, with the existing bounds estimate when no fresh observation exists. Load epochs reject prior-world samples. Physics step diagnostics use bounded counters without allocation or a registry mutex. The detailed log separates scene/occupant work, callback spacing, mod physics-hook time and native physics-step time. All active mods, visual settings and content are preserved. Live stutter reduction has not yet been measured.

Scheduling optimization: completed native updates admit full vehicle work, plus one startup/load bootstrap. If native pulses stop for 100 ms, a bounded 60 Hz clock fallback keeps on-foot gameplay active. Full work has a 240 Hz ceiling, and missed native updates coalesce without a catch-up queue. UI input samples retain one-shot events such as V until the next gameplay update consumes them. Recursive callbacks skip re-entry. Read-only RAM, process-I/O and GPU-budget diagnostics supplement callback timings. Offline checks pass; live V behavior and stutter reduction still require an in-game check.

## Latest local package — 25 October 2026 (unpublished)

Not yet published as a new numbered release.

- Added a FOMOD installer: the core is required; extra New Atlantis parked vehicles and the Parked Vehicles patch are independent, unchecked options.
- Added addon dependency checks against each ESM's actual masters. The renamed conversion patch is blocked while Vehicles - Real.esm remains installed.
- Added individual movement and sizing inside status-card and route groups. Group movement/resizing preserves child arrangements; foot and vehicle cards retain separate child layouts.
- Fixed the status card dropping its fourth escort row when other lines filled the old limit.
- Fixed the information panel moving twice under All vehicle HUD.
- Added an experimental ground-first route planner and native ground driving setting, off by default. Corrected vehicle bounds and retained avoidance offsets during ground guidance.
- Extended focus fire to hostile-driven vehicles, retaining the vehicle target through driver changes until it becomes empty, wrecked or unavailable.
- Removed player ram orders, deliberate AI ram/sideswipe attacks, and their setting/binding. Collision damage remains.
- Removed legacy City Traffic, Scheduled Traffic, the GHN road pool and GHN content from this package. Optional New Atlantis parked placements remain available.
- Reduced repeated work through bounded discovery, retry backoff, cached lookups, batched gameplay logging, coalesced updates and native chassis mass observations.
- Built and passed all 50 runtime test suites. FOMOD schema/archive and 8,748 dependency/selection combinations pass.
- Jemison CPU/FPS reports remain under investigation. A live performance improvement and native Vortex/MO2 installer UI behavior have not been confirmed.

Earlier City Traffic and ram notes above describe historical development; the removals in this section supersede them for the latest local FOMOD package.

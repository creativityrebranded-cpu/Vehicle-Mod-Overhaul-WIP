# Shared connection boundaries

`main_vehicle.cpp` composes the systems once for both core and standalone paths.

- `runtimeforms.h/.cpp`: stable plugin/local keys and ranges; one guarded native
  lookup and plugin-ID conversion adapter. Consumers do not invent load-order IDs.
- `runtimecells.h/.cpp`: bounded grid enumeration, explicit readiness policy and
  validated current-call reference views. Cursor consumers reacquire views.
- `vehiclelinks.h`: discovery/owned-vehicle/driver/use callbacks supplied to
  `world::SetServices`. Reconnection clears old snapshots; save reset keeps the
  connections. Callbacks must outlive the consumer and provide native refs.
- `cityprofile.h`: world matching, form ranges, frame conversion and ring anchors
  selected from content definitions. Only New Atlantis is shipped; another city
  also needs its native world/cell template and serializer master layout.
- `pilotlayout.h`, `parkedlayout.h`: named resource dependencies instead of
  heuristic load-order scans and prefix masks. Pilot-quest reset discards cached
  quests and pending diagnostics before resolving the loaded session again.
- `framework.h`: bounded work traversal and retry policy independent of content.
- `vcontract.h`: existing movement-goal/profile and seat-provider interfaces.

Native physics/scene readers retain their engine-specific implementation. The
core API ABI, six pilot aliases, 32 dedicated traffic vehicles, 64 spares, file
formats and serialized identities are preserved. Stable IDs may be retained;
raw form and cell-array pointers are borrowed for the current call/frame only.

`connections_test` exercises reusable content/service contracts. The production
adapters and snapshot lifecycle run in `runtimeperf_test` against isolated engine
fixtures. `citybuild_test` retains the mandatory independent byte comparison;
`cityfiles_test` checks actual startup filesystem behavior.

## Live physics observations and timing

`bodymass.h` supplies a fixed-capacity, nonwaiting observation cache. The physics callback reads current chassis inverse mass while native access is already valid, validates the full body generation and dynamic flag, and publishes only identity/mass/time/load epoch. The world sampler matches current scene bodies and accepts observations for at most two seconds. It never caches a physics pointer or invokes the synchronous reference mass visitor. Snapshot mass is the observed chassis mass, rather than the old reference-wide sum; the existing bounds estimate remains the fallback. The physical flight solver continues using its own current native mass/inertia.

`hookcost.h` separates mod-hook and native-step elapsed time and provides bounded per-instance step counters. Contention/capacity loss drops telemetry only; reports count skipped samples. `framepacing.h` measures callback spacing, including delays outside the previous measured task; these are not GPU/present measurements. Load reset clears pacing and invalidates mass epochs. Scene/occupant spike reports also name the slowest reference.

## Native-update admission

`updateschedule.h` keeps a lock-free atomic `Pulse` producer separate from the main-thread `Gate`. The standalone ground/ship-system hook publishes after its native update and probe work complete, including the no-TES return. The core path reuses the existing player-update callback and then publishes. No new hook, queue, worker, engine address, external ABI or user FPS cap is added.

The composition root reads the selected pulse on each raw task callback. One bootstrap is admitted after initialization/reset; thereafter a changed serial admits a full update, subject to a 240 Hz admission ceiling. Every missed serial is coalesced into the current state. There is no catch-up loop or accumulated update debt. Pulse publication remains monotonic across save resets; consumer reset permits one fresh bootstrap. Recursive task callbacks skip rather than re-enter the mutable update pipeline.

The standalone pulse can stop on foot. After 100 ms without a new pulse, clock fallback admits full updates at most 60 times per second so gameplay cannot freeze. Between full updates, settings, input/UI bindings and log housekeeping can run at most 60 times per second without scene discovery or AI/placement jobs. Input sampling retains triggered/started events until the next gameplay batch publishes them; that batch clears them after consumers run. Focus/menu, context, eligibility, binding and load changes discard stale events. Native flight physics, contact handling, camera/model update hooks and game simulation keep their existing paths. A slow consumer performs one current update rather than replaying old frames.

Diagnostics distinguish raw task poll frequency, admitted full update frequency, housekeeping, skipped polls, coalesced native serials and clock fallback. Task wall time includes waits and is not CPU utilization or rendered FPS. `vehicle_probe` separately reports native original versus probe/model elapsed time. `resources.cpp` samples process RAM, system available RAM, page-fault counts, process read throughput and DXGI memory budgets every two seconds while verbose logging is enabled. Fault counts include soft faults; process reads are not disk-only. Slow GPU diagnostics suspend further GPU queries. These readings do not establish a stall's cause. Admission does not bound engine-call duration, rendering, OS scheduling or total frame time. Existing bounded discovery/registry/scene/navigation/beacon/retry work units remain in place.

## Authored crowd population data

Traffic population planning uses authored crowd data from `Starfield.esm`: GBFM crowd-region components define density and conditional population proportions, with references to NPC_ templates and their PACK/FLST behavior definitions. No city-by-city census is required to identify those pools. The exploratory `npccensus.cpp` observer is excluded from the plugin target and update loop. Its source and temporal tests remain for reference. See `docs/NPC_SOURCE_DATA.md` for the source trace and limits.

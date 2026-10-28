# NPC activity census

Retired from the runtime plugin after the source-data trace. This document
describes the exploratory observer, which is no longer linked or called. Use
NPC_SOURCE_DATA.md for the authored crowd definitions; no city visit is needed
to identify those pools.

The read-only census supports the proposed population budget for airborne
traffic. It does not enable traffic or remove, move, board, reassign or disable
NPCs. With Detailed log enabled, it reports to FlyingVehicleOverhaul.log using
`NPC census` and `NPC census row` prefixes.

Remain in the same city area during normal play for about two minutes. Reports
show actor/base/cell/package IDs, package type, sample count, observed and quiet
time, distance moved and package changes. A review candidate needs a recognized
New Atlantis crowd base, valid package/position information, existing free-actor
checks, no combat and at least six samples over a quiet minute. Movement faster
than 0.4 m/s or a package change restarts the quiet interval. Missing data,
loading gaps longer than 15 seconds, cell/base changes and large coordinate
jumps restart evidence. Losing gameplay focus clears observations. Turning
Detailed log off stops and clears the census.

This is activity evidence for review. `replacement eligibility UNVERIFIED`
means exactly that: quest aliases, service/dialogue roles and overridden base
records have not been exhaustively audited. Standing still, a missing path or
an unknown package never establishes replacement eligibility. The known-base
table contains 60 exact CrowdActor_NA templates inspected in Starfield.esm with
no unique/essential/protected base flags or attached VMAD scripts. Runtime free
checks also exclude followers, dead or seated actors and existing mod jobs.

The census shares the runtime cell adapter and reads bounded slices of its
reference views without an engine-wide actor search or native path query. It
keeps at most 256 identity records, services at most once per 50 ms, checks at
most 128 references/eight cell advances and samples at most one actor per
slice. A soft 0.2 ms budget is checked between reference visits; it cannot bound
the duration of a native getter. Named crowd IDs resolve one per slice, then
cache until reset. No actor/reference pointer is retained in the observation
table. Cell pointer cursors are only identity anchors and are reacquired before
use. A summary appears every 30 seconds, with at most eight rows spread at one
per second through the existing buffered logger. Low-activity recognized crowd
rows are shown first, followed by other recognized or unclassified identities.

Counts describe recently observed attached-grid actors within 200 m, not the
complete city population or its original-game baseline. Persistent/interior or
unloaded populations may be absent. Sparse sampling can miss brief activity.
The next population/traffic implementation must establish a reviewed roster
and preserve one-for-one actor accounting before changing any crowd members.

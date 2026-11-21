#pragma once
#include <algorithm>
// Spawn-spot ground rule (2026-11-01). A spot is refused only when the hull's real bottom
// would be more than 0.5 m underground; otherwise the envelope test starts just above the
// ground, so road-level lane points (about a metre over the road) can be placed.
namespace spawnfloor {
// z: reference origin; bottom: hull bottom relative to the origin (negative); halfZ: envelope
// half height. Returns -1 (under the ground) or 0, with `floor` = where the envelope test starts.
inline int Check(double z, double bottom, double halfZ, bool groundKnown, double ground, double& floor) {
    floor = z - halfZ;
    if (!groundKnown) return 0;  // unknown ground is trusted to the authored lane, as before
    if (z + bottom < ground - 0.5) return -1;
    floor = std::max(floor, ground + 0.25);
    return 0;
}
}

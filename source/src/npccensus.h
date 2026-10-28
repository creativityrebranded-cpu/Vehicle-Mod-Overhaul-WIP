#pragma once

// Read-only, bounded activity observations. Never disables, moves or assigns an
// actor. Low activity is evidence for review, not permission to replace an NPC.
namespace npccensus {
void Tick();
void Reset();
}

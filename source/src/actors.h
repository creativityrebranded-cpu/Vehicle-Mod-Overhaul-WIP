#pragma once

#include <cstdint>

// SeamlessActors (module plugin): NPCs and creatures across world shifts.
// 0.1 measures only: before each shift it records every loaded actor's position; 3 s after, it reports actors that
// went back toward their old-frame coordinates (an AI path or package target still in the old frame), and actors
// that jumped more than walking allows. Planned (notes/modules/SeamlessActors.md): re-plan paths and offset combat /
// detection memory for actors near the player in the shift frame, the rest staggered.
namespace actors
{
	void BeforeShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void Sync(std::uintptr_t tes);
	void LoadReset();
}

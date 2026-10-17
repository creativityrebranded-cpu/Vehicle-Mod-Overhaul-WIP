#pragma once

#include <cstdint>

// SeamlessDistant (module plugin): distant terrain / LOD across world shifts (the "floating terrain").
// 0.1 measures only: after each shift, a class census of the scene branches the core did not move (what kinds of
// nodes they hold, how many, how far from the player), to find the distant terrain / LOD branch. With the user's `tdl`
// test this decides the fix: move that branch with the shift, or let the engine rebuild it (notes/modules/SeamlessDistant.md).
namespace distant
{
	void BeforeShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	// 0.5: hides distant LOD at each shift (the `tdl` flag) and shows it again once the world settles.
	void Sync(std::uintptr_t tes);
	// 0.6: a load happened: refresh once the world has settled.
	void LoadReset();
}

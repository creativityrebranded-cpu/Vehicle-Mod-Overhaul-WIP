#pragma once

#include <cstdint>

// SeamlessPacks (module plugin): building packs (BGSGroupedInstancedGeometry) across world shifts.
// 0.1 measures only: before each shift it records every grouped object's first instance record translation (+0x130
// vector -> record +0x50); after the shift it reports how many moved by the shift and how many stayed (= drawn in the
// old frame). Planned (see notes/modules/SeamlessPacks.md): rebuild stale packs by Disable + Enable of their owner
// reference, near first, far staggered.
namespace packs
{
	void BeforeShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void Sync(std::uintptr_t tes);
	void LoadReset();
}

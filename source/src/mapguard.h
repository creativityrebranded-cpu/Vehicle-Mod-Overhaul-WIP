#pragma once
// The local surface map's model build (2026-10-29 13:20 crash, the user: "it froze then crashed" in New Atlantis): the game's
// StarMap::SurfaceMapModelLoader collects a RefModel for every reference of the world's persistent cell and the loaded cells
// inside the map's area (FUN_141426240) and, on a worker thread, attaches one child per RefModel to a new NiNode
// (FUN_141426FC0 -> NiNode::AttachChild FUN_142BD15E0). The node's child array counts in 16 bits: at 65 535 children the
// count wraps, the array is resized to 0 and the next write goes through a null pointer (read of 0x7FFF8 at
// Starfield.exe+0x2BD3C05). Install patches the build task's run slot (vtable 0x144C54E38 slot 2, FUN_14142FAE0): past
// kMaxModels the list is shortened for the build and given back afterwards, and the count is logged with the persistent
// cell's contents, so the cause of a list that long shows in the log.
#include <cstdint>
namespace mapguard
{
	bool Install();  // main thread, once
	// Main thread, every update: logs the player's persistent cell (reference count, run-time ones, the most common bases)
	// every 60 s and when the build saw a long list.
	// Also removes the REV-8's leftover wheel dust from the persistent cell (mapguard.cpp). Skipped while `loading`.
	void Tick(bool loading);
	// Any thread: `base` (a form's base object pointer) is one of the REV-8's wheel-dust bases (resolved on the first Tick).
	bool IsDustBase(std::uintptr_t base);
	// Any thread: a wheel-dust puff is being made persistent (loadedarea's persistence hook; allowed). True: log its callers
	// (the first few during play; a save load's are only counted).
	bool NoteDustPersist();
}

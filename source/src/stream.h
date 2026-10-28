#pragma once

#include <cstdint>

// SeamlessStream (module plugin): loading ahead of the player so the world is in memory before it is seen.
// 0.1: sets the core's load-ahead lead to kLeadSeconds and reports, every 10 s while moving faster than 15 m/s, the
// speed and how many grid cells still wait for collision. Planned (notes/modules/SeamlessStream.md): lead past sight
// in every travel direction, a larger loaded radius at speed, and readiness reports for the shift point.
namespace stream
{
	void Start();
	void Sync(std::uintptr_t tes);
}

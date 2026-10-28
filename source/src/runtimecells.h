#pragma once
#include <cstdint>
namespace runtimecells {
// Main-thread adapters; raw views are for the current call only. A resumed
// traversal keeps its cursor/IDs, then obtains a fresh view on the next frame.
unsigned GridSize(unsigned cap=64);
enum class Policy {AttachedCell,ReadyGridEntry};
std::uintptr_t Cell(unsigned index,Policy policy);
std::uintptr_t Attached(unsigned index);
bool References(std::uintptr_t cell,std::uintptr_t*& data,std::uint32_t& count);
void Reset();
}

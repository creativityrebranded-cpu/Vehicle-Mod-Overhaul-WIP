#pragma once
// Relocation consumers, core wiring (2026-11-20). Pure logic: relocationrebase.h. This header gives the core's consumers (Plan, World, Keep-in-sight,
// and the Route / spot test call sites) ONE way to ask "did a seated move of this vehicle just apply?".
//   Poll(vehicle)   game thread only: refreshes the board from the Coordinator's published status, returns the latest applied move (or an
//                   empty Event). In FVO_RELEASE builds the doer does not exist: Poll is an inline empty answer and costs nothing.
//   Peek(vehicle)   any thread: the board's latest applied move without asking the Coordinator.
// The source is the core's own diagnostic export FVO_TrafficRelocationV1 (npcvehicles.cpp), which reads Coordinator::LastFor / Relocating.
#include "relocationrebase.h"

#if !defined(FVO_RELEASE) && !defined(FVO_RELOC_NO_SOURCE)
extern "C" int FVO_TrafficRelocationV1(unsigned vehicle, FvoRelocationStatusV1* out);   // defined in npcvehicles.cpp (same DLL)
#endif

namespace relocconsumers {
inline relocrebase::Board& Board() { static relocrebase::Board b; return b; }
inline relocrebase::SourceFn Source() {
#if !defined(FVO_RELEASE) && !defined(FVO_RELOC_NO_SOURCE)
    return &FVO_TrafficRelocationV1;
#else
    return nullptr;
#endif
}
inline relocrebase::Event Peek(std::uint32_t vehicle) {
    if (!Source()) return {};   // a player build has no doer: no receipt, no lock
    relocrebase::Event ev; if (!Board().Latest(vehicle, ev)) ev = {}; return ev;
}
inline relocrebase::Event Poll(std::uint32_t vehicle) {
    if (const auto src = Source()) Board().Refresh(vehicle, src);
    return Peek(vehicle);
}
}  // namespace relocconsumers

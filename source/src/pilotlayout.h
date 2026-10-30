#pragma once
#include "runtimeforms.h"
#include "fvolimits.h"
namespace pilotlayout {
inline constexpr int kSlots=fvolimits::kVehicles;  // FVO_NPCPilots.esm (make_pilots_esm.py PILOTS) must have this many
inline constexpr runtimeforms::Key kBoardQuest{"FVO_NPCPilots.esm",0x800};
inline constexpr runtimeforms::Key kActivateQuest{"FVO_NPCPilots.esm",0x810};
inline constexpr runtimeforms::Key kMapMessageSeen{"FVO_NPCPilots.esm",0xD00};  // global FVO_TrafficManagerSeen
// Get-in packages: slots 0..11 at 0x801.., 12..63 at 0x900.. (kBoardPackagesMore), 64.. at 0xB00.. (kBoardPackagesRest).
static_assert(kSlots>=64,"FVO_NPCPilots.esm's first three blocks hold 64 slots");
inline constexpr runtimeforms::Range kBoardPackages{"FVO_NPCPilots.esm",0x801,12};
inline constexpr runtimeforms::Range kBoardPackagesMore{"FVO_NPCPilots.esm",0x900,52};
inline constexpr runtimeforms::Range kBoardPackagesRest{"FVO_NPCPilots.esm",0xB00,kSlots-64};
}

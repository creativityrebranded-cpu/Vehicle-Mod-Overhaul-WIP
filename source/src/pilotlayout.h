#pragma once
#include "runtimeforms.h"
namespace pilotlayout {
inline constexpr int kSlots=6;
inline constexpr runtimeforms::Key kBoardQuest{"FVO_NPCPilots.esm",0x800};
inline constexpr runtimeforms::Key kActivateQuest{"FVO_NPCPilots.esm",0x810};
inline constexpr runtimeforms::Range kBoardPackages{"FVO_NPCPilots.esm",0x801,kSlots};
}

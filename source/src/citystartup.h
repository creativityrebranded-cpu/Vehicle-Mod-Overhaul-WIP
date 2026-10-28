#pragma once
#include "citybuild.h"

// At every game start, before the game reads its plugins (SFSE loads this DLL first): rebuilds Data\FVO_CityTraffic.esm from the
// user's recordings when they changed (citybuild.h). Only when the plugin is installed; the previous file is kept as
// Logs\FlyingVehicleOverhaul\citybuild-previous.esm, and nothing is replaced when anything fails.
namespace citystartup
{
	struct Result {bool ok=false,changed=false;std::string error;std::map<std::uint64_t,std::uint32_t> active;std::size_t parked=0,recorded=0,removed=0;};
	// Actual startup implementation, also used by the offline tool against an isolated destination.
	Result RebuildAt(const std::wstring& plugin,const std::wstring& settings);
	const Result& Snapshot(); // immutable after startup, including a verified fallback on a failed rebuild
	void Run();
}

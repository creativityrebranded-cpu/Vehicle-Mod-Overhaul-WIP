#pragma once

#include "core_api.h"

#include <cstdint>

// Core plugin: the API table and the callback dispatch (see core_api.h).
namespace coreapi
{
	const seamless::CoreApi& Table();
	bool InstallMouseHook();

	void FireFrame();
	void FireSync(std::uintptr_t tes);
	void FireBeforeSave();
	void FireAfterSave();
	void FireLoadReset();
	void FireZoneEnter(std::uint64_t key, float x, float y, float z, float vx, float vy, bool start);
	void FirePlayerUpdate(std::uintptr_t player);
	bool AnyBusy();
	unsigned long long ContentPostSteps();
	void FireBeforeShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void FireShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
}

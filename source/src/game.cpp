#include "game.h"

#include "addresses.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>

namespace game
{
	std::uintptr_t Base()
	{
		static const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		return base;
	}

	namespace
	{
		// Plain function so structured exception handling has no C++ objects to unwind.
		bool GuardedCopy(void* dst, std::uintptr_t src, std::size_t size)
		{
			__try {
				std::memcpy(dst, reinterpret_cast<const void*>(src), size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	template <class T>
	std::optional<T> SafeRead(std::uintptr_t address)
	{
		T value{};
		if (!GuardedCopy(&value, address, sizeof(T))) {
			return std::nullopt;
		}
		return value;
	}

	template std::optional<std::uintptr_t> SafeRead<std::uintptr_t>(std::uintptr_t);
	template std::optional<std::uint32_t> SafeRead<std::uint32_t>(std::uintptr_t);
	template std::optional<std::int32_t> SafeRead<std::int32_t>(std::uintptr_t);
	template std::optional<std::uint8_t> SafeRead<std::uint8_t>(std::uintptr_t);
	template std::optional<Vec3> SafeRead<Vec3>(std::uintptr_t);

	bool VerifySignatures()
	{
		bool ok = true;
		for (const auto& sig : addr::kSignatures) {
			const auto* code = reinterpret_cast<const std::uint8_t*>(Base() + sig.rva);
			if (std::memcmp(code, sig.bytes, sizeof(sig.bytes)) != 0) {
				logger::Write("Signature mismatch: %s at RVA 0x%08X", sig.name, sig.rva);
				ok = false;
			}
		}
		return ok;
	}

	namespace
	{
		// Returns the object pointer stored at a global, if it is non-null and has the expected vtable.
		std::uintptr_t CheckedObject(std::uint32_t globalRva, std::uint32_t vtblRva)
		{
			const auto obj = SafeRead<std::uintptr_t>(Base() + globalRva);
			if (!obj || *obj == 0) {
				return 0;
			}
			const auto vtbl = SafeRead<std::uintptr_t>(*obj);
			return (vtbl && *vtbl == Base() + vtblRva) ? *obj : 0;
		}
	}

	PlanetState ReadPlanetState()
	{
		PlanetState s;
		if (const auto player = CheckedObject(addr::kPlayerCharacterPtr, addr::kPlayerCharacterVtbl)) {
			if (const auto pos = SafeRead<Vec3>(player + off::kRefPosition)) {
				s.havePlayer = true;
				s.playerPos = *pos;
			}
		}
		if (const auto mgr = CheckedObject(addr::kPlanetManagerPtr, addr::kPlanetManagerVtbl)) {
			const auto state = SafeRead<std::uint8_t>(mgr + off::kMgrState);
			const auto cx = SafeRead<std::int32_t>(mgr + off::kMgrGridCentre);
			const auto cy = SafeRead<std::int32_t>(mgr + off::kMgrGridCentre + 4);
			const auto dx = SafeRead<std::int32_t>(mgr + off::kMgrShiftDelta);
			const auto dy = SafeRead<std::int32_t>(mgr + off::kMgrShiftDelta + 4);
			if (state && cx && cy && dx && dy) {
				s.haveManager = true;
				s.managerState = *state;
				s.gridCentreX = *cx;
				s.gridCentreY = *cy;
				s.shiftDeltaX = *dx;
				s.shiftDeltaY = *dy;
			}
		}
		return s;
	}

	Limits ReadLimits()
	{
		const auto b = Base();
		Limits l{};
		l.borderEnabled = SafeRead<std::uint8_t>(b + addr::kBorderRegionsEnabled).value_or(1) != 0;
		l.maxPlayableCoord = SafeRead<std::uint32_t>(b + addr::kMaximumPlayableAreaWorldCoord).value_or(0);
		l.blockSize = SafeRead<std::uint32_t>(b + addr::kBlockSize).value_or(0);
		l.mergeZone = SafeRead<std::uint32_t>(b + addr::kBlockMergeZoneSize).value_or(0);
		l.terrainShiftCells = SafeRead<std::uint32_t>(b + addr::kTerrainShiftThresholdInCells).value_or(0);
		l.havokCellExtents = SafeRead<std::int32_t>(b + addr::kNumCellWorldExtents).value_or(0);

		// Same formula as the game's border check (FUN_141a33cb0).
		const std::uint32_t cells = std::min(l.maxPlayableCoord * 2 / 100, (l.blockSize + l.mergeZone) * 8);
		l.borderMetres = static_cast<float>(cells / 2) * off::kCellSize - 50.0f;
		l.physicsMetres = static_cast<float>(l.havokCellExtents) * 50.0f;
		return l;
	}
}

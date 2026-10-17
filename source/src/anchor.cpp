#include "anchor.h"

#include "addresses.h"
#include "game.h"

namespace anchor
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t offset) { return *reinterpret_cast<T*>(p + offset); }
		std::uintptr_t Global(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		// TES grid entries wrap the cell; same validity test as the engine's own grid loop (FUN_140589e60).
		std::uintptr_t GridCell(std::uintptr_t entry)
		{
			if (!entry) {
				return 0;
			}
			const auto state = At<std::uint32_t>(entry, off::kGridEntryState);
			if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) {
				return 0;
			}
			return At<std::uintptr_t>(entry, off::kGridEntryCell);
		}
	}

	bool InShipInterior(std::uintptr_t ref)
	{
		const auto cell = ref ? At<std::uintptr_t>(ref, off::kRefParentCell) : 0;
		if (!cell || !(At<std::uint8_t>(cell, off::kCellFlags) & 1)) {
			return false;
		}
		const auto data = At<std::uintptr_t>(cell, off::kCellExterior);
		return data && At<std::uint8_t>(data, off::kCellInteriorKind) == 1;
	}

	std::uintptr_t ShipOf(std::uintptr_t ref)
	{
		return ref ? EngineFn<std::uintptr_t (*)(std::uintptr_t, char, std::uintptr_t, std::uintptr_t)>(addr::kShipOfRef)(ref, 1, 0, 0) : 0;
	}

	Anchor Current()
	{
		const auto player = Global(addr::kPlayerCharacterPtr);
		const auto playerCell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
		if (!playerCell) {
			return {};
		}
		if (!(At<std::uint8_t>(playerCell, off::kCellFlags) & 1)) {
			return { player, playerCell, false };
		}
		if (InShipInterior(player)) {
			const auto ship = ShipOf(player);
			const auto shipCell = ship ? At<std::uintptr_t>(ship, off::kRefParentCell) : 0;
			if (shipCell && !(At<std::uint8_t>(shipCell, off::kCellFlags) & 1) && At<std::uintptr_t>(shipCell, off::kCellWorldspace)) {
				return { ship, shipCell, true };
			}
		}
		return {};
	}

	bool WorldSettled()
	{
		const auto tes = Global(addr::kTESPtr);
		const auto mgr = Global(addr::kPlanetManagerPtr);
		const auto a = Current();
		if (!tes || !mgr || !a.ref || At<std::uint8_t>(mgr, off::kMgrState) != 2 || At<std::uint8_t>(a.cell, off::kCellState) != 10) {
			return false;
		}
		const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
		if (!entries || count == 0) {
			return false;
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto cell = GridCell(entries[i]);
			if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) {
				return false;
			}
		}
		return true;
	}

	bool DemandReady()
	{
		const auto tes = Global(addr::kTESPtr);
		const auto mgr = Global(addr::kPlanetManagerPtr);
		const auto a = Current();
		if (!tes || !mgr || !a.ref || At<std::uint8_t>(mgr, off::kMgrState) != 2 || At<std::uint8_t>(a.cell, off::kCellState) != 10) {
			return false;
		}
		const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
		// Runtime game setting val_uGridsToDemand_General (notes/settings-map.tsv).
		// Unknown configurations retain the existing strict behavior.
		constexpr std::uintptr_t kGridsToDemand = 0x05E8A668;
		const auto demand = *reinterpret_cast<const std::uint32_t*>(game::Base() + kGridsToDemand);
		if (!entries || count != 25 || demand != 3) return WorldSettled();
		const int gx = At<std::int32_t>(tes, off::kTESGridCentre);
		const int gy = At<std::int32_t>(tes, off::kTESGridCentre + 4);
		const auto world = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
		if (!world) return false;
		std::uint16_t readyMask = 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto cell = GridCell(entries[i]);
			if (!cell || At<std::uintptr_t>(cell, off::kCellWorldspace) != world) continue;
			const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
			if (!ext) continue;
			const int x = At<std::int32_t>(ext, 0), y = At<std::int32_t>(ext, 4);
			if (x < gx - 1 || x > gx + 1 || y < gy - 1 || y > gy + 1) continue;
			if (At<std::uint8_t>(cell, off::kCellState) != 10) return false;
			const unsigned bit = static_cast<unsigned>((y - gy + 1) * 3 + (x - gx + 1));
			readyMask |= static_cast<std::uint16_t>(1u << bit);
		}
		return readyMask == 0x1FF;
	}
}

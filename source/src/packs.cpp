#include "packs.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "telemetry.h"

#include <Windows.h>

#include <cmath>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace packs
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }

		constexpr std::uint32_t kRecordsBegin = 0x130, kRecordsEnd = 0x138;  // vector of 0x70-byte instance records
		constexpr std::uint32_t kRecordTranslate = 0x50;

		struct Rec { float x, y; std::size_t count; };
		std::unordered_map<std::uintptr_t, Rec> g_before;
		std::int32_t g_cx = 0, g_cy = 0;
		ULONGLONG g_checkAt = 0;

		bool FirstRecord(std::uintptr_t grouped, float& x, float& y, std::size_t& count)
		{
			__try {
				const auto b = At<std::uintptr_t*>(grouped, kRecordsBegin), e = At<std::uintptr_t*>(grouped, kRecordsEnd);
				if (!b || e <= b || !*b) return false;
				count = std::size_t(e - b);
				const float* t = reinterpret_cast<const float*>(*b + kRecordTranslate);
				x = t[0];
				y = t[1];
				return std::isfinite(x) && std::isfinite(y);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void Measure(const char* when)
		{
			unsigned moved = 0, stayed = 0, other = 0, gone = 0;
			const float ex = float(g_cx) * off::kCellSize, ey = float(g_cy) * off::kCellSize;
			std::vector<std::uintptr_t> now;
			modscene::FindClass(modscene::SceneRoot(), "BGSGroupedInstancedGeometry", now);
			std::unordered_map<std::uintptr_t, bool> present;
			for (const auto n : now) present[n] = true;
			for (const auto& [node, r] : g_before) {
				float x, y;
				std::size_t c;
				if (!present.count(node) || !FirstRecord(node, x, y, c)) {
					++gone;
					continue;
				}
				if (std::hypot(x + ex - r.x, y + ey - r.y) < 0.5f) ++moved;
				else if (std::hypot(x - r.x, y - r.y) < 0.5f) ++stayed;
				else ++other;
			}
			// 0.2: the grouped objects present now that were not there before the shift (rebuilt by the engine?): where do their
			// records sit, near the player in the new frame (correct) or near where the player was in the old frame?
			const auto player = *reinterpret_cast<const std::uintptr_t*>(game::Base() + addr::kPlayerCharacterPtr);
			unsigned fresh = 0, freshNew = 0, freshOld = 0;
			if (player) {
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				for (const auto n : now) {
					if (g_before.count(n)) continue;
					float x, y;
					std::size_t c;
					if (!FirstRecord(n, x, y, c)) continue;
					++fresh;
					const float dNew = std::hypot(x - pp.x, y - pp.y), dOld = std::hypot(x - (pp.x + ex), y - (pp.y + ey));
					if (dNew < dOld) ++freshNew; else ++freshOld;
				}
			}
			logger::Write("Packs: %s shift (%d, %d): %zu grouped objects before; instance records moved with the shift %u, stayed in the old frame %u, changed otherwise %u, gone %u; new since the shift %u (%u placed in the new frame, %u in the old)",
				when, g_cx, g_cy, g_before.size(), moved, stayed, other, gone, fresh, freshNew, freshOld);
			telemetry::Record("packs", "\"when\":\"%s\",\"before\":%zu,\"moved\":%u,\"stayed\":%u,\"other\":%u,\"gone\":%u", when, g_before.size(), moved, stayed, other, gone);
		}
	}

	namespace
	{
		// 0.3: who owns a grouped object? Walk its parents and look for a loaded reference's 3D root (the owner to
		// Disable + Enable for a rebuild). Logged once per session for up to 40 grouped objects.
		bool g_ownersLogged = false;

		void LogOwners(const std::vector<std::uintptr_t>& nodes)
		{
			std::unordered_map<std::uintptr_t, std::uintptr_t> rootToRef;
			for (const auto r : modscene::LoadedRefs()) {
				const auto loaded = At<std::uintptr_t>(r, off::kRefLoadedData);
				const auto n = loaded ? At<std::uintptr_t>(loaded, off::kLoadedData3D) : 0;
				if (n) rootToRef[n] = r;
			}
			unsigned withOwner = 0, without = 0, shown = 0;
			for (std::size_t i = 0; i < nodes.size() && i < 40; ++i) {
				std::uintptr_t owner = 0;
				std::string chain;
				auto n = nodes[i];
				for (int d = 0; n && d < 16; ++d) {
					if (d) chain += " < ";
					chain += modscene::RttiName(n);
					if (const auto it = rootToRef.find(n); it != rootToRef.end()) {
						owner = it->second;
						break;
					}
					n = At<std::uintptr_t>(n, off::kNodeParent);
				}
				++(owner ? withOwner : without);
				if (shown < 6) {
					++shown;
					logger::Write("Packs:   grouped %p: owner %s%08X (%s); chain %s", reinterpret_cast<void*>(nodes[i]), owner ? "" : "none ",
						owner ? At<std::uint32_t>(owner, off::kFormId) : 0u, owner ? modscene::RttiName(owner) : "-", chain.c_str());
				}
			}
			logger::Write("Packs: owners: %u of the first %zu grouped objects hang under a loaded reference's 3D, %u do not (%zu references with 3D)",
				withOwner, std::min<std::size_t>(nodes.size(), 40), without, rootToRef.size());
		}
	}

	void BeforeShift(std::int32_t, std::int32_t, std::uintptr_t)
	{
		g_before.clear();
		std::vector<std::uintptr_t> nodes;
		modscene::FindClass(modscene::SceneRoot(), "BGSGroupedInstancedGeometry", nodes);
		if (!g_ownersLogged && !nodes.empty()) {
			g_ownersLogged = true;
			LogOwners(nodes);
		}
		for (const auto n : nodes) {
			Rec r{};
			if (FirstRecord(n, r.x, r.y, r.count)) {
				g_before[n] = r;
			}
		}
	}

	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t)
	{
		g_cx = cx;
		g_cy = cy;
		Measure("right after");
		g_checkAt = GetTickCount64() + 3000;
	}

	void Sync(std::uintptr_t)
	{
		if (g_checkAt && GetTickCount64() >= g_checkAt) {
			g_checkAt = 0;
			Measure("3 s after");
		}
	}

	void LoadReset()
	{
		g_before.clear();
		g_checkAt = 0;
	}
}

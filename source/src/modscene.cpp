#include "modscene.h"

#include "addresses.h"
#include "game.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>

namespace modscene
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		std::uintptr_t AsNode(std::uintptr_t n)
		{
			__try {
				auto** vtbl = *reinterpret_cast<void***>(n);
				return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(n);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		struct RefCollector
		{
			void* const* vtbl;
			std::vector<std::uintptr_t>* out;
			unsigned limit;
			static void Noop(RefCollector*) {}
			static int Invoke(RefCollector* self, std::uintptr_t* refPtr)
			{
				if (refPtr && *refPtr && self->out->size() < self->limit) {
					self->out->push_back(*refPtr);
				}
				return 1;
			}
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop),
				reinterpret_cast<void*>(&Invoke), reinterpret_cast<void*>(&Noop) };
		};

		void Walk(std::uintptr_t n, const char* cls, std::vector<std::uintptr_t>& out, unsigned limit, int depth, unsigned& visited)
		{
			if (!n || depth > 40 || out.size() >= limit || ++visited > 400000) {
				return;
			}
			if (std::strstr(RttiName(n), cls)) {
				out.push_back(n);
			}
			const auto node = AsNode(n);
			if (!node) {
				return;
			}
			const auto children = At<std::uintptr_t*>(node, off::kNodeChildren);
			const auto count = At<std::uint16_t>(node, off::kNodeChildCount);
			for (std::uint16_t i = 0; children && i < count; ++i) {
				Walk(children[i], cls, out, limit, depth + 1, visited);
			}
		}
	}

	const char* RttiName(std::uintptr_t obj)
	{
		__try {
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(obj);
			const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
			const auto td = game::Base() + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
			return reinterpret_cast<const char*>(td + 0x10);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return "?";
		}
	}

	std::uintptr_t SceneRoot()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto loaded = player ? At<std::uintptr_t>(player, off::kRefLoadedData) : 0;
		auto n = loaded ? At<std::uintptr_t>(loaded, off::kLoadedData3D) : 0;
		for (int d = 0; n && d < 64; ++d) {
			const auto parent = At<std::uintptr_t>(n, off::kNodeParent);
			if (!parent) {
				return n;
			}
			n = parent;
		}
		return 0;
	}

	void FindClass(std::uintptr_t root, const char* cls, std::vector<std::uintptr_t>& out, unsigned limit)
	{
		unsigned visited = 0;
		Walk(root, cls, out, limit, 0, visited);
	}

	std::vector<std::uintptr_t> LoadedRefs(unsigned limit)
	{
		std::vector<std::uintptr_t> out;
		const auto tes = Ptr(addr::kTESPtr);
		if (!tes) {
			return out;
		}
		const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = std::min<std::uint32_t>(At<std::uint32_t>(tes, off::kTESGridCount), 64);
		for (std::uint32_t i = 0; entries && i < count; ++i) {
			if (!entries[i]) continue;
			const auto cell = At<std::uintptr_t>(entries[i], off::kGridEntryCell);
			if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) continue;
			RefCollector cb{ RefCollector::kVtbl, &out, limit };
			EngineFn<int (*)(std::uintptr_t, RefCollector*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
		}
		return out;
	}

	bool IsActor(std::uintptr_t ref)
	{
		const char* n = RttiName(ref);
		return std::strstr(n, "Actor@@") != nullptr || std::strstr(n, "PlayerCharacter@@") != nullptr;
	}
}

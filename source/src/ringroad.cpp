#include "ringroad.h"
#include "runtimeforms.h"
#include "addresses.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ringroad
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T> T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }

		constexpr std::uint32_t kFormId = 0x28, kFormFlags = 0x20, kDisabledFlag = 0x800, kBaseForm = 0x98;
		constexpr ULONGLONG kSettleMs = 10000;
		ULONGLONG g_resetAt = 0;
		bool g_cleaned = false;

		// 1 disabled now, 0 not a beacon here (another save's id) or already hidden, -1 fault. No C++ objects in this frame.
		int DisableBeacon(std::uint32_t id, uptr base)
		{
			__try {
				const uptr ref = id ? runtimeforms::LookupChecked(id) : 0;
				if (!ref || At<std::uint32_t>(ref, kFormId) != id || !base || At<uptr>(ref, kBaseForm) != base) return 0;
				if (At<std::uint32_t>(ref, kFormFlags) & kDisabledFlag) return 0;
				reinterpret_cast<void (*)(uptr, char)>(game::Base() + addr::kDisableReference)(ref, 0);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// Every "beacons=" id in every world section of ringroad.ini.
		std::vector<std::uint32_t> ListedBeacons()
		{
			std::vector<std::uint32_t> ids;
			const std::wstring ini = fvopaths::File(L"ringroad.ini");
			if (ini.empty()) return ids;
			std::vector<wchar_t> names(8192, 0);
			GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), ini.c_str());
			for (const wchar_t* s = names.data(); *s; s += wcslen(s) + 1) {
				wchar_t w[2048] = {};
				GetPrivateProfileStringW(s, L"beacons", L"", w, 2048, ini.c_str());
				std::string text;
				for (const wchar_t* c = w; *c; ++c) text.push_back(*c < 0x80 ? char(*c) : ' ');
				std::size_t at = 0; unsigned id = 0; int used = 0;
				while (std::sscanf(text.c_str() + at, "%x%n", &id, &used) == 1) { ids.push_back(id); at += used; }
			}
			return ids;
		}
	}

	void Reset() { g_resetAt = GetTickCount64(); g_cleaned = false; }

	void Tick()
	{
		if (g_cleaned) return;
		const ULONGLONG now = GetTickCount64();
		if (!g_resetAt) g_resetAt = now;
		if (now - g_resetAt < kSettleMs || !Ptr(addr::kPlayerCharacterPtr)) return;
		g_cleaned = true;
		const uptr base = Ptr(addr::kBeaconBaseObject);
		unsigned disabled = 0, faults = 0;
		const auto ids = ListedBeacons();
		for (const auto id : ids) {
			const int r = DisableBeacon(id, base);
			if (r > 0) ++disabled; else if (r < 0) ++faults;
		}
		if (disabled || faults) logger::Write("Ring road: removed beacons: %u disabled, %u unreadable (of %zu listed)", disabled, faults, ids.size());
	}
}

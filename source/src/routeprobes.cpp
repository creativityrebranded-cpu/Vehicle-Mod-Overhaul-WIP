#include "routeprobes.h"
#include "addresses.h"
#include "features.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "placementenable.h"
#include "runtimeforms.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Placement, move and show/hide as the old ring editor did them (notes/ring-beacons-removed-20261025/before/ringroad.cpp),
// for any list of points.
namespace routeprobes
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T> T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class F> F EngineFn(std::uint32_t rva) { return reinterpret_cast<F>(game::Base() + rva); }
		constexpr std::uint32_t kFormId = 0x28, kFormFlags = 0x20, kDisabledFlag = 0x800, kBaseForm = 0x98;
		constexpr int kMax = 200;            // probes per world at most
		constexpr int kPerFrame = 2;         // placed or moved per frame (spread over frames: no hitch)

		std::mutex g_lock;
		std::string g_wantWorld;
		std::vector<Marker> g_want;
		unsigned g_wantSerial = 0;

		std::string g_world;                 // the world the probes below belong to
		std::vector<std::uint32_t> g_ids;    // probe form ids (routeprobes.ini, one line per world)
		std::vector<Marker> g_now;
		unsigned g_serial = ~0u;
		int g_cursor = 0;
		bool g_shown = false, g_unsaved = false;
		int g_made = 0, g_moved = 0;
		ULONGLONG g_doneAt = 0;

		uptr Lookup(std::uint32_t id)
		{
			if (!id) return 0;
			__try {
				const uptr f = runtimeforms::LookupChecked(id);
				return f && At<std::uint32_t>(f, kFormId) == id ? f : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		uptr RefFromHandle(std::uint32_t handle)
		{
			uptr ref = 0; uptr* out = &ref;
			struct { uptr vtbl; uptr** capture; } lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			__try { EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
			return ref;
		}
		std::uint32_t Place(uptr base, uptr cell, uptr ws, const float pos[3], float scale)
		{
			const uptr data = Ptr(addr::kDataHandlerPtr);
			if (!base || !cell || !ws || !data) return 0;
			alignas(16) std::uint8_t d[0xC0] = {};
			const auto dp = reinterpret_cast<uptr>(d);
			At<uptr>(dp, 0x00) = game::Base() + addr::kNewRefrDataVtbl;
			At<float>(dp, 0x10) = pos[0]; At<float>(dp, 0x14) = pos[1]; At<float>(dp, 0x18) = pos[2];
			At<float>(dp, 0x28) = scale;
			At<uptr>(dp, 0x30) = base; At<uptr>(dp, 0x38) = cell; At<uptr>(dp, 0x40) = ws;
			At<std::uint8_t>(dp, 0x8C) = 1; At<std::uint8_t>(dp, 0x8D) = 1; At<std::uint32_t>(dp, 0x98) = 1;
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try { EngineFn<void* (*)(uptr, std::uint64_t*, void*)>(addr::kCreateReference)(data, &handle, d); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
			const uptr ref = RefFromHandle(static_cast<std::uint32_t>(handle));
			if (!ref) return 0;
			const std::uint32_t id = At<std::uint32_t>(ref, kFormId);
			__try { EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref); } __except (EXCEPTION_EXECUTE_HANDLER) {}
			return id;
		}
		bool Move(uptr ref, uptr cell, uptr ws, float pos[3])
		{
			float rot[3] = { 0, 0, 0 };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try { EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(ref, &handle, cell, ws, pos, rot); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		struct EnableEngine {
			uptr ref;
			void CancelRequests() { EngineFn<void (*)(uptr)>(addr::kEnableReference)(ref); }
			bool Disabled() const { return (At<std::uint32_t>(ref, kFormFlags) & kDisabledFlag) != 0; }
			void Enable() { EngineFn<void (*)(uptr, char)>(addr::kRefEnableTask)(ref, 1); }
		};
		void Show(uptr ref, bool on)
		{
			__try {
				const bool disabled = (At<std::uint32_t>(ref, kFormFlags) & kDisabledFlag) != 0;
				if (on && disabled) { EnableEngine engine{ ref }; placementenable::Restore(engine); }
				if (!on && !disabled) EngineFn<void (*)(uptr, char)>(addr::kDisableReference)(ref, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		bool Where(uptr ref, float pos[3])
		{
			__try { const auto& p = At<game::Vec3>(ref, off::kRefPosition); pos[0] = p.x; pos[1] = p.y; pos[2] = p.z; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool PlayerWorld(uptr& cell, uptr& ws, char (&name)[201])
		{
			name[0] = 0;
			__try {
				const uptr player = Ptr(addr::kPlayerCharacterPtr);
				cell = player ? At<uptr>(player, off::kRefParentCell) : 0;
				ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
				if (!ws) return false;
				auto** vt = *reinterpret_cast<void***>(ws);
				const char* n = reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(ws);
				int i = 0;
				for (; n && n[i] && i < 200; ++i) name[i] = n[i];
				name[i] = 0;
				return i > 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		const std::wstring& Ini() { static const std::wstring p = fvopaths::File(L"routeprobes.ini"); return p; }

		// The probe model (2026-10-28, the user: the beacons "are too crowded and too large ... maybe small lights"): routeprobes.ini
		// [look] model = a form id (default 00190DBD, NATheMastLightPuck01: the small round lights of the MAST); the old outpost
		// beacon if that form is missing. Read every 10 s, so a changed model applies while playing.
		constexpr std::uint32_t kPuck = 0x00190DBD;
		uptr ProbeBase()
		{
			static uptr base = 0;
			static ULONGLONG readAt = 0;
			const ULONGLONG now = GetTickCount64();
			if (base && now - readAt < 10000) return base;
			readAt = now;
			wchar_t text[32]{};
			GetPrivateProfileStringW(L"look", L"model", L"", text, 32, Ini().c_str());
			unsigned id = 0;
			if (std::swscanf(text, L"%x", &id) != 1 || !id) {
				id = kPuck;
				WritePrivateProfileStringW(L"look", L"model", L"00190DBD", Ini().c_str());
			}
			base = Lookup(id);
			if (!base) base = Ptr(addr::kBeaconBaseObject);
			return base;
		}
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		void LoadIds(const std::string& world)
		{
			g_ids.clear();
			// Probes made with another model (the beacons before 2026-10-28): hidden and forgotten; new ones are made.
			wchar_t style[32]{};
			GetPrivateProfileStringW(L"style", Wide(world).c_str(), L"", style, 32, Ini().c_str());
			wchar_t want[32];
			const uptr base = ProbeBase();
			std::swprintf(want, 32, L"%08X", base ? At<std::uint32_t>(base, kFormId) : 0u);
			if (std::wstring(want) != style) {
				std::vector<wchar_t> old(4096);
				GetPrivateProfileStringW(L"probes", Wide(world).c_str(), L"", old.data(), DWORD(old.size()), Ini().c_str());
				std::string text;
				for (const wchar_t* c = old.data(); *c; ++c) text.push_back(char(*c));
				std::size_t at = 0; unsigned id = 0; int used = 0, n = 0;
				while (std::sscanf(text.c_str() + at, "%x%n", &id, &used) == 1) { if (const uptr r = Lookup(id)) { Show(r, false); ++n; } at += used; }
				WritePrivateProfileStringW(L"probes", Wide(world).c_str(), L"", Ini().c_str());
				WritePrivateProfileStringW(L"style", Wide(world).c_str(), want, Ini().c_str());
				if (n) logger::Write("Route markers: %d probe(s) of the old look hidden for %s; new ones are made", n, world.c_str());
				return;
			}
			std::vector<wchar_t> w(4096);
			GetPrivateProfileStringW(L"probes", Wide(world).c_str(), L"", w.data(), DWORD(w.size()), Ini().c_str());
			std::string text;
			for (const wchar_t* c = w.data(); *c; ++c) text.push_back(char(*c));
			std::size_t at = 0;
			unsigned id = 0; int used = 0;
			while (std::sscanf(text.c_str() + at, "%x%n", &id, &used) == 1) { g_ids.push_back(id); at += used; }
		}
		void SaveIds()
		{
			if (g_world.empty()) return;
			std::string ids;
			for (const auto id : g_ids) { char b[12]; std::snprintf(b, sizeof b, "%s%08X", ids.empty() ? "" : " ", id); ids += b; }
			WritePrivateProfileStringW(L"probes", Wide(g_world).c_str(), Wide(ids).c_str(), Ini().c_str());
			g_unsaved = false;
		}
		void HideAll()
		{
			for (const auto id : g_ids) if (const uptr ref = Lookup(id)) Show(ref, false);
			if (g_unsaved) SaveIds();
			if (g_shown) logger::Write("Route markers: %zu probes hidden (%s)", g_ids.size(), g_world.c_str());
			g_shown = false;
		}
	}

	void Set(const char* world, const Marker* markers, int count)
	{
		std::lock_guard lk(g_lock);
		g_wantWorld = world ? world : "";
		g_want.assign(markers, markers + std::clamp(count, 0, kMax));
		++g_wantSerial;
	}

	void Reset()
	{
		if (g_unsaved) SaveIds();
		g_world.clear(); g_ids.clear(); g_now.clear(); g_shown = false; g_serial = ~0u; g_cursor = 0;
	}

	void Tick()
	{
		const bool on = features::On(features::Id::RouteMarkers);
		uptr cell = 0, ws = 0;
		char name[201];
		const bool inWorld = PlayerWorld(cell, ws, name);
		if (!on || !inWorld) { if (g_shown) HideAll(); return; }
		std::string world;
		std::vector<Marker> want;
		unsigned serial;
		{
			std::lock_guard lk(g_lock);
			world = g_wantWorld; serial = g_wantSerial;
			if (serial != g_serial) want = g_want;
		}
		if (world != name) { if (g_shown) HideAll(); return; }  // the addon's list is for another world (or not sent yet)
		if (world != g_world) { if (g_shown) HideAll(); if (g_unsaved) SaveIds(); g_world = world; LoadIds(world); g_serial = ~0u; }
		if (serial != g_serial) { g_serial = serial; g_now = want; g_cursor = 0; g_made = g_moved = 0; g_shown = true; }
		if (g_cursor > int(std::max(g_now.size(), g_ids.size()))) {
			if (GetTickCount64() - g_doneAt < 10000) return;
			g_cursor = 0;
		}
		const uptr base = ProbeBase();
		const int end = g_cursor + kPerFrame;
		for (; g_cursor < end && g_cursor < int(std::max(g_now.size(), g_ids.size())); ++g_cursor) {
			const int i = g_cursor;
			if (i >= int(g_now.size())) { if (const uptr ref = Lookup(g_ids[i])) Show(ref, false); continue; }  // more probes than points
			if (i >= int(g_ids.size())) g_ids.push_back(0);
			uptr ref = Lookup(g_ids[i]);
			if (ref && base && At<uptr>(ref, kBaseForm) != base) { Show(ref, false); ref = 0; }  // something else in this save, or the old look
			float pos[3] = { g_now[i].x, g_now[i].y, g_now[i].z };
			if (!ref) {
				g_ids[i] = Place(base, cell, ws, pos, g_now[i].scale);
				if (g_ids[i]) { ++g_made; g_unsaved = true; }
				continue;
			}
			float at[3];
			if (!Where(ref, at) || std::hypot(at[0] - pos[0], at[1] - pos[1]) > 0.5f || std::fabs(at[2] - pos[2]) > 0.5f) { if (Move(ref, cell, ws, pos)) ++g_moved; }
			Show(ref, true);
		}
		if (g_cursor < int(std::max(g_now.size(), g_ids.size()))) return;
		++g_cursor;  // done; checked again every 10 s (a cell that unloaded can take probes with it)
		if (g_unsaved) SaveIds();
		if (g_made || g_moved) logger::Write("Route markers: %zu probes for %s (%d placed, %d moved)", g_now.size(), g_world.c_str(), g_made, g_moved);
		g_made = g_moved = 0;
		g_doneAt = GetTickCount64();
	}
}

extern "C" __declspec(dllexport) void FVO_SetRouteMarkers(const char* world, const float* xyzs, int count)
{
	std::vector<routeprobes::Marker> m;
	for (int i = 0; xyzs && i < count; ++i) m.push_back({ xyzs[i * 4], xyzs[i * 4 + 1], xyzs[i * 4 + 2], xyzs[i * 4 + 3] });
	routeprobes::Set(world, m.data(), int(m.size()));
}

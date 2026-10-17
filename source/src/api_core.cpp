// Core side of core_api.h: the table the feature plugins receive, the callback registries, the look-handler mouse hook
// (shared by ship and vehicle control), and the small stand-ins the core's own sources call where the features used to
// live in the same DLL (flight::, content::, shipmodel::).
#include "api_core.h"

#include "addresses.h"
#include "game.h"
#include "handoff.h"
#include "log.h"
#include "parking.h"
#include "prefetch.h"
#include "rebase.h"
#include "recentre.h"
#include "terrainguard.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace coreapi
{
	namespace
	{
		std::mutex g_lock;
		std::vector<seamless::FrameFn> g_frame;
		std::vector<seamless::SyncFn> g_sync;
		std::vector<seamless::VoidFn> g_beforeSave, g_afterSave, g_loadReset;
		std::vector<seamless::ZoneEnterFn> g_zoneEnter;
		std::vector<seamless::PlayerFn> g_playerUpdate;
		std::vector<seamless::BoolFn> g_busy;
		std::atomic<seamless::TimeFn> g_postSteps{ nullptr };
		std::vector<seamless::ShiftFn> g_beforeShift, g_shift;

		template <class T>
		std::vector<T> Copy(const std::vector<T>& v)
		{
			std::lock_guard lk(g_lock);
			return v;
		}

		template <class T>
		void Add(std::vector<T>& v, T fn)
		{
			if (fn) {
				std::lock_guard lk(g_lock);
				v.push_back(fn);
			}
		}

		// ---- Mouse capture: PlayerControls::LookHandler vftable[6] (FUN_1412BC700) writes the frame's mouse counts into
		// the controls data (+0xC x, +0x10 y). While captured they are taken (for ship/vehicle control) and zeroed so the
		// camera does not orbit.
		using LookMouse_t = void (*)(std::uintptr_t, std::uintptr_t);
		LookMouse_t g_lookOriginal = nullptr;
		std::atomic<bool> g_capture{ false };
		std::atomic<float> g_mouseX{ 0.0f }, g_mouseY{ 0.0f };

		void LookMouseDetour(std::uintptr_t handler, std::uintptr_t event)
		{
			g_lookOriginal(handler, event);
			if (!g_capture.load()) {
				return;
			}
			const auto data = *reinterpret_cast<std::uintptr_t*>(handler + 0x40);
			if (!data) {
				return;
			}
			float& x = *reinterpret_cast<float*>(data + 0xC);
			float& y = *reinterpret_cast<float*>(data + 0x10);
			g_mouseX.store(g_mouseX.load() + x);
			g_mouseY.store(g_mouseY.load() + y);
			x = 0.0f;
			y = 0.0f;
		}

		// ---- Table entries
		bool ApiPending() { return recentre::Pending(); }
		float ApiHalfExtent() { return handoff::PhysicsHalfExtent(); }
		void ApiTotalShift(std::int32_t* x, std::int32_t* y)
		{
			std::int32_t a = 0, b = 0;
			recentre::TotalShift(a, b);
			if (x) *x = a;
			if (y) *y = b;
		}
		bool ApiZoneKeyAt(float x, float y, float z, std::uint64_t* key)
		{
			std::uint64_t k = 0;
			const bool ok = handoff::ZoneKeyAt(x, y, z, k);
			if (key) *key = k;
			return ok;
		}
		bool ApiZoneKeyAtLatLong(double lat, double lon, std::uint64_t* key)
		{
			std::uint64_t k = 0;
			const bool ok = handoff::ZoneKeyAtLatLong(lat, lon, k);
			if (key) *key = k;
			return ok;
		}
		double ApiZoneSide() { return handoff::ZoneSide(); }
		void ApiRequestGround(int slot, float x, float y, float z, float above, float length) { prefetch::RequestGround(slot, x, y, z, above, length); }
		void ApiRequestGroundMask(int slot, float x, float y, float z, float above, float length, std::uint32_t mask)
		{
			prefetch::RequestGroundMask(slot, x, y, z, above, length, mask);
		}
		bool ApiLatestGround(int slot, float* ground, unsigned* age)
		{
			float g = 0;
			unsigned a = 0;
			const bool ok = prefetch::LatestGround(slot, g, a);
			if (ground) *ground = g;
			if (age) *age = a;
			return ok;
		}
		void ApiSetProbeAnchor(std::uintptr_t ref) { prefetch::SetProbeAnchor(ref); }
		void ApiHideShipNow(const char* why) { rebase::HideShipNow(why); }
		void ApiParkClear() { parking::Clear(); }
		bool ApiParkGet(std::uint32_t* planet, double* lat, double* lon)
		{
			std::uint32_t p = 0;
			double a = 0, b = 0;
			const bool ok = parking::Get(p, a, b);
			if (planet) *planet = p;
			if (lat) *lat = a;
			if (lon) *lon = b;
			return ok;
		}
		void ApiCaptureMouse(bool on)
		{
			g_capture = on;
			if (!on) {
				g_mouseX = 0.0f;
				g_mouseY = 0.0f;
			}
		}
		void ApiTakeMouse(double* x, double* y)
		{
			const double a = g_mouseX.exchange(0.0f), b = g_mouseY.exchange(0.0f);
			if (x) *x = a;
			if (y) *y = b;
		}
		void ApiOnFrame(seamless::FrameFn fn) { Add(g_frame, fn); }
		void ApiOnSync(seamless::SyncFn fn) { Add(g_sync, fn); }
		void ApiOnBeforeSave(seamless::VoidFn fn) { Add(g_beforeSave, fn); }
		void ApiOnAfterSave(seamless::VoidFn fn) { Add(g_afterSave, fn); }
		void ApiOnLoadReset(seamless::VoidFn fn) { Add(g_loadReset, fn); }
		void ApiOnZoneEnter(seamless::ZoneEnterFn fn) { Add(g_zoneEnter, fn); }
		void ApiOnPlayerUpdate(seamless::PlayerFn fn) { Add(g_playerUpdate, fn); }
		void ApiAddBusy(seamless::BoolFn fn) { Add(g_busy, fn); }
		void ApiSetContentPostSteps(seamless::TimeFn fn) { g_postSteps = fn; }
		void ApiOnBeforeShift(seamless::ShiftFn fn) { Add(g_beforeShift, fn); }
		void ApiOnShift(seamless::ShiftFn fn) { Add(g_shift, fn); }
		bool ApiWorldSettled() { return recentre::WorldSettled(); }
		double ApiPlanetRadius() { return handoff::PlanetRadius(); }
		void ApiSetLeadSeconds(float s) { prefetch::SetLeadSeconds(s); }
		unsigned ApiCollisionPending() { return terrainguard::PendingCount(); }

		const seamless::CoreApi g_table{
			seamless::kApiVersion,
			&ApiPending, &ApiHalfExtent, &ApiTotalShift,
			&ApiZoneKeyAt, &ApiZoneKeyAtLatLong, &ApiZoneSide,
			&ApiRequestGround, &ApiRequestGroundMask, &ApiLatestGround, &ApiSetProbeAnchor,
			&ApiHideShipNow, &ApiParkClear, &ApiParkGet,
			&ApiCaptureMouse, &ApiTakeMouse,
			&ApiOnFrame, &ApiOnSync, &ApiOnBeforeSave, &ApiOnAfterSave, &ApiOnLoadReset, &ApiOnZoneEnter, &ApiOnPlayerUpdate, &ApiAddBusy,
			&ApiSetContentPostSteps,
			&ApiOnBeforeShift, &ApiOnShift, &ApiWorldSettled, &ApiPlanetRadius, &ApiSetLeadSeconds, &ApiCollisionPending
		};
	}

	const seamless::CoreApi& Table() { return g_table; }

	// The consolidated plugin uses the same table internally for map consumers.
	// The separate vehicle receives it through SFSE messaging when available.

	bool InstallMouseHook()
	{
		auto** vtbl = reinterpret_cast<void**>(game::Base() + addr::kLookHandlerVtbl);
		void* expected = reinterpret_cast<void*>(game::Base() + addr::kLookHandlerMouse);
		if (vtbl[6] != expected) {
			logger::Write("Core: look handler vtable slot 6 is not the mouse handler; mouse capture off");
			return false;
		}
		DWORD old = 0;
		VirtualProtect(&vtbl[6], sizeof(void*), PAGE_READWRITE, &old);
		g_lookOriginal = reinterpret_cast<LookMouse_t>(vtbl[6]);
		vtbl[6] = reinterpret_cast<void*>(&LookMouseDetour);
		VirtualProtect(&vtbl[6], sizeof(void*), old, &old);
		logger::Write("Core: mouse look handler hooked (mouse capture for ship and vehicle control)");
		return true;
	}

	void FireFrame()
	{
		for (auto fn : Copy(g_frame)) fn();
	}
	void FireSync(std::uintptr_t tes)
	{
		for (auto fn : Copy(g_sync)) fn(tes);
	}
	void FireBeforeSave()
	{
		for (auto fn : Copy(g_beforeSave)) fn();
	}
	void FireAfterSave()
	{
		for (auto fn : Copy(g_afterSave)) fn();
	}
	void FireLoadReset()
	{
		for (auto fn : Copy(g_loadReset)) fn();
	}
	void FireZoneEnter(std::uint64_t key, float x, float y, float z, float vx, float vy, bool start)
	{
		for (auto fn : Copy(g_zoneEnter)) fn(key, x, y, z, vx, vy, start);
	}
	void FirePlayerUpdate(std::uintptr_t player)
	{
		for (auto fn : Copy(g_playerUpdate)) fn(player);
	}
	bool AnyBusy()
	{
		for (auto fn : Copy(g_busy)) {
			if (fn()) {
				return true;
			}
		}
		return false;
	}
	void FireBeforeShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes)
	{
		for (auto fn : Copy(g_beforeShift)) fn(cx, cy, tes);
	}
	void FireShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes)
	{
		for (auto fn : Copy(g_shift)) fn(cx, cy, tes);
	}
	unsigned long long ContentPostSteps()
	{
		const auto fn = g_postSteps.load();
		return fn ? fn() : 0;
	}
}

namespace seamless
{
	const CoreApi* Api() { return &coreapi::Table(); }
}

namespace flight
{
	void CaptureMouse(bool on) { coreapi::Table().CaptureMouse(on); }
	void TakeMouse(double& x, double& y) { coreapi::Table().TakeMouse(&x, &y); }
}

// Feature-plugin side of core_api.h: holds the table received from the core and forwards the calls the feature sources
// make into the core's modules (recentre::, handoff::, prefetch::, rebase::, parking::, the mouse capture). Without the
// core every call answers a safe default.
#include "core_api.h"
#include "vehicle_probe.h"

#include <atomic>
#include <cstdint>

namespace seamless
{
	namespace
	{
		std::atomic<const CoreApi*> g_api{ nullptr };
	}

	const CoreApi* Api() { return g_api.load(); }

	void SetApi(const CoreApi* api) { g_api = api; }
}

namespace
{
	const seamless::CoreApi* A() { return seamless::Api(); }
}

namespace recentre
{
	bool Pending() { return A() && A()->RecentrePending(); }
	void TotalShift(std::int32_t& x, std::int32_t& y)
	{
		x = y = 0;
		if (A()) A()->TotalShift(&x, &y);
	}
}

namespace handoff
{
	float PhysicsHalfExtent() { return A() ? A()->PhysicsHalfExtent() : 6400.0f; }
	bool ZoneKeyAt(float x, float y, float z, std::uint64_t& key) { return A() && A()->ZoneKeyAt(x, y, z, &key); }
	bool ZoneKeyAtLatLong(double lat, double lon, std::uint64_t& key) { return A() && A()->ZoneKeyAtLatLong(lat, lon, &key); }
	double ZoneSide() { return A() ? A()->ZoneSide() : 3600.0; }
}

namespace prefetch
{
	void RequestGround(int slot, float x, float y, float z, float above, float length)
	{
		if (A()) A()->RequestGround(slot, x, y, z, above, length);
		else vehicle_probe::Request(slot, x, y, z, above, length);
	}
	void RequestGroundMask(int slot, float x, float y, float z, float above, float length, std::uint32_t mask)
	{
		if (A()) A()->RequestGroundMask(slot, x, y, z, above, length, mask);
		else vehicle_probe::Request(slot, x, y, z, above, length, mask);
	}
	bool LatestGround(int slot, float& ground, unsigned& age)
	{
		return A() ? A()->LatestGround(slot, &ground, &age) : vehicle_probe::Latest(slot, ground, age);
	}
	void SetProbeAnchor(std::uintptr_t ref)
	{
		if (A()) A()->SetProbeAnchor(ref);
	}
	void RequestRay(int slot, const float from[3], const float to[3], std::uint64_t mask)
	{
		if (!A()) vehicle_probe::RequestRay(slot, from, to, mask);
	}
	bool LatestRay(int slot, RayHit& out, unsigned& age)
	{
		return !A() && vehicle_probe::LatestRay(slot, out, age);
	}
}

namespace rebase
{
	void HideShipNow(const char* why)
	{
		if (A()) A()->HideShipNow(why);
	}
}

namespace parking
{
	void Clear()
	{
		if (A()) A()->ParkClear();
	}
	bool Get(std::uint32_t& planet, double& lat, double& lon) { return A() && A()->ParkGet(&planet, &lat, &lon); }
}


namespace flight
{
	void CaptureMouse(bool on)
	{
		if (A()) A()->CaptureMouse(on);
	}
	void TakeMouse(double& x, double& y)
	{
		x = y = 0;
		if (A()) A()->TakeMouse(&x, &y);
	}
}

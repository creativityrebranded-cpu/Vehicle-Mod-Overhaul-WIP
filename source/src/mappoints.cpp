#include "mappoints.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <mutex>

namespace mappoints
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }

		// BodyInspectState POI list: std::vector of 0x80-byte records (begin +0x168, end +0x170); see globe.cpp for the flag bytes.
		constexpr std::uint32_t kPoiBegin = 0x168, kBody = 0xC90;
		constexpr std::size_t kRecordSize = 0x80;
		constexpr std::uint32_t kRecLatitude = 0x40, kRecLongitude = 0x48;

		using Publish_t = void (*)(void*, uptr);
		Publish_t g_original = nullptr;

		struct Rec { double lat, lon; bool outpost; };
		std::mutex g_lock;
		std::vector<Rec> g_pois;
		std::uint32_t g_body = 0;
		unsigned g_logged = ~0u;

		constexpr std::size_t kMaxRead = 4096;
		Rec g_scratch[kMaxRead];

		// Reads the shown + visible records into g_scratch (no objects here: SEH only).
		bool ReadRecords(uptr state, std::size_t& kept, std::size_t& outposts, std::size_t& n, std::uint32_t& body)
		{
			__try {
				const auto* begin = At<const std::uint8_t*>(state, kPoiBegin);
				const auto* end = At<const std::uint8_t*>(state, kPoiBegin + 8);
				n = (begin && end > begin) ? std::size_t(end - begin) / kRecordSize : 0;
				kept = outposts = 0;
				if (!n || n > 20000) return false;
				for (std::size_t i = 0; i < n && kept < kMaxRead; ++i) {
					const auto* r = begin + i * kRecordSize;
					if (!r[0x6A] || !r[0x6C]) continue;
					const double lat = *reinterpret_cast<const double*>(r + kRecLatitude);
					const double lon = *reinterpret_cast<const double*>(r + kRecLongitude);
					if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 1.571 || std::fabs(lon) > 6.284) continue;
					g_scratch[kept++] = { lat, lon, r[0x73] != 0 };
					if (r[0x73]) ++outposts;
				}
				body = At<std::uint32_t>(state, kBody);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Pass-through: the records are read before the game's own publish runs, nothing is written.
		void Publish(void* arg, uptr state)
		{
			std::size_t kept = 0, outposts = 0, n = 0;
			std::uint32_t body = 0;
			if (ReadRecords(state, kept, outposts, n, body)) {
				std::vector<Rec> v(g_scratch, g_scratch + kept);
				const unsigned sig = unsigned(kept) * 31u + unsigned(outposts) + body;
				{
					std::lock_guard lk(g_lock);
					g_pois = std::move(v);
					g_body = body;
				}
				if (sig != g_logged) {
					g_logged = sig;
					logger::Write("Map points: captured %zu of %zu records, %zu outposts, body %08X (scanner marker magnet)", kept, n, outposts, body);
				}
			}
			g_original(arg, state);
		}

		// World position -> lat/long (the engine's own conversion, FUN_14078d1f0): out[0] = longitude, out[1] = latitude.
		void ToLatLong(uptr mgr, double x, double y, float z, double& lat, double& lon)
		{
			double out[2] = {};
			const float p[3] = { float(x), float(y), z };
			reinterpret_cast<void* (*)(uptr, double*, const float*)>(game::Base() + addr::kWorldToLatLong)(mgr, out, p);
			lon = out[0];
			lat = out[1];
		}

		bool PlanetOf(uptr mgr, std::uint32_t& planet)
		{
			__try {
				if (At<std::uint8_t>(mgr, off::kMgrState) != 2) return false;
				planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		double Wrap(double a)
		{
			while (a > 3.14159265358979) a -= 6.28318530717959;
			while (a < -3.14159265358979) a += 6.28318530717959;
			return a;
		}

		// Newton on the engine's world -> lat/long (its inverse is not known): start at the player, follow the numeric Jacobian. Plain data only: SEH.
		bool Solve(uptr mgr, double qlat, double qlon, const float pp[3], double& x, double& y)
		{
			x = pp[0];
			y = pp[1];
			__try {
				for (int it = 0; it < 8; ++it) {
					double lat0, lon0, lat1, lon1, lat2, lon2;
					ToLatLong(mgr, x, y, pp[2], lat0, lon0);
					const double eLon = Wrap(qlon - lon0), eLat = qlat - lat0;
					const double h = 200.0;
					ToLatLong(mgr, x + h, y, pp[2], lat1, lon1);
					ToLatLong(mgr, x, y + h, pp[2], lat2, lon2);
					const double a = Wrap(lon1 - lon0) / h, c = (lat1 - lat0) / h, b = Wrap(lon2 - lon0) / h, d = (lat2 - lat0) / h;
					const double det = a * d - b * c;
					if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
					const double dx = (d * eLon - b * eLat) / det, dy = (-c * eLon + a * eLat) / det;
					if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
					x += dx;
					y += dy;
					if (std::hypot(dx, dy) < 1.0) return true;
					if (std::hypot(x - pp[0], y - pp[1]) > 400000.0) return false;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return false;
		}
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[16] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kGlobePublish, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Publish), &original)) {
			logger::Write("Map points: the globe publish is already hooked (or differs); no outpost data for the scanner magnet");
			return false;
		}
		g_original = reinterpret_cast<Publish_t>(original);
		logger::Write("Map points: watching the globe's marker publish");
		return true;
	}

	bool WorldPoints(const float playerPos[3], std::vector<Point>& out)
	{
		out.clear();
		const uptr mgr = *reinterpret_cast<uptr*>(game::Base() + addr::kPlanetManagerPtr);
		std::uint32_t planet = 0;
		if (!g_original || !mgr || !PlanetOf(mgr, planet)) return false;
		std::vector<Rec> pois;
		{
			std::lock_guard lk(g_lock);
			if (g_body != planet) return false;   // only the planet the player is on
			pois = g_pois;
		}
		for (const auto& q : pois) {
			double x = 0, y = 0;
			if (Solve(mgr, q.lat, q.lon, playerPos, x, y)) out.push_back({ float(x), float(y), q.outpost });
		}
		return !out.empty();
	}
}

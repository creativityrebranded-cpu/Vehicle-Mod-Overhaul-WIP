#include "globe.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "anchor.h"
#include "parking.h"
#include "telemetry.h"
#include "core_api.h"
#include "hexgrid.h"
#include "sphere.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace globe
{
	namespace
	{
		// BodyInspectState POI list: std::vector of 0x80-byte records (begin +0x168, end +0x170, capacity +0x178).
		constexpr std::uint32_t kPoiBegin = 0x168;
		constexpr std::size_t kRecordSize = 0x80;
		constexpr std::uint32_t kRecName = 0x08, kRecExtra = 0x10, kRecQuest = 0x18;
		constexpr std::uint32_t kRecLatitude = 0x40, kRecLongitude = 0x48, kRecDirty = 0x78;
		// Flag bytes, in UI order: shouldShowIcon, showHighlight, isVisible, isLandable, isFastTravelable,
		// hasQuestTarget, hasPlayerMarker, questActive, hasOutpost, discovered, hasGroupLandingMarker.
		// Confirmed in game: "Landing Area" and "Akila City" differ only in +0x71 = hasPlayerMarker.
		constexpr std::uint8_t kBoolOffsets[11] = { 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x70, 0x71, 0x72, 0x73, 0x76, 0x77 };
		constexpr std::uint32_t kRecHasPlayerMarker = 0x71;

		using Publish_t = void (*)(void*, std::uintptr_t);
		Publish_t g_original = nullptr;

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		// Marker positions, refreshed on the main thread, read by the hook.
		struct Cache
		{
			bool   havePlayer = false, haveShip = false;
			bool settled = false;
			double playerLat = 0, playerLong = 0, shipLat = 0, shipLong = 0;
			double planetRadius = 0;
			std::uint32_t planetId = 0;
		};
		std::mutex g_lock;
		Cache g_cache;

		void* g_namePlayer = nullptr;
		void* g_nameShip = nullptr;
		void* g_nameZone = nullptr;
		void* g_nameCell = nullptr;
		constexpr double kZoneEnter = 7200.0;
		constexpr double kExactEnter = 150.0;
		constexpr double kCellMetres = 100.0;

		double SurfaceDistance(const Cache& c, double lat, double lon)
		{
			const double dlat = lat - c.playerLat, dlon = lon - c.playerLong;
			const double a = std::pow(std::sin(dlat * 0.5), 2) + std::cos(lat) * std::cos(c.playerLat) * std::pow(std::sin(dlon * 0.5), 2);
			return 2.0 * c.planetRadius * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
		}

		struct Group
		{
			std::array<std::uint8_t, kRecordSize> sample{};
			double lat = 0, lon = 0;
			std::size_t count = 0;
			bool zone = false;
		};

		std::int32_t Signed28(std::uint64_t value)
		{
			std::uint32_t x = static_cast<std::uint32_t>(value & 0xFFFFFFF);
			if (x & 0x8000000) x |= 0xF0000000;
			return static_cast<std::int32_t>(x);
		}

		void AddGroup(std::map<std::tuple<unsigned, std::uint64_t, std::int64_t>, Group>& groups,
			const std::uint8_t* record, const Cache& c, double lat, double lon, double distance)
		{
			std::uint64_t zoneKey = 0;
			const auto* api = seamless::Api();
			if (!api || !api->ZoneKeyAtLatLong(lat, lon, &zoneKey)) return;
			const bool zoneStage = distance >= kZoneEnter;
			std::int64_t cellKey = 0;
			double proxyLat = lat, proxyLon = lon;
			if (!zoneStage) {
				const auto latBin = static_cast<std::int32_t>(std::floor(lat * c.planetRadius / kCellMetres));
				proxyLat = (latBin + 0.5) * kCellMetres / c.planetRadius;
				const double cosLat = std::max(0.01, std::cos(proxyLat));
				const auto lonBin = static_cast<std::int32_t>(std::floor(lon * c.planetRadius * cosLat / kCellMetres));
				proxyLon = (lonBin + 0.5) * kCellMetres / (c.planetRadius * cosLat);
				cellKey = static_cast<std::int64_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(latBin)) << 32) |
					static_cast<std::uint32_t>(lonBin));
			}
			else {
				const double side = api->ZoneSide();
				if (side <= 0) return;
				const int frequency = static_cast<int>(std::lround(sphere::EdgeAngle() * c.planetRadius / side));
				if (frequency <= 0) return;
				const auto i = Signed28((zoneKey >> 28) & 0xFFFFFFF);
				const auto j = Signed28(zoneKey & 0xFFFFFFF);
				hexgrid::Lattice lattice{ 1.0 };
				double x = 0, y = 0;
				lattice.CentreOf({ i, j }, x, y);
				sphere::ToLatLong({ static_cast<int>((zoneKey >> 56) & 0xFF), x, y }, frequency, proxyLat, proxyLon);
			}
			auto& g = groups[{ zoneStage ? 1u : 0u, zoneKey, cellKey }];
			if (!g.count) {
				std::memcpy(g.sample.data(), record, kRecordSize);
				g.lat = proxyLat;
				g.lon = proxyLon;
			}
			++g.count;
			g.zone = zoneStage;
		}

		const char* FixedText(std::uintptr_t entry)
		{
			if (!entry) {
				return "";
			}
			while ((At<std::uint32_t>(entry, 0x14) >> 1) & 1) {
				entry = At<std::uintptr_t>(entry, 0x08);
			}
			return reinterpret_cast<const char*>(entry + 0x18);
		}

		// World position -> planet latitude/longitude (radians), the engine's own conversion (FUN_14078d1f0):
		// out[0] = longitude, out[1] = latitude.
		bool ToLatLong(std::uintptr_t mgr, const game::Vec3& pos, double& lat, double& lon)
		{
			double out[2] = {};
			float p[3] = { pos.x, pos.y, pos.z };
			EngineFn<void* (*)(std::uintptr_t, double*, const float*)>(addr::kWorldToLatLong)(mgr, out, p);
			lon = out[0];
			lat = out[1];
			return true;
		}

		void MakeRecord(std::uint8_t* rec, const std::uint8_t* tmpl, void* name, double lat, double lon)
		{
			std::memcpy(rec, tmpl, kRecordSize);
			*reinterpret_cast<std::uint32_t*>(rec) = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			*reinterpret_cast<void**>(rec + kRecName) = name;
			*reinterpret_cast<void**>(rec + kRecExtra) = nullptr;
			*reinterpret_cast<void**>(rec + kRecQuest) = nullptr;
			*reinterpret_cast<double*>(rec + kRecLatitude) = lat;
			*reinterpret_cast<double*>(rec + kRecLongitude) = lon;
			rec[kRecDirty] = 1;
		}

		// Logs the globe's records (rate-limited) so the bool flags can be mapped to their meaning.
		void LogRecords(std::uintptr_t state, const std::uint8_t* begin, std::size_t n, const Cache& c)
		{
			static ULONGLONG last = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - last < 10000) {
				return;
			}
			last = now;
			telemetry::Record("globe_publish", "\"records\":%zu,\"state_body\":\"%08X\",\"planet\":\"%08X\",\"player\":[%.5f,%.5f],\"ship\":[%.5f,%.5f],\"have\":[%d,%d]",
				n, At<std::uint32_t>(state, 0xC90), c.planetId, c.playerLat, c.playerLong, c.shipLat, c.shipLong, c.havePlayer ? 1 : 0, c.haveShip ? 1 : 0);
			for (std::size_t i = 0; i < n && i < 64; ++i) {
				const auto* r = begin + i * kRecordSize;
				char bools[12] = {};
				for (int b = 0; b < 11; ++b) {
					bools[b] = r[kBoolOffsets[b]] ? '1' : '0';
				}
				std::string name = FixedText(*reinterpret_cast<const std::uintptr_t*>(r + kRecName));
				for (auto& ch : name) {
					if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20) {
						ch = ' ';
					}
				}
				telemetry::Record("globe_poi", "\"i\":%zu,\"name\":\"%s\",\"lat\":%.5f,\"long\":%.5f,\"type\":%u,\"u\":[%u,%u,%u,%u],\"w\":[%u,%u],\"bools\":\"%s\",\"handle\":\"%08X\"",
					i, name.c_str(), *reinterpret_cast<const double*>(r + kRecLatitude), *reinterpret_cast<const double*>(r + kRecLongitude), r[0x68],
					*reinterpret_cast<const std::uint32_t*>(r + 0x30), *reinterpret_cast<const std::uint32_t*>(r + 0x34),
					*reinterpret_cast<const std::uint32_t*>(r + 0x38), *reinterpret_cast<const std::uint32_t*>(r + 0x3C),
					*reinterpret_cast<const std::uint16_t*>(r + 4), *reinterpret_cast<const std::uint16_t*>(r + 6), bools,
					*reinterpret_cast<const std::uint32_t*>(r));
			}
		}

		// Detour for the globe's marker publish (FUN_141684a10): append our records for the duration of the call.
		void Publish(void* arg, std::uintptr_t state)
		{
			Cache c;
			{
				std::lock_guard lk(g_lock);
				c = g_cache;
			}
			auto& begin = At<std::uint8_t*>(state, kPoiBegin);
			auto& end = At<std::uint8_t*>(state, kPoiBegin + 8);
			auto& cap = At<std::uint8_t*>(state, kPoiBegin + 16);
			const std::size_t n = (begin && end > begin) ? static_cast<std::size_t>(end - begin) / kRecordSize : 0;
			LogRecords(state, begin, n, c);

			if (n == 0 || (!c.havePlayer && !c.haveShip) || !g_namePlayer) {
				g_original(arg, state);
				return;
			}
			thread_local std::vector<std::uint8_t> buffer;
			buffer.assign(begin, end);
			std::uint8_t tmpl[kRecordSize];
			std::memcpy(tmpl, begin, kRecordSize);  // icon/flags copied from an existing POI until the flags are mapped
			// Display-only hierarchy. Original records, references and fast-travel coordinates are left alone.
			// A proxy has no valid reference handle and is never a travel target.
			std::map<std::tuple<unsigned, std::uint64_t, std::int64_t>, Group> groups;
			std::vector<std::uint8_t> showFlags(n), visibleFlags(n);
			std::size_t exact = 0, hidden = 0;
			const bool sameBody = c.havePlayer && c.planetRadius > 0 &&
				At<std::uint32_t>(state, 0xC90) == c.planetId && g_nameZone && g_nameCell;
			for (std::size_t i = 0; i < n; ++i) {
				auto* r = buffer.data() + i * kRecordSize;
				showFlags[i] = r[0x6A];
				visibleFlags[i] = r[0x6C];
				if (!sameBody || !r[0x6C] || !r[0x6A]) continue;
				const double lat = *reinterpret_cast<const double*>(r + kRecLatitude);
				const double lon = *reinterpret_cast<const double*>(r + kRecLongitude);
				if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > 1.571 || std::abs(lon) > 6.284) continue;
				const double d = SurfaceDistance(c, lat, lon);
				if (d < kExactEnter && c.settled) { ++exact; continue; }
				const auto oldCount = groups.size();
				AddGroup(groups, r, c, lat, lon, d);
				if (groups.size() == oldCount && groups.empty()) continue;
				r[0x6A] = 0;
				r[0x6C] = 0;
				++hidden;
			}
			for (const auto& [key, g] : groups) {
				if (!g.count) continue;
				buffer.resize(buffer.size() + kRecordSize);
				auto* rec = buffer.data() + buffer.size() - kRecordSize;
				MakeRecord(rec, g.sample.data(), g.zone ? g_nameZone : g_nameCell, g.lat, g.lon);
				rec[0x6A] = 1;
				rec[0x6B] = 0;
				rec[0x6C] = 1;
				rec[0x6D] = 0;
				rec[0x6E] = 0;
				rec[0x70] = 0;
				rec[0x71] = 0;
				rec[0x72] = 0;
				rec[0x73] = 0;
				rec[0x76] = 1;
				rec[0x77] = 0;
			}
			if (sameBody) telemetry::Record("marker_hierarchy", "\"source\":%zu,\"hidden\":%zu,\"exact\":%zu,\"proxies\":%zu", n, hidden, exact, groups.size());
			// The globe's own "you are here" is a flag on the landing location, drawn at the landing site. Move it
			// to our streaming Player Location record (in the published copy only).
			std::vector<std::uint8_t> playerFlags(n);
			for (std::size_t i = 0; i < n; ++i) {
				playerFlags[i] = buffer[i * kRecordSize + kRecHasPlayerMarker];
				if (c.havePlayer) {
					buffer[i * kRecordSize + kRecHasPlayerMarker] = 0;
				}
			}
			if (c.havePlayer) {
				buffer.resize(buffer.size() + kRecordSize);
				auto* rec = buffer.data() + buffer.size() - kRecordSize;
				MakeRecord(rec, tmpl, g_namePlayer, c.playerLat, c.playerLong);
				rec[kRecHasPlayerMarker] = 1;
			}
			if (c.haveShip) {
				buffer.resize(buffer.size() + kRecordSize);
				auto* rec = buffer.data() + buffer.size() - kRecordSize;
				MakeRecord(rec, tmpl, g_nameShip, c.shipLat, c.shipLong);
				rec[kRecHasPlayerMarker] = 0;
			}
			auto* const oldBegin = begin;
			auto* const oldEnd = end;
			auto* const oldCap = cap;
			begin = buffer.data();
			end = buffer.data() + buffer.size();
			cap = end;
			g_original(arg, state);
			// Keep any per-record updates the publish made to the game's own records.
			std::memcpy(oldBegin, buffer.data(), n * kRecordSize);
			for (std::size_t i = 0; i < n; ++i) {
				oldBegin[i * kRecordSize + kRecHasPlayerMarker] = playerFlags[i];  // the game's own records keep their flag
				oldBegin[i * kRecordSize + 0x6A] = showFlags[i];
				oldBegin[i * kRecordSize + 0x6C] = visibleFlags[i];
			}
			begin = oldBegin;
			end = oldEnd;
			cap = oldCap;
		}

		// Ship-only map update. The former hierarchy also moved every native POI into
		// synthetic zone markers, which made Cydonia misleading; it remains disabled.
		void PublishShip(void* arg, std::uintptr_t state)
		{
			Cache c;
			{
				std::lock_guard lk(g_lock);
				c = g_cache;
			}
			auto& begin = At<std::uint8_t*>(state, kPoiBegin);
			auto& end = At<std::uint8_t*>(state, kPoiBegin + 8);
			auto& cap = At<std::uint8_t*>(state, kPoiBegin + 16);
			const auto n = (begin && end > begin) ? static_cast<std::size_t>(end - begin) / kRecordSize : 0;
			LogRecords(state, begin, n, c);
			if (!n || !c.haveShip || !g_nameShip || At<std::uint32_t>(state, 0xC90) != c.planetId) {
				g_original(arg, state);
				return;
			}
			thread_local std::vector<std::uint8_t> buffer;
			buffer.assign(begin, end);
			buffer.resize(buffer.size() + kRecordSize);
			auto* rec = buffer.data() + n * kRecordSize;
			MakeRecord(rec, begin, g_nameShip, c.shipLat, c.shipLong);
			rec[0x6A] = 1;  // show icon
			rec[0x6B] = 0;
			rec[0x6C] = 1;  // visible
			rec[0x6D] = 0;  // display only: the ship is not a landing target
			rec[0x6E] = 0;  // no fast travel to an unloaded ship
			rec[0x70] = 0;
			rec[0x71] = 0;  // do not make the ship the player marker
			rec[0x72] = 0;
			rec[0x73] = 0;
			rec[0x76] = 1;
			rec[0x77] = 0;
			auto* const oldBegin = begin;
			auto* const oldEnd = end;
			auto* const oldCap = cap;
			begin = buffer.data();
			end = buffer.data() + buffer.size();
			cap = end;
			g_original(arg, state);
			std::memcpy(oldBegin, buffer.data(), n * kRecordSize);
			begin = oldBegin;
			end = oldEnd;
			cap = oldCap;
			telemetry::Record("ship_map", "\"planet\":\"%08X\",\"lat\":%.7f,\"lon\":%.7f,\"source\":%zu",
				c.planetId, c.shipLat, c.shipLong, n);
		}
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[16] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kGlobePublish, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&PublishShip), &original)) {
			return false;
		}
		g_original = reinterpret_cast<Publish_t>(original);
		return true;
	}

	void Tick()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 500) {
			return;
		}
		last = now;
		if (!g_namePlayer) {
			const auto create = EngineFn<void (*)(void**, const char*, bool)>(addr::kFixedStringCreate);
			create(&g_namePlayer, "Player Location", true);
			create(&g_nameShip, "My Ship", true);
			create(&g_nameZone, "Zone locations", true);
			create(&g_nameCell, "Nearby locations", true);
		}

		Cache c;
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto anchor = anchor::Current();
		if (mgr && player && anchor.ref && At<std::uint8_t>(mgr, off::kMgrState) == 2) {
			c.planetId = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			c.havePlayer = ToLatLong(mgr, At<game::Vec3>(anchor.ref, off::kRefPosition), c.playerLat, c.playerLong);
			if (const auto* api = seamless::Api()) {
				c.planetRadius = api->PlanetRadius();
				c.settled = api->WorldSettled();
			}

			// Prefer a loaded ship in the current frame. A landed ship may have a ship cell
			// without a worldspace parent, so accept it only while its 3D is loaded nearby.
			// Once it streams out, the core's canonical last-known planet position wins.
			std::uintptr_t ship = 0;
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
			if (ship) {
				const auto shipCell = At<std::uintptr_t>(ship, off::kRefParentCell);
				const auto anchorWs = At<std::uintptr_t>(anchor.cell, off::kCellWorldspace);
				const bool hidden = (At<std::uint32_t>(ship, 0x20) & 0x800) != 0;  // disabled: parked out of sight (rebase)
				const auto shipWs = shipCell ? At<std::uintptr_t>(shipCell, off::kCellWorldspace) : 0;
				const auto loaded = At<std::uintptr_t>(ship, 0xB8);
				const bool has3D = loaded && At<std::uintptr_t>(loaded, 0x08);
				const auto sp = At<game::Vec3>(ship, off::kRefPosition);
				const auto pp = At<game::Vec3>(anchor.ref, off::kRefPosition);
				const bool nearby = has3D && std::isfinite(sp.x) && std::isfinite(sp.y) &&
					std::hypot(sp.x - pp.x, sp.y - pp.y) < 3000.0f;
				if (!hidden && anchorWs && (shipWs == anchorWs || (!shipWs && nearby)) && nearby) {
					c.haveShip = ToLatLong(mgr, sp, c.shipLat, c.shipLong);
				}
				std::uint32_t parkedPlanet = 0;
				double lat = 0, lon = 0;
				if (!c.haveShip && parking::Get(parkedPlanet, lat, lon) && parkedPlanet == c.planetId) {
					c.shipLat = lat;
					c.shipLong = lon;
					c.haveShip = true;
				}
				EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
			}
		}
		std::lock_guard lk(g_lock);
		g_cache = c;
	}
}

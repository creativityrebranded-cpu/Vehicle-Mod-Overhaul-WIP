#include "landing.h"
#include "recentre.h"

#include "addresses.h"
#include "anchor.h"
#include "handoff.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace landing
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kSurfaceLand = 0x017DEC70;  // (out desc, planet id, double latlong[2], marker handle, char, int mode)
		constexpr std::uint32_t kSiteLookup = 0x00798D50;   // (registry = *(mgr+0x140), out {lat, lon, form id}, latlong)
		constexpr std::uint32_t kMgrSites = 0x140;
		constexpr std::uint32_t kLookupForm = 0x005DE3B0;  // (form id) -> TESForm* (global form map), or null
		constexpr std::uint8_t kFormTypeWorldspace = 'V';  // TESForm +0x2E (the marker finder FUN_14077F900 requires 'V')
		constexpr ULONGLONG kExitDwellMs = 3000;           // outside a site's footprint this long before landing outside
		constexpr bool kAutoLanding = true;                // 0.51.2: on, separate-worldspace sites only, transitions only
		constexpr ULONGLONG kMinGapMs = 120000;            // at most one automatic landing every 2 minutes
		constexpr ULONGLONG kSameSiteMs = 300000;          // never the same site twice within 5 minutes
		constexpr ULONGLONG kOutsideFirstMs = 3000;        // outside every separate-worldspace footprint this long first
		// City guard (0.51.5): New Atlantis lists 35 hand-built areas packed into the city. When the current landing
		// itself is in a hand-built site, walking between the city's areas within this distance of it never lands.
		constexpr double kCityGuardMetres = 5000.0;

		struct
		{
			std::uint32_t planet = 0;          // planet the registry was read for
			std::set<std::uint32_t> siteWorlds;  // worldspace form ids of the planet's separate-worldspace sites
			std::uint32_t lastSite = 0;          // the site of the last automatic landing
			ULONGLONG outsideAll = 0;            // since when the player is outside every separate-worldspace footprint
			ULONGLONG lastLanding = 0;
			ULONGLONG outsideSince = 0;
			std::uint32_t announcedSite = 0;
			std::atomic<bool> requested{ false };
			std::string requestWhy;
		} g;

		bool PlayerLatLong(std::uintptr_t mgr, std::uintptr_t ref, double out[2])
		{
			alignas(16) double ll[4] = {};
			const auto p = At<game::Vec3>(ref, off::kRefPosition);
			const float fp[3] = { p.x, p.y, p.z };
			EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, ll, fp);
			out[0] = ll[0];
			out[1] = ll[1];
			return std::isfinite(out[0]) && std::isfinite(out[1]);
		}

		// Planet atlas (0.56.0): every registry site with its zone, built on a worker thread in parallel with play, so the
		// planner knows which zone ahead holds which site (hand-built worldspaces such as Cydonia included) before the
		// player gets there. Sites: {lat, lon} radians, the site form, hand-built worldspace or not.
		struct AtlasSite
		{
			double lat = 0, lon = 0;
			std::uint32_t id = 0;
			bool world = false;   // hand-built separate worldspace (entered by landing)
			std::uint64_t zone = 0;
		};
		struct Atlas
		{
			std::mutex lock;
			std::uint32_t planet = 0;
			std::vector<AtlasSite> sites;
			std::unordered_map<std::uint64_t, std::vector<std::size_t>> byZone;
			std::atomic<bool> ready{ false };
			std::atomic<unsigned> generation{ 0 };
		} g_atlas;

		void BuildAtlasAsync(std::uint32_t planet, std::vector<AtlasSite> sites)
		{
			const unsigned gen = g_atlas.generation.fetch_add(1) + 1;
			g_atlas.ready = false;
			std::thread([planet, sites = std::move(sites), gen]() mutable {
				// The zone lattice needs the planet radius (measured by handoff after the planet settles): wait up to 60 s.
				for (int i = 0; i < 600 && handoff::PlanetRadius() <= 0; ++i) {
					if (g_atlas.generation.load() != gen) {
						return;
					}
					Sleep(100);
				}
				std::unordered_map<std::uint64_t, std::vector<std::size_t>> byZone;
				for (std::size_t i = 0; i < sites.size(); ++i) {
					std::uint64_t key = 0;
					if (handoff::ZoneKeyAtLatLong(sites[i].lat, sites[i].lon, key)) {
						sites[i].zone = key;
						byZone[key].push_back(i);
					}
				}
				if (g_atlas.generation.load() != gen) {
					return;  // a newer planet's atlas started
				}
				{
					std::lock_guard lk(g_atlas.lock);
					g_atlas.planet = planet;
					g_atlas.sites = std::move(sites);
					g_atlas.byZone = std::move(byZone);
					unsigned worlds = 0;
					for (const auto& st : g_atlas.sites) {
						if (st.world) {
							++worlds;
							logger::Write("Atlas: hand-built site %08X at lat/long %.6f, %.6f in zone %016llX", st.id, st.lat, st.lon,
								static_cast<unsigned long long>(st.zone));
						}
					}
					logger::Write("Atlas: planet %08X mapped on a worker thread: %zu sites (%u hand-built worldspaces) in %zu zones", planet,
						g_atlas.sites.size(), worlds, g_atlas.byZone.size());
				}
				g_atlas.ready = true;
			}).detach();
		}

		// The planet's site registry, logged once per planet (the map of hand-built landing sites).
		void ReadRegistry(std::uintptr_t mgr)
		{
			const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			if (planet == g.planet) {
				return;
			}
			g.planet = planet;
			g.siteWorlds.clear();
			const auto reg = At<std::uintptr_t>(mgr, kMgrSites);
			if (!reg) {
				logger::Write("Landing: planet %08X has no landing-site registry", planet);
				return;
			}
			const auto count = At<std::uint32_t>(reg, 0x08);
			const auto entries = At<std::uintptr_t>(reg, 0x10);
			logger::Write("Landing: planet %08X has %u hand-built landing sites", planet, count);
			std::vector<AtlasSite> atlas;
			for (std::uint32_t i = 0; entries && i < count && i < 512; ++i) {
				const auto e = entries + i * 24;
				const double lat = At<double>(e, 0), lon = At<double>(e, 8);
				const auto id = At<std::uint32_t>(e, 0x10);
				// The entry id is the site's own form: a worldspace (type 'V') for a separate place such as a city, otherwise
				// a reference in this worldspace (landing markers, points of interest).
				const auto form = EngineFn<std::uintptr_t (*)(std::uint32_t)>(kLookupForm)(id);
				const std::uint8_t type = form ? At<std::uint8_t>(form, 0x2E) : 0;
				// 0.51.2 counted every worldspace-type site, but all 57 entries are worldspace forms, the generated FF... ones
				// included (each generated landing area is its own worldspace). Only hand-built sites (ids from the game
				// data, below 0xFF000000) are places to land into; on Akila these are 002639EC and 0027CFA2.
				const bool handBuilt = id < 0xFF000000u;
				if (type == kFormTypeWorldspace && handBuilt) {
					g.siteWorlds.insert(id);
				}
				atlas.push_back({ lat, lon, id, type == kFormTypeWorldspace && handBuilt, 0 });
				if (handBuilt || i < 8) {
					logger::Write("Landing:   site %u at lat/long %.6f, %.6f: form %08X type '%c' (0x%02X)%s", i, lat, lon, id, type >= 0x20 && type < 0x7F ? type : '?',
						type, (type == kFormTypeWorldspace && handBuilt) ? " = hand-built worldspace (entered by landing)" : (handBuilt ? "" : " (generated)"));
				}
			}
			logger::Write("Landing: %zu of them are hand-built worldspaces (entered by landing); the generated ones are not", g.siteWorlds.size());
			telemetry::Record("landing_sites", "\"planet\":\"%08X\",\"count\":%u,\"worlds\":%zu", planet, count, g.siteWorlds.size());
			BuildAtlasAsync(planet, std::move(atlas));
		}

		unsigned g_landings = 0;  // automatic landings this session; stops at kMaxLandings (a safety net against loops)
		constexpr unsigned kMaxLandings = 5;

		bool Land(std::uintptr_t mgr, const double ll[2], const char* why)
		{
			if (g_landings >= kMaxLandings) {
				static bool told = false;
				if (!told) {
					told = true;
					logger::Write("Landing: %u automatic landings this session; no more until the next start (%s skipped)", g_landings, why);
				}
				return false;
			}
			++g_landings;
			alignas(16) std::uint8_t desc[0x100] = {};
			double target[2] = { ll[0], ll[1] };
			const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			logger::Write("Landing: %s: landing at lat/long %.6f, %.6f on planet %08X (the game's surface landing)", why, ll[0], ll[1], planet);
			telemetry::Record("landing_surface", "\"why\":\"%s\",\"lat\":%.8f,\"lon\":%.8f", why, ll[0], ll[1]);
			EngineFn<void* (*)(void*, std::uint32_t, double*, std::uint32_t, char, int)>(kSurfaceLand)(desc, planet, target, 0, 0, 0);
			g.lastLanding = GetTickCount64();
			g.outsideSince = 0;
			return true;
		}
	}

	std::uint32_t SiteAt(const double latlong[2])
	{
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const auto reg = mgr ? At<std::uintptr_t>(mgr, kMgrSites) : 0;
		if (!reg || !At<std::uint32_t>(reg, 0x08)) {
			return 0;
		}
		alignas(16) std::uint8_t out[32] = {};
		EngineFn<void* (*)(std::uintptr_t, void*, const double*)>(kSiteLookup)(reg, out, latlong);
		return *reinterpret_cast<const std::uint32_t*>(out + 16);
	}

	namespace
	{
		// Great-circle distance between two lat/long pairs (radians), metres; -1 without a planet radius.
		double Distance(const double a[2], const double b[2])
		{
			const double r = handoff::PlanetRadius();
			if (r <= 0) {
				return -1.0;
			}
			const double s1 = std::sin((b[0] - a[0]) / 2), s2 = std::sin((b[1] - a[1]) / 2);
			const double h = s1 * s1 + std::cos(a[0]) * std::cos(b[0]) * s2 * s2;
			return 2.0 * r * std::asin(std::min(1.0, std::sqrt(h)));
		}

		// Planner (0.56.0): the mover's predicted path (6 s, 20 s, 60 s ahead, capped at 8 km) against the atlas. Reports
		// the zone ahead and the hand-built sites it holds, and the nearest hand-built site the path approaches, so the
		// next zone's site is known (and announced) before arrival. Entering still uses the walked-in landing on foot.
		void PlanAhead(std::uintptr_t mgr, std::uintptr_t player, ULONGLONG now)
		{
			if (!g_atlas.ready.load()) {
				return;
			}
			const auto vehicle = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player);
			const auto mover = vehicle ? vehicle : player;
			const auto p = At<game::Vec3>(mover, off::kRefPosition);
			static game::Vec3 lastP{};
			static ULONGLONG lastT = 0;
			static float vx = 0, vy = 0;
			if (lastT && now > lastT) {
				const float dt = float(now - lastT) / 1000.0f;
				const float jx = p.x - lastP.x, jy = p.y - lastP.y;
				if (dt < 2.0f && std::hypot(jx, jy) < 500.0f) {  // a shift or a teleport is not motion
					vx = 0.6f * vx + 0.4f * jx / dt;
					vy = 0.6f * vy + 0.4f * jy / dt;
				}
			}
			lastP = p;
			lastT = now;
			const auto latlongAt = [&](float x, float y, double out[2]) {
				alignas(16) double ll[4] = {};
				const float fp[3] = { x, y, p.z };
				EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, ll, fp);
				out[0] = ll[0];
				out[1] = ll[1];
				return std::isfinite(out[0]) && std::isfinite(out[1]);
			};
			double here[2];
			if (!latlongAt(p.x, p.y, here)) {
				return;
			}
			const float speed = std::hypot(vx, vy);
			constexpr float kHorizons[3] = { 6.0f, 20.0f, 60.0f };
			double ahead[3][2];
			for (int i = 0; i < 3; ++i) {
				const float t = speed > 1.0f ? std::min(kHorizons[i], 8000.0f / speed) : 0.0f;
				if (!latlongAt(p.x + vx * t, p.y + vy * t, ahead[i])) {
					ahead[i][0] = here[0];
					ahead[i][1] = here[1];
				}
			}
			std::uint64_t zoneHere = 0, zoneAhead = 0;
			handoff::ZoneKeyAtLatLong(here[0], here[1], zoneHere);
			handoff::ZoneKeyAtLatLong(ahead[1][0], ahead[1][1], zoneAhead);

			std::uint32_t bestId = 0;
			double bestNow = 1e18, bestAhead = 1e18;
			double bestLL[2] = { 0, 0 };
			std::vector<std::uint32_t> inAheadZone;
			{
				std::lock_guard lk(g_atlas.lock);
				if (g_atlas.planet != At<std::uint32_t>(mgr, off::kMgrPlanetId)) {
					return;
				}
				for (const auto& st : g_atlas.sites) {
					if (!st.world) {
						continue;
					}
					const double sl[2] = { st.lat, st.lon };
					const double dNow = Distance(here, sl);
					double dAhead = dNow;
					for (int i = 0; i < 3; ++i) {
						dAhead = std::min(dAhead, Distance(ahead[i], sl));
					}
					if (dNow >= 0 && dNow < bestNow) {
						bestNow = dNow;
						bestAhead = dAhead;
						bestId = st.id;
						bestLL[0] = st.lat;
						bestLL[1] = st.lon;
					}
				}
				if (const auto z = g_atlas.byZone.find(zoneAhead); z != g_atlas.byZone.end() && zoneAhead != zoneHere) {
					for (const auto i : z->second) {
						if (g_atlas.sites[i].world) {
							inAheadZone.push_back(g_atlas.sites[i].id);
						}
					}
				}
			}
			static std::uint64_t toldZone = 0;
			if (zoneAhead != zoneHere && zoneAhead != toldZone) {
				toldZone = zoneAhead;
				logger::Write("Atlas: zone ahead %016llX (20 s at %.0f m/s) holds %zu hand-built site(s)", static_cast<unsigned long long>(zoneAhead), speed,
					inAheadZone.size());
				for (const auto id : inAheadZone) {
					logger::Write("Atlas:   %08X in the zone ahead", id);
				}
			}
			// Announce an approaching hand-built site once (within 6 km with the path closing on it), and again inside 1 km.
			// Per place, not per site (0.57.1): a city is several sites a few hundred metres apart (Cydonia: 8) and the
			// nearest one alternated, repeating the notice every second. A site within 2 km of the last announced one is
			// the same place; a new place, or 5 minutes, announces again.
			static double toldFarLL[2] = { 1e9, 1e9 }, toldNearLL[2] = { 1e9, 1e9 };
			static ULONGLONG toldFarAt = 0, toldNearAt = 0;
			static ULONGLONG lastLog = 0;
			const auto samePlace = [&](const double told[2], ULONGLONG at) {
				return told[0] < 1e8 && Distance(told, bestLL) >= 0 && Distance(told, bestLL) < 2000.0 && now - at < 300000;
			};
			if (bestId && bestNow < 6000.0 && bestAhead < bestNow - 1.0 && !samePlace(toldFarLL, toldFarAt)) {
				toldFarLL[0] = bestLL[0];
				toldFarLL[1] = bestLL[1];
				toldFarAt = now;
				notify::Show("Hand-built site ahead: %.1f km", bestNow / 1000.0);
				logger::Write("Atlas: approaching hand-built site %08X: %.0f m now, %.0f m on the predicted path (%.0f m/s)", bestId, bestNow, bestAhead, speed);
			}
			if (bestId && bestNow < 1000.0 && !samePlace(toldNearLL, toldNearAt)) {
				toldNearLL[0] = bestLL[0];
				toldNearLL[1] = bestLL[1];
				toldNearAt = now;
				if (vehicle) {
					notify::Show("Hand-built site %.0f m ahead: continue on foot to enter", bestNow);
				}
				else {
					notify::Show("Hand-built site %.0f m ahead", bestNow);
				}
				logger::Write("Atlas: hand-built site %08X within %.0f m%s", bestId, bestNow, vehicle ? " (in a vehicle: entering needs on foot)" : "");
			}
			if (now - lastLog >= 10000) {
				lastLog = now;
				logger::Write("Atlas: at %.6f, %.6f zone %016llX; nearest hand-built site %08X %.0f m (path min %.0f m)", here[0], here[1],
					static_cast<unsigned long long>(zoneHere), bestId, bestNow, bestAhead);
			}
		}
	}

	void RequestLandHere(const char* why)
	{
		if (!g.requested.exchange(true)) {
			g.requestWhy = why;
		}
	}

	void Tick()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 500) {
			return;
		}
		last = now;
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto a = anchor::Current();
		if (mgr && player && At<std::uint8_t>(mgr, off::kMgrState) == 2 && a.cell && At<std::uintptr_t>(a.cell, off::kCellWorldspace)) {
			ReadRegistry(mgr);
			PlanAhead(mgr, player, now);  // on foot and in vehicles
		}
		if (!mgr || !player || At<std::uint8_t>(mgr, off::kMgrState) != 2 || a.ref != player || !a.cell) {
			return;  // only on foot, outdoors, with the planet settled
		}
		if (EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player)) {
			return;  // in a vehicle: a landing would leave it behind
		}
		const auto ws = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
		if (!ws) {
			return;
		}
		ReadRegistry(mgr);
		double ll[2];
		if (!PlayerLatLong(mgr, player, ll)) {
			return;
		}
		const auto wsForm = At<std::uint32_t>(ws, 0x28);
		const auto site = SiteAt(ll);
		// 0.51.0 read the registry's form id as the site's worldspace; it is the site's own form (mostly generated
		// references), so "site != worldspace" held everywhere and the player was landed three times in a row from the
		// load. Now only sites whose form IS a worldspace count, and only walked-in transitions land (0.51.2).
		if (!kAutoLanding) {
			(void)wsForm;
			return;
		}
		// Only sites that are a separate worldspace count; generated sites and markers are places in this worldspace.
		const std::uint32_t worldSite = (site && g.siteWorlds.count(site)) ? site : 0;
		const bool inSiteWorld = g.siteWorlds.count(wsForm) != 0;
		const bool gapOk = !g.lastLanding || now - g.lastLanding >= kMinGapMs;
		if (!worldSite) {
			if (!g.outsideAll) {
				g.outsideAll = now;
			}
		}
		// A requested landing (another cube face).
		if (g.requested.load() && gapOk) {
			g.requested = false;
			notify::Show("Crossing onto a new part of the planet...");
			Land(mgr, ll, g.requestWhy.c_str());
			g.lastSite = 0;
			return;
		}
		// Entering: walked from outside every separate-worldspace footprint (for a few seconds) into one, from another
		// worldspace. Never at load (the outside timer starts only once the player has been seen outside).
		if (worldSite && worldSite != wsForm) {
			const bool walkedIn = g.outsideAll && now - g.outsideAll >= kOutsideFirstMs;
			const bool sameRecent = g.lastSite == worldSite && g.lastLanding && now - g.lastLanding < kSameSiteMs;
			if (g.announcedSite != worldSite) {
				g.announcedSite = worldSite;
				logger::Write("Landing: in the footprint of the separate-worldspace site %08X (player in %08X)%s", worldSite, wsForm,
					walkedIn ? "" : " - not entered: the player did not walk in from outside");
			}
			// City guard: the landing itself is in a hand-built site and the player is still near it.
			double landed[2];
			recentre::OriginalLandingLatLong(landed);  // the real landing (mgr+0x98 now follows shifts, 0.55.0)
			const auto landedSite = SiteAt(landed);
			const double fromLanding = Distance(landed, ll);
			const bool inLandedCity = landedSite && g.siteWorlds.count(landedSite) && fromLanding >= 0 && fromLanding < kCityGuardMetres;
			if (walkedIn && inLandedCity) {
				static std::uint32_t told = 0;
				if (told != worldSite) {
					told = worldSite;
					logger::Write("Landing: not entering %08X: the landing is in the hand-built site %08X %.0f m away (city guard, %.0f m)", worldSite, landedSite,
						fromLanding, kCityGuardMetres);
				}
				return;
			}
			if (walkedIn && gapOk && !sameRecent) {
				notify::Show("Arriving...");
				g.lastSite = worldSite;
				g.outsideAll = 0;
				Land(mgr, ll, "entering a separate-worldspace site");
			}
			return;
		}
		g.announcedSite = worldSite;
		// Leaving: in a site's worldspace, outside every footprint for a few seconds.
		if (inSiteWorld && !worldSite) {
			if (!g.outsideSince) {
				g.outsideSince = now;
			}
			if (now - g.outsideSince >= kExitDwellMs && gapOk) {
				notify::Show("Leaving...");
				g.lastSite = wsForm;
				Land(mgr, ll, "leaving a separate-worldspace site");
			}
			return;
		}
		g.outsideSince = 0;
	}
}

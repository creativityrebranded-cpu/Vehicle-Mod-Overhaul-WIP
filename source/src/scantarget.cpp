#include "scantarget.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "mappoints.h"
#include "scannercontext.h"
#include "ramlogic.h"
#include "targetinput.h"
#include "npcvehicles.h"
#include "vehiclehits.h"
#include "realvehicles.h"
#include "hoverui.h"
#include "log.h"
#include "notify.h"
#include "prefetch.h"
#include "vehicleflight.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace scantarget
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }

		constexpr int kSlot = 21;                       // prefetch ray slots 21..23 are free (0..20 pilots, 24..44 sweep rays)
		constexpr std::uint64_t kSolid = 0xA5C12671Eull;  // the solid Havok layers (vehicleflight.cpp kSolidLayers)
		constexpr std::uint64_t kMask = kSolid & ~((1ull << 8) | (1ull << 30) | (1ull << 33));  // no characters
		constexpr float kRange = 1200.0f;               // m: how far the ray looks (real hits only exist where collision is loaded)
		constexpr float kMaxEstimate = 1000.0f;         // m: the farthest estimated target (the user, 2026-10-20: "as far as ~1K max")
		constexpr float kEstHeight = 15.0f;             // m: the eye height the estimate maps the view angle with
		constexpr float kEye = 1.6f;                    // m: eye height above the player's origin
		constexpr ULONGLONG kAskEveryMs = 70;
		constexpr std::uint32_t kLookupForm = 0x005DE3B0;

		struct Target
		{
			bool valid = false, estimated = false, snapped = false, outpost = false;
			float x = 0, y = 0, z = 0, dist = 0;
			std::uint32_t formId = 0;
			std::string name;
		};

		int g_key = 'T';
		bool g_keyRead = false;
		targetinput::Input g_input;
		Target g_pressedTarget;
		std::uint32_t g_pressedActor = 0;
		bool g_pressedInScanner = false, g_lastScanner = false;
		bool g_pressedCombat = false;
		bool g_lastDriving = false;
		std::uint32_t g_pressedFocus = 0;
		bool g_pressedPlacement=false;
		double g_pressedHeading=0;
		ULONGLONG g_lastAsk = 0, g_setAt = 0;
		float g_setDist = 0;
		bool g_setEstimated = false;
		std::string g_setName;

		// Marker chain (2026-10-20, the user): a press while a trip to the current marker is on queues the point; when the player's vehicle is
		// kSwapEtaS from the current marker (time to arrival from its speed) or kSwapNear metres, the game's marker moves to the next point and Auto,
		// which follows the marker, flies on without stopping. A double tap of T clears the entire route instead of selecting a new point.
		constexpr double kSwapEtaS = 4.0, kSwapNear = 15.0, kArrivedNear = 20.0;
		struct Point { float x, y, z; std::string name; };
		std::vector<Point> g_chain;      // queued points, next first
		double g_vx = 0, g_vy = 0, g_lastX = 0, g_lastY = 0;  // the vehicle's smoothed speed
		ULONGLONG g_lastAt = 0;
		std::uint32_t g_lastVeh = 0;
		Point g_current{};               // the point the game's marker was last moved to by us
		bool g_haveCurrent = false;
		int g_skipKey=-1, g_retryKey=-1;
		bool g_retryDown=false, g_skipDown=false, g_routeCleared=false;
		ULONGLONG g_clearedAt=0;
		ULONGLONG g_lostSince=0;

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		void ReadKey()
		{
			if (g_keyRead) return;
			g_keyRead = true;
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const std::wstring ini = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\seamless.ini";
				CoTaskMemFree(docs);
				g_key = int(GetPrivateProfileIntW(L"scanner", L"key", 'T', ini.c_str()));
			}
			logger::Write("Scanner marker: key %d (%c)", g_key, g_key >= 32 && g_key < 127 ? char(g_key) : '?');
		}

		uptr RefById(std::uint32_t id)
		{
			if (!id) return 0;
			__try {
				const uptr r = reinterpret_cast<uptr (*)(std::uint32_t)>(game::Base() + kLookupForm)(id);
				return r && At<std::uint32_t>(r, off::kFormId) == id ? r : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		uptr HighlightedActor() {
			const auto pick=hoverui::ScannerPickContext();uptr ref=0;
			if(pick.handle) {
				uptr*out=&ref;struct Lookup{uptr vtbl;uptr**capture;}lookup{game::Base()+addr::kHandleToRefFunctorVtbl,&out};
				reinterpret_cast<void(*)(uptr,std::uint32_t,void*)>(game::Base()+addr::kHandleLookup)(
					*reinterpret_cast<uptr*>(game::Base()+addr::kRefHandleTablePtr),pick.handle,&lookup);
				if(ref)reinterpret_cast<void(*)(uptr)>(game::Base()+addr::kRefRelease)(ref);
			}
			if(!ref)ref=RefById(pick.form);
			return ref&&npcvehicles::LooksLikeActor(ref)&&!npcvehicles::IsDead(ref)?ref:0;
		}
		// The game's own map points (outposts, landmarks, markers) of this planet, as world points near the player; refreshed every few seconds and when the player has moved far.
		std::vector<mappoints::Point> g_pois;
		ULONGLONG g_poiAt = 0;
		float g_poiFrom[2] = { 0, 0 };
		bool g_poiHave = false;

		// Pulls the target to a map point that lies on the view line (horizontal offset from the line <= max(80 m, 3% of its distance)), at any distance. A real hit that is
		// clearly nearer than the point is an obstacle and wins. Only points of the planet the player is on exist here (mappoints::WorldPoints).
		void SnapToMapPoint(Target& t, const game::Vec3& pp, float dx, float dy, ULONGLONG now, const hoverui::ScannerDestination& selection)
		{
			const float hl = std::hypot(dx, dy);
			if (hl < 0.02f) return;   // looking straight up or down
			if (now - g_poiAt > 4000 || (g_poiHave && std::hypot(pp.x - g_poiFrom[0], pp.y - g_poiFrom[1]) > 3000.0f)) {
				g_poiAt = now;
				const float p3[3] = { pp.x, pp.y, pp.z };
				g_poiHave = mappoints::WorldPoints(p3, g_pois);
				g_poiFrom[0] = pp.x;
				g_poiFrom[1] = pp.y;
				if (!g_poiHave) g_pois.clear();
			}
			if (g_pois.empty()) return;
			const std::string& selected = selection.name;
			const float ux = dx / hl, uy = dy / hl;
			const float hitDist = t.valid && !t.estimated ? float(std::hypot(double(t.x) - pp.x, double(t.y) - pp.y)) : -1.0f;
			const mappoints::Point* best = nullptr;
			float bestLat = 1e30f;
			for (const auto& q : g_pois) {
				const float rx = q.x - pp.x, ry = q.y - pp.y;
				const float along = rx * ux + ry * uy;
				const float lateral = std::fabs(rx * uy - ry * ux);
				const float score = float(scannercontext::PoiScore(q.name, selected, along, lateral, hitDist,
					std::hypot(rx, ry), selection.distance));
				if (score < bestLat) { bestLat = score; best = &q; }
			}
			if (!best) return;
			t.valid = true;
			t.estimated = false;
			t.snapped = true;
			t.outpost = best->outpost;
			t.x = best->x;
			t.y = best->y;
			t.z = pp.z;
			t.formId = 0;
			t.name = best->name;
		}

		// The view line: the camera root's world transform (NiAVObject: rotation +0x80, 3 rows of 16 bytes; translate +0xB0, as shipmodel reads it).
		// Forward = row 1 (+0x90, +0x94, +0x98), the reading vehicleflight's CameraHeadings defaults to (the user confirmed it; column 1 turned mirrored).
		// (2026-10-19, the user: the marker did not land where they looked: the ray used the body's rotation, which in third person is not the
		// view and never pitches.)
		bool CameraRay(const game::Vec3& pp, float* origin, float& dx, float& dy, float& dz)
		{
			__try {
				const uptr cam = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCameraPtr);
				const uptr root = cam ? At<uptr>(cam, 0x08) : 0;
				if (!root) return false;
				const float fx = At<float>(root, 0x90), fy = At<float>(root, 0x94), fz = At<float>(root, 0x98);
				const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
				if (!std::isfinite(len) || len < 0.5f || len > 1.5f) return false;
				dx = fx / len; dy = fy / len; dz = fz / len;
				const auto ct = At<game::Vec3>(root, 0xB0);
				// The camera's own position when it is near the player (third person: behind the shoulder), so the ray runs through the screen centre.
				if (std::isfinite(ct.x) && std::isfinite(ct.y) && std::isfinite(ct.z) && std::hypot(double(ct.x) - pp.x, double(ct.y) - pp.y) < 20.0 && std::fabs(double(ct.z) - pp.z) < 20.0) {
					origin[0] = ct.x; origin[1] = ct.y; origin[2] = ct.z;
				}
				else {
					origin[0] = pp.x; origin[1] = pp.y; origin[2] = pp.z + kEye;
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The point the player is looking at.
		Target Look(uptr player, ULONGLONG now, bool placement = false)
		{
			Target t;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			float from[3] = { pp.x, pp.y, pp.z + kEye };
			float dx, dy, dz;
			if (!CameraRay(pp, from, dx, dy, dz)) {  // no camera: the body's facing
				const auto rot = At<game::Vec3>(player, off::kRefRotation);
				const float cp = std::cos(rot.x), sp = std::sin(rot.x);
				dx = std::sin(rot.z) * cp; dy = std::cos(rot.z) * cp; dz = -sp;  // rotation x > 0 looks down
			}
			static ULONGLONG lastView = 0;
			if (logger::Verbose() && now - lastView > 2000) {  // proof of the view reading: the ray's heading next to the body's
				lastView = now;
				logger::Write("Scanner marker: view heading %.3f, pitch %.3f, from (%.1f, %.1f, %.1f); body heading %.3f", std::atan2(dx, dy), std::asin(std::clamp(dz, -1.0f, 1.0f)),
					from[0], from[1], from[2], At<game::Vec3>(player, off::kRefRotation).z);
			}
			const float to[3] = { from[0] + dx * kRange, from[1] + dy * kRange, from[2] + dz * kRange };
			if (now - g_lastAsk >= kAskEveryMs) {
				g_lastAsk = now;
				prefetch::RequestRay(kSlot, from, to, kMask);
			}
			prefetch::RayHit h;
			unsigned age = 0;
			if (prefetch::LatestRay(kSlot, h, age) && h.hit && age < 30 && std::isfinite(h.x)) {
				// The surface the ray landed on, as it is (2026-10-21, the user: destinations as exact as the recorded placements;
				// the old 40 m pull to a structure's own origin put the marker in the middle of buildings).
				t = { true, false, false, false, h.x, h.y, h.z, 0, h.formId };
			}
			else if (!placement && dz < 0.34f) {
				// Nothing loaded that far (collision only exists ~250 m around the player): an estimate along the view heading, marked "~". The old flat
				// plane at eye height needed a fraction of a degree under the horizon for anything past ~250 m (2026-10-20, the user: "only maxing out to
				// ~250m"); now the angle under the horizon maps to the distance as if the eye were kEstHeight up: 1 degree ~860 m, level or up to
				// ~20 degrees up kMaxEstimate. Terrain heights out there are not known: the marker sits at the player's height (the autopilot holds its own).
				const float hl = std::hypot(dx, dy);
				if (hl > 0.05f) {
					const float down = -dz / hl;  // tan of the angle under the horizon
					const float s = down > 1e-4f ? std::min(kMaxEstimate, kEstHeight / down) : kMaxEstimate;
					if (s > 1.0f) t = { true, true, false, false, pp.x + dx / hl * s, pp.y + dy / hl * s, pp.z, 0, 0 };
				}
			}
			// Authoring records the physical surface, never a building origin,
			// navigation landmark or estimated terrain height.
			if (placement) {
				if (t.valid) t.dist = float(scannercontext::Range3D(double(t.x)-pp.x,double(t.y)-pp.y,double(t.z)-pp.z));
				return t;
			}
			const auto selected = hoverui::ScannerDestinationContext();
			if (!selected.name.empty()) {
				// The named marker owns the aim. Until map coordinates are available, use the
				// game's measured range along the camera ray and label that projection as estimated.
				if (selected.distance > 0) {
					t = {true, true, false, false,
						float(from[0] + dx * selected.distance), float(from[1] + dy * selected.distance),
						float(from[2] + dz * selected.distance), 0, 0, selected.name};
				} else {
					// Retain a ray/estimate only as spatial context; its position is still uncertain.
					t.estimated = true;
					t.name = selected.name;
				}
			}
			// A map point only stands in where nothing real was hit (past loaded collision) or for the game's selected marker; a
			// real surface hit is never pulled to a landmark near the view line.
			if (!selected.name.empty() || !t.valid || t.estimated) SnapToMapPoint(t, pp, dx, dy, now, selected);
			if (t.valid) {
				t.dist = selected.distance > 0 && !selected.name.empty() ? float(selected.distance) :
					float(scannercontext::Range3D(double(t.x)-pp.x,double(t.y)-pp.y,double(t.z)-pp.z));
			}
			return t;
		}

		void Text(const Target& t, bool marked, char* out, size_t cap)
		{
			if (!t.valid) {
				std::snprintf(out, cap, marked ? "MARKER SET" : "NO TARGET");
				return;
			}
			char d[32];
			if (t.dist >= 1000.0f) std::snprintf(d, sizeof(d), "%s%.2f km", t.estimated ? "~" : "", t.dist / 1000.0f);
			else std::snprintf(d, sizeof(d), "%s%.0f m", t.estimated ? "~" : "", t.dist);
			std::snprintf(out, cap, "%s  %s%s%s", marked ? "MARKER SET" : "TARGET", d,
				t.name.empty() && !t.snapped ? "" : "  ", t.name.empty() ? (t.snapped ? (t.outpost ? "(outpost)" : "(map point)") : "") : t.name.c_str());
		}
	}

	namespace
	{
		// The player's own vehicle, its distance to the current marker and its time to arrival there; false: no vehicle or no marker.
		bool TripState(ULONGLONG now, double& left, double& eta)
		{
			const uptr v = RefById(vehicleflight::OwnRoverId());
			if (!v) return false;
			const auto vp = At<game::Vec3>(v, off::kRefPosition);
			const std::uint32_t id = At<std::uint32_t>(v, off::kFormId);
			if (id != g_lastVeh || !g_lastAt || now - g_lastAt > 1000) {
				g_vx = g_vy = 0;
			}
			else if (now > g_lastAt) {
				const double dt = (now - g_lastAt) / 1000.0;
				const double a = std::min(1.0, dt * 3.0);
				g_vx += ((vp.x - g_lastX) / dt - g_vx) * a;
				g_vy += ((vp.y - g_lastY) / dt - g_vy) * a;
			}
			g_lastVeh = id;
			g_lastX = vp.x;
			g_lastY = vp.y;
			g_lastAt = now;
			double mx = 0, my = 0;
			if (!vehicleflight::MarkerPoint(v, mx, my)) return false;
			left = std::hypot(mx - vp.x, my - vp.y);
			const double toward = left > 0.1 ? (g_vx * (mx - vp.x) + g_vy * (my - vp.y)) / left : 0.0;  // speed toward the marker
			eta = toward > 0.5 ? left / toward : 1e9;
			return true;
		}

		bool PlaceMarker(const Point& q, const char* why)
		{
			const float pos[3] = { q.x, q.y, q.z };
			const bool ok = readout::SetPlayerMarker(pos);
			logger::Write("Scanner marker: %s [%s] (%.0f, %.0f, %.0f) -> %s; %zu more queued", why, q.name.c_str(), q.x, q.y, q.z, ok ? "marker placed" : "FAILED", g_chain.size());
			if (ok) {
				vehicleflight::FreshScannerTarget();
				g_current = q;
				g_haveCurrent = true;
				g_routeCleared = false;
			}
			return ok;
		}

		bool AdvanceStop(bool skipped)
		{
			if(g_chain.empty()) return false;
			if(!triplogic::Advance(g_chain,g_current,g_haveCurrent,
				vehicleflight::GetAutoMode()==vehicleflight::AutoMode::Patrol,skipped,
				[&](const Point& next){return PlaceMarker(next,skipped ? "skip to" : "next stop");})) return false;
			const auto& next=g_current;
			g_setAt=0;
			char message[256];
			std::snprintf(message,sizeof(message),"Next stop: %s",next.name.empty() ? "Map point" : next.name.c_str());
			notify::Show(message);
			return true;
		}

		// Drops the queued route; the game's marker stays where it is and no longer drives the vehicle (until a new selection).
		void DropRoute()
		{
			const auto vehicle=RefById(vehicleflight::OwnRoverId());
			double mx=0,my=0;
			if(vehicle && vehicleflight::MarkerPoint(vehicle,mx,my)) {g_current.x=float(mx); g_current.y=float(my);}
			g_chain.clear(); g_haveCurrent=false; g_setAt=0;
			g_routeCleared=true; g_clearedAt=GetTickCount64();
		}

		void ClearRoute()
		{
			DropRoute();
			vehicleflight::CancelScannerTrip();
			notify::Show("Route cleared | holding position");
		}

		std::string RouteName(const std::string& name, const char* fallback)
		{
			if(name.empty()) return fallback;
			if(name.size()<=42) return name;
			std::size_t end=42;
			while(end && (static_cast<unsigned char>(name[end])&0xC0)==0x80) --end;
			return name.substr(0,end)+"...";
		}

		void RouteTick(ULONGLONG now)
		{
			const uptr vehicle=RefById(vehicleflight::OwnRoverId());
			double mx=0,my=0;
			const bool marker=vehicle && vehicleflight::MarkerPoint(vehicle,mx,my);
			if(marker && (g_haveCurrent || g_routeCleared) && std::hypot(mx-g_current.x,my-g_current.y)>2.0) {
				// An externally replaced marker must not continue the old route under an old name.
				g_chain.clear(); g_haveCurrent=false; g_routeCleared=false;
			}
			if(g_retryKey<0) g_retryKey=controlmap::ClaimFirst("route","Retry trip",{VK_OEM_1,VK_F8,VK_F12});
            if(g_skipKey<0) g_skipKey=controlmap::ClaimFirst("route","Skip stop",{VK_OEM_4,VK_F9,VK_F10,'K'});
			const bool skip=Focused() && g_skipKey && (GetAsyncKeyState(g_skipKey)&0x8000)!=0;
			const bool retry=Focused() && g_retryKey && (GetAsyncKeyState(g_retryKey)&0x8000)!=0;
            const bool available=(marker || !g_chain.empty()) && !g_routeCleared;
			if(retry && !g_retryDown && available) vehicleflight::RetryScannerTrip();
			else if(skip && !g_skipDown && available) {
				if(g_chain.empty()) ClearRoute();
				else if(AdvanceStop(true)) vehicleflight::RequestScannerTrip();
			}
			g_skipDown=skip; g_retryDown=retry;
			if(g_routeCleared) {
				hoverui::SetRoute(now-g_clearedAt<3000 ? "ROUTE CLEARED\nHolding position" : "");
				return;
			}
			if(!marker) {
				if(!g_lostSince) g_lostSince=now;
				if(now-g_lostSince>3000) { g_chain.clear(); g_haveCurrent=false; }
				hoverui::SetRoute("");
				return;
			}
			g_lostSince=0;
			const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
			const auto progress=vehicleflight::ProgressFor(vehicleflight::OwnRoverId());
			const bool matching=progress.active && std::hypot(progress.x-mx,progress.y-my)<2.0;
			const bool orbit=vehicleflight::GetAutoMode()==vehicleflight::AutoMode::Orbit;
			const char* status=matching ? triplogic::Text(progress.phase) : orbit && progress.active ? "Orbiting" : "Waiting for driver";
			double left=0,eta=0;
			TripState(now,left,eta);
			const std::string name=RouteName(g_haveCurrent ? g_current.name : "","Map marker");
			char detail[160];
			if(eta>0 && eta<3600 && matching && progress.phase==triplogic::Phase::Travelling)
				std::snprintf(detail,sizeof(detail),"%s | %.0f m | ETA %.0f s",status,std::hypot(mx-pos.x,my-pos.y),eta);
			else std::snprintf(detail,sizeof(detail),"%s | %.0f m",status,std::hypot(mx-pos.x,my-pos.y));
			std::string panel="Route: "+name+"\n"+detail;
			for(std::size_t i=0;i<g_chain.size() && i<3;++i) {
				panel+="\n"+std::to_string(i+1)+". "+RouteName(g_chain[i].name,"Map point");
			}
			if(g_chain.size()>3) panel+="\n+"+std::to_string(g_chain.size()-3)+" more stops";
			const std::string retryKey=progress.phase==triplogic::Phase::Unreachable?(g_retryKey?controlmap::KeyName(g_retryKey):"unbound"):"";
			const std::string skipKey=g_skipKey?controlmap::KeyName(g_skipKey):"unbound",clearKey=controlmap::KeyName(g_key);
			hoverui::SetRoute(panel.c_str(),skipKey.c_str(),clearKey.c_str(),retryKey.c_str());
		}

		// Intermediate points keep the existing seamless handoff, but only at the destination's height.
		void ChainTick(ULONGLONG now)
		{
			if(g_chain.empty() || g_routeCleared) return;
			double left=0,eta=0;
			if(!TripState(now,left,eta) || (eta>kSwapEtaS && left>kSwapNear)) return;
			const auto progress=vehicleflight::ProgressFor(vehicleflight::OwnRoverId());
			const auto vehicle=RefById(vehicleflight::OwnRoverId());
			double mx=0,my=0;
			if(!vehicle || !vehicleflight::MarkerPoint(vehicle,mx,my) || !progress.active ||
				std::hypot(progress.x-mx,progress.y-my)>2.0 || std::fabs(progress.heightError)>2.5 ||
				progress.phase==triplogic::Phase::WaitingTerrain || progress.phase==triplogic::Phase::Unreachable) return;
			AdvanceStop(false);
		}

	}

	namespace
	{
		// A single T tap (after the double-tap window) always means "go there": the aimed place becomes the destination, or a queued stop
		// while a trip runs. The same in every context (2026-10-21, the user's control scheme): Cover, guard points and rams never change it.
		void Select(uptr player, ULONGLONG now)
		{
			const auto pick = hoverui::ScannerPickContext();
			logger::Write("Target selection (tap): native form %08X handle %08X actor %08X combat %d", pick.form, pick.handle, g_pressedActor,
				npcvehicles::IsInCombat(player) ? 1 : 0);
			const auto& selected = g_pressedTarget;
			if (!selected.valid) { notify::Show("Scanner marker: nothing to mark"); return; }
			const Point q{selected.x, selected.y, selected.z, selected.name};
			double left = 0, eta = 0;
			if (g_haveCurrent && vehicleflight::ProgressFor(vehicleflight::OwnRoverId()).active && TripState(now, left, eta) && left > kArrivedNear && !g_routeCleared) {
				g_chain.push_back(q);
				logger::Write("Scanner marker: queued stop %zu at (%.0f, %.0f, %.0f)", g_chain.size()+1, q.x, q.y, q.z);
				notify::Show("Marker queued (%zu stops)", g_chain.size()+1);
			} else if (PlaceMarker(q, "set at")) {
				vehicleflight::RequestScannerTrip();
				notify::Show(selected.name.empty() ? "Target selected | navigation started" : "Target: %s | navigation started", selected.name.c_str());
			} else { notify::Show("Target could not be set"); return; }
			g_setAt = now; g_setDist = selected.dist; g_setEstimated = selected.estimated; g_setName = selected.name;
		}

		// A T hold (1 s) acts on what was aimed at when it was pressed: a highlighted person or vehicle is rammed straight away; a place is held:
		// on foot your vehicle guards it and defends from it, driving you fly there and hold (the route is replaced).
		void Hold(bool driving, ULONGLONG now)
		{
			if (!g_pressedInScanner) { notify::Show("Aim with the scanner, then hold %s", controlmap::KeyName(g_key).c_str()); return; }
			const auto actor = RefById(g_pressedActor);
			const auto pick = hoverui::ScannerPickContext();
			logger::Write("Target selection (hold): native form %08X handle %08X actor %08X driving %d", pick.form, pick.handle, g_pressedActor, driving ? 1 : 0);
			if (actor) {
				if (vehicleflight::RequestRam(actor)) notify::Show("Ram order set");
				else notify::Show("Ram order unavailable");
				return;
			}
			const auto& selected = g_pressedTarget;
			if (!selected.valid) { notify::Show("Scanner marker: nothing to mark"); return; }
			if (!driving) {
				if (vehicleflight::SetGuardPoint(selected.x, selected.y)) notify::Show("Guard point set");
				else notify::Show("Guard point unavailable");
				return;
			}
			g_chain.clear(); g_haveCurrent = false; g_routeCleared = false;
			const Point q{selected.x, selected.y, selected.z, selected.name};
			if (!PlaceMarker(q, "hold at")) { notify::Show("Target could not be set"); return; }
			vehicleflight::RequestScannerTrip();
			notify::Show("Route replaced | holding at the target");
			g_setAt = now; g_setDist = selected.dist; g_setEstimated = selected.estimated; g_setName = selected.name;
		}
	}

	void Tick()
	{
		const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
		if (!player) return;
		const ULONGLONG now = GetTickCount64();
		ReadKey();
		RouteTick(now);
		ChainTick(now);
		const bool scanner = hoverui::IsScannerOpen();
		const bool driving = reinterpret_cast<uptr (*)(uptr)>(game::Base() + addr::kPlayerVehicle)(player) != 0;
		const Target t = scanner ? Look(player, now, !driving && realvehicles::RecordingPlacements()) : Target{};
		const bool held = (GetAsyncKeyState(g_key) & 0x8000) != 0;
		const bool combatFocusMode = !driving && (npcvehicles::CombatOf(player).fighting || npcvehicles::CombatFocus());
		const bool enabled = Focused() && (combatFocusMode || scanner || driving || g_haveCurrent || !g_chain.empty() ||
			vehicleflight::Ramming(vehicleflight::OwnRoverId()));
		// A tap captured in the scanner must not land after it closed (or a tap outside it inside the scanner).
		if (scanner != g_lastScanner || driving != g_lastDriving) {
			g_input.Cancel(held);g_lastScanner=scanner;g_lastDriving=driving;g_pressedCombat=false;
		}
		const auto action = g_input.Update(held, enabled, now);
		if(action.tap && g_pressedPlacement && scanner && !driving && realvehicles::RecordingPlacements()) {
			const auto& point=g_pressedTarget;
			if(realvehicles::RecordPlacement(point.x,point.y,point.z,g_pressedHeading,!point.valid||point.estimated,point.name.c_str())) {
				const float pos[]{point.x,point.y,point.z};readout::SetPlayerMarker(pos);
			}
		}
		else if (action.tap && g_pressedCombat && !driving) {
			notify::Show(g_pressedFocus && npcvehicles::SetCombatFocus(g_pressedFocus) ? "Focus fire set | double-tap T to clear" : "Aim at a hostile target to focus fire");
		}
		else if (action.tap && g_pressedInScanner) Select(player, now);
		else if (action.tap && driving) vehicleflight::ToggleAuto();  // outside the scanner, driving: Auto on / off
		if (action.capture) {
			g_pressedTarget = t;
			g_pressedInScanner = scanner;
			g_pressedCombat = combatFocusMode;
			g_pressedPlacement=scanner&&!driving&&realvehicles::RecordingPlacements();
			g_pressedHeading=At<game::Vec3>(player,off::kRefRotation).z;vehicleflight::ViewHeading(g_pressedHeading);
			const auto actor = scanner ? HighlightedActor() : 0;
			g_pressedActor = actor ? At<std::uint32_t>(actor, off::kFormId) : 0;
			g_pressedFocus=g_pressedActor;
			if(!g_pressedFocus)g_pressedFocus=scanner?hoverui::ScannerPickContext().form:vehiclehits::CrosshairTarget();
		}
		if (action.doubleTap) {
			if(scanner&&!driving&&realvehicles::RecordingPlacements())notify::Show("Placement recording: tap T to save a surface");
			else if(!driving && (combatFocusMode||g_pressedCombat)) {npcvehicles::SetCombatFocus(0);notify::Show("Focus cleared | automatic targeting");}
			else { logger::Write("Scanner key: double tap clears the route"); ClearRoute(); }
		}
		if (action.hold && !(scanner&&!driving&&realvehicles::RecordingPlacements())) Hold(driving, now);
		if (!scanner) { hoverui::SetScan(""); return; }
		char buf[256];
		if (g_setAt && now - g_setAt < 3000) {
			Target shown = t;
			shown.valid = true;
			shown.estimated = g_setEstimated;
			shown.dist = g_setDist;
			shown.name = g_setName;
			shown.snapped = false;
			Text(shown, true, buf, sizeof(buf));
		}
		else {
			Text(t, false, buf, sizeof(buf));
		}
		if (!g_chain.empty()) {
			const size_t n = std::strlen(buf);
			std::snprintf(buf + n, sizeof(buf) - n, "  +%zu queued", g_chain.size());
		}
		if(realvehicles::RecordingPlacements())std::snprintf(buf,sizeof(buf),"VEHICLE PLACEMENTS | %s | tap T to save",t.valid?(t.estimated?"aim closer: estimated surface":"surface ready"):"aim at a surface");
		hoverui::SetScan(buf);
		static ULONGLONG lastLog = 0;
		if (logger::Verbose() && now - lastLog > 2000) {
			lastLog = now;
			logger::Write("Scanner marker: looking at (%.0f, %.0f, %.0f), %.0f m%s, form %08X", t.x, t.y, t.z, t.dist, t.estimated ? " (estimated)" : "", t.formId);
		}
	}

	std::string KeyName() { ReadKey(); return controlmap::KeyName(g_key); }

	void EndRide()
	{
		g_chain.clear(); g_haveCurrent=false; g_setAt=0; g_setName.clear();
		g_input.Cancel(true); g_pressedActor=0; g_pressedInScanner=false;
		g_routeCleared=false; g_lastAt=0; g_lastVeh=0; g_lostSince=0;
		hoverui::SetRoute(""); hoverui::SetScan("");
	}

	void Reset()
	{
		g_input = {}; g_pressedActor = 0; g_pressedInScanner = false; g_lastScanner = false;
		g_pressedCombat=false;g_pressedFocus=0;g_lastDriving=false;
		g_pressedPlacement=false;
		g_setAt = 0;
		g_setName.clear();
		g_chain.clear();
		g_lastAt = 0;
		g_haveCurrent = false;
		g_retryDown=false; g_skipDown=false; g_routeCleared=false; g_lostSince=0;
		hoverui::SetRoute("");
		g_pois.clear();
		g_poiHave = false;
		g_poiAt = 0;
	}
}

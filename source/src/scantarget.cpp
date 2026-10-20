#include "scantarget.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "mappoints.h"
#include "scannercontext.h"
#include "npcvehicles.h"
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
		constexpr float kMagnet = 40.0f;                // m: the structure the ray hit pulls the marker to its own position inside this
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
		bool g_keyRead = false, g_keyDown = false;
		ULONGLONG g_lastAsk = 0, g_setAt = 0;
		float g_setDist = 0;
		bool g_setEstimated = false;
		std::string g_setName;

		// Marker chain (2026-10-20, the user): a press while a trip to the current marker is on queues the point; when the player's vehicle is
		// kSwapEtaS from the current marker (time to arrival from its speed) or kSwapNear metres, the game's marker moves to the next point and Auto,
		// which follows the marker, flies on without stopping. Holding the key kReplaceMs replaces the whole chain with the new point.
		constexpr double kSwapEtaS = 4.0, kSwapNear = 15.0, kArrivedNear = 20.0;
		constexpr ULONGLONG kReplaceMs = 800;
		struct Point { float x, y, z; std::string name; };
		std::vector<Point> g_chain;      // queued points, next first
		ULONGLONG g_downAt = 0;
		bool g_held = false;             // this press already acted as a hold
		double g_vx = 0, g_vy = 0, g_lastX = 0, g_lastY = 0;  // the vehicle's smoothed speed
		ULONGLONG g_lastAt = 0;
		std::uint32_t g_lastVeh = 0;
		Point g_current{};               // the point the game's marker was last moved to by us
		bool g_haveCurrent = false;
		// The Auto behaviour key (stop / orbit / patrol): the first free of these, claimed once.
		int g_modeKey = -1;
		bool g_modeDown = false;

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
		Target Look(uptr player, ULONGLONG now)
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
				t = { true, false, false, false, h.x, h.y, h.z, 0, h.formId };
				// A small magnetic pull: a structure's own position when it is close to where the ray landed.
				if (h.formId) {
					if (const uptr r = RefById(h.formId)) {
						const auto rp = At<game::Vec3>(r, off::kRefPosition);
						if (std::hypot(double(rp.x) - h.x, double(rp.y) - h.y) <= kMagnet && std::isfinite(rp.x)) {
							t.x = rp.x;
							t.y = rp.y;
							t.snapped = true;
						}
					}
				}
			}
			else if (dz < 0.34f) {
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
			SnapToMapPoint(t, pp, dx, dy, now, selected);
			if (t.valid) t.dist = float(std::hypot(double(t.x) - pp.x, double(t.y) - pp.y));
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
				g_current = q;
				g_haveCurrent = true;
			}
			return ok;
		}

		// The Auto behaviour key: stop -> orbit -> patrol -> stop.
		void ModeKey()
		{
			if (g_modeKey < 0) {
				g_modeKey = controlmap::ClaimFirst("scanner", "Auto behaviour (stop / orbit / patrol)", { VK_OEM_6, 'J', 'K', 'N' });
				logger::Write("Scanner marker: Auto behaviour key %s", g_modeKey ? controlmap::KeyName(g_modeKey).c_str() : "none free");
			}
			if (!g_modeKey) {
				hoverui::SetAutoMode("", "", false);
				return;
			}
			const bool down = Focused() && (GetAsyncKeyState(g_modeKey) & 0x8000) != 0;
			const bool pressed = down && !g_modeDown;
			if (pressed) {
				const int next = (int(vehicleflight::GetAutoMode()) + 1) % 3;
				vehicleflight::SetAutoMode(vehicleflight::AutoMode(next));
				notify::Show(next == 1 ? "Auto: orbit the marker" : next == 2 ? "Auto: patrol the marker chain (loops)" : "Auto: stop at the marker");
				// On foot with a follower free (or one already driving) and a marker set: the follower takes the vehicle there with this behaviour.
				const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
				const uptr rover = RefById(vehicleflight::OwnRoverId());
				double mx = 0, my = 0;
				if (player && rover && !reinterpret_cast<uptr (*)(uptr)>(game::Base() + addr::kPlayerVehicle)(player) && vehicleflight::MarkerPoint(rover, mx, my)) {
					const auto pp = At<game::Vec3>(player, off::kRefPosition);
					const bool crew = npcvehicles::CrewOrderOf(vehicleflight::OwnRoverId()) != npcvehicles::CrewOrder::None;
					if (crew || npcvehicles::FreeFollower(player, pp.x, pp.y, 60.0)) vehicleflight::RequestScannerTrip();
				}
			}
			g_modeDown = down;
			// The HUD button: the behaviour now, with the key (it flashes blue on a press).
			static std::string keyName;
			if (keyName.empty()) keyName = controlmap::KeyName(g_modeKey);
			const auto m = vehicleflight::GetAutoMode();
			hoverui::SetAutoMode(keyName.c_str(), m == vehicleflight::AutoMode::Orbit ? "Auto: Orbit" : m == vehicleflight::AutoMode::Patrol ? "Auto: Patrol" : "Auto: Stop", pressed);
		}

		// Every frame (scanner open or not): the hot swap to the next queued point.
		void ChainTick(ULONGLONG now)
		{
			double left = 0, eta = 0;
			const bool trip = TripState(now, left, eta);
			if (g_chain.empty()) return;
			static ULONGLONG lostSince = 0;
			if (!trip) {
				// The marker is gone (removed on the map) or the vehicle is not there: after 3 s the chain goes with it (a summon's move blinks the vehicle out).
				if (!lostSince) lostSince = now;
				if (now - lostSince < 3000) return;
				logger::Write("Scanner marker: chain of %zu dropped (no marker or no vehicle for 3 s)", g_chain.size());
				g_chain.clear();
				lostSince = 0;
				return;
			}
			lostSince = 0;
			if (eta > kSwapEtaS && left > kSwapNear) return;
			const Point next = g_chain.front();
			g_chain.erase(g_chain.begin());
			// Patrol: the point just reached goes back to the end, so the chain loops.
			if (vehicleflight::GetAutoMode() == vehicleflight::AutoMode::Patrol && g_haveCurrent) g_chain.push_back(g_current);
			char why[80];
			std::snprintf(why, sizeof(why), "next in the chain (%.0f m / %.1f s from the last) at", left, eta > 1e8 ? -1.0 : eta);
			if (PlaceMarker(next, why)) {
				char msg[48];
				if (g_chain.empty()) std::snprintf(msg, sizeof(msg), "Next marker (last)");
				else std::snprintf(msg, sizeof(msg), "Next marker (%zu more)", g_chain.size());
				notify::Show(msg);
			}
		}
	}

	void Tick()
	{
		const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
		if (!player) return;
		const ULONGLONG now = GetTickCount64();
		ModeKey();
		ChainTick(now);
		if (!hoverui::IsScannerOpen()) {
			hoverui::SetScan("");
			g_keyDown = false;
			return;
		}
		ReadKey();
		const Target t = Look(player, now);
		const bool down = Focused() && (GetAsyncKeyState(g_key) & 0x8000) != 0;
		const bool press = down && !g_keyDown;
		const bool release = !down && g_keyDown;
		g_keyDown = down;
		if (press) {
			g_downAt = now;
			g_held = false;
		}
		// Tap (acts on release): set, or queue while a trip is on. Hold: replace the chain with this point.
		const bool hold = down && !g_held && g_downAt && now - g_downAt >= kReplaceMs;
		if (hold) g_held = true;
		if (hold || (release && !g_held)) {
			if (!t.valid) {
				notify::Show("Scanner marker: nothing to mark");
				logger::Write("Scanner marker: pressed, but the view hits nothing");
			}
			else {
				const Point q{ t.x, t.y, t.z, t.name };
				double left = 0, eta = 0;
				const bool trip = TripState(now, left, eta);
				if (!hold && trip && left > kArrivedNear) {
					g_chain.push_back(q);
					g_setAt = now;
					g_setDist = t.dist;
					g_setEstimated = t.estimated;
					g_setName = t.name;
					logger::Write("Scanner marker: queued (%.0f, %.0f, %.0f) as stop %zu (current marker %.0f m from your vehicle)", t.x, t.y, t.z, g_chain.size() + 1, left);
					char msg[48];
					std::snprintf(msg, sizeof(msg), "Marker queued (%zu in the chain)", g_chain.size() + 1);
					notify::Show(msg);
				}
				else {
					if (!g_chain.empty()) logger::Write("Scanner marker: chain of %zu replaced", g_chain.size());
					g_chain.clear();
					const bool ok = PlaceMarker(q, hold ? "set (hold: chain replaced) at" : "set at");
					logger::Write("Scanner marker: %.0f m away%s%s (hit form %08X)", t.dist, t.estimated ? " (estimated)" : "", t.snapped ? ", pulled to the structure" : "", t.formId);
					if (ok) {
						g_setAt = now;
						g_setDist = t.dist;
						g_setEstimated = t.estimated;
						g_setName = t.name;
						if (t.name.empty()) notify::Show("Marker set | your vehicle is coming");
						else {
							char named[256];
							std::snprintf(named, sizeof(named), "Destination: %s | your vehicle is coming", t.name.c_str());
							notify::Show(named);
						}
						vehicleflight::RequestScannerTrip();
					}
				}
			}
		}
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
		hoverui::SetScan(buf);
		static ULONGLONG lastLog = 0;
		if (logger::Verbose() && now - lastLog > 2000) {
			lastLog = now;
			logger::Write("Scanner marker: looking at (%.0f, %.0f, %.0f), %.0f m%s, form %08X", t.x, t.y, t.z, t.dist, t.estimated ? " (estimated)" : "", t.formId);
		}
	}

	void Reset()
	{
		g_keyDown = false;
		g_setAt = 0;
		g_setName.clear();
		g_chain.clear();
		g_lastAt = 0;
		g_downAt = 0;
		g_haveCurrent = false;
		g_modeDown = false;
		g_pois.clear();
		g_poiHave = false;
		g_poiAt = 0;
	}
}

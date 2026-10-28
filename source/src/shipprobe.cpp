#include "shipprobe.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "traffic.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>

// Engine pieces (1.16.244), all from notes/NPC_SHIP_LANDING_AND_HIJACK_20261020.md section "Probe":
//  Movement message handlers (MovementMessageMap<TESObjectREFR>, registered in FUN_141893EE0):
//   FUN_1412B0E90(ship)        BeginLandingApproach -> FUN_14211AA40(ship, 1) landing-approach mode on
//   FUN_1412B0ED0(ship)        the matching "approach off" message -> FUN_14211AA40(ship, 0)
//   FUN_1412B0BF0(ship, msg)   Land (msg + 0x10 = destination form id) -> picks a marker -> FUN_1421219B0
//   FUN_1412B0F10(ship)        TakeOff (no-op for a flying NPC ship; a landed one -> FUN_1421199B0 take-off action)
//  FUN_1421219B0(ship, marker) touch-down core: ship position/rotation := marker's, links, landing events (also EnableWithLanding)
//  FUN_1412AB5A0(ref, &req)    path request submit (cancels the current one first, FUN_1412AB040)
//  Fly-to-point (the space-combat recipe FUN_140309AA0): FUN_1422C7320(0x1D0) + FUN_140309DE0 (PathingRequestOpenSpace),
//   FUN_141924960(actor, &req, location, radius(xmm3), 2, &out) sets the goal, FUN_1419229A0(actor, &req) submits.
namespace shipprobe
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Base() { return game::Base(); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(Base() + rva); }

		constexpr std::uint32_t kApproachOn = 0x012B0E90, kApproachOff = 0x012B0ED0, kLand = 0x012B0BF0, kTakeOff = 0x012B0F10;
		constexpr std::uint32_t kLandCore = 0x021219B0, kSubmit = 0x012AB5A0;
		constexpr std::uint32_t kShipOfRef = 0x02116840, kShipActor = 0x02115A60, kRefLocation = 0x00B4CEE0, kLocRelease = 0x003846F0;
		constexpr std::uint32_t kAlloc = 0x022C7320, kOpenSpaceCtor = 0x00309DE0, kSetGoal = 0x01924960, kSubmitActor = 0x019229A0;
		constexpr std::uint32_t kLinkedRef = 0x00517D60, kTakeOffCore = 0x021199B0;  // FUN_1421199B0(ship): the take-off the TakeOff message runs
		constexpr std::uint32_t kLandedAv = 0x05EA07F0;        // DAT_145EA07F0: the Landed actor value form (SetLanded FUN_142117D00)
		constexpr std::uint32_t kGoalRadius = 0x04F04D28;      // DAT_144F04D28: the radius DisableWithLanding's fly-away passes
		constexpr std::uint32_t kComReissueTime = 0x05EA5490;  // DAT_145EA5490: space combat re-issues after this many seconds
		constexpr std::uint32_t kComReissueDrift = 0x05EA54B0; // DAT_145EA54B0: ... or when its target drifted this far (m)
		constexpr std::uint32_t kSpaceOffsets = 0x061FAE88, kSpaceCount = 0x061FAE80;
		constexpr std::uint32_t kOpenSpaceVtbl = 0x04AFBB30;   // PathingRequestOpenSpace::vftable (144AFBB30)

		using Ship1_t = std::uint64_t (*)(uptr);
		using Ship2_t = std::uint64_t (*)(uptr, uptr);
		Ship1_t g_approachOn = nullptr, g_approachOff = nullptr, g_takeOff = nullptr;
		Ship2_t g_land = nullptr, g_landCore = nullptr;
		using Submit_t = char (*)(uptr, uptr*);
		Submit_t g_submit = nullptr;

		struct Track
		{
			uptr ship = 0;
			std::uint32_t id = 0;
			ULONGLONG since = 0, last = 0;
		};
		std::mutex g_lock;
		Track g_tracks[4];
		ULONGLONG g_lastSample = 0;
		int g_keySummon = 0, g_keyB = 0;
		bool g_keyDown[3] = {};

		// Test B state.
		uptr g_bShip = 0;
		ULONGLONG g_bNext = 0;
		int g_bStep = 0;
		float g_bP2[3] = {};
		float g_bStartZ = 0;

		bool Readable(uptr p, std::size_t n)
		{
			__try {
				volatile std::uint8_t x = 0;
				for (std::size_t i = 0; i < n; i += 8) x ^= *reinterpret_cast<volatile std::uint8_t*>(p + i);
				(void)x;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		float Seconds() { return float(GetTickCount64() % 10000000ull) / 1000.0f; }
		std::uint32_t FormId(uptr r) { return r && Readable(r, 0x30) ? At<std::uint32_t>(r, 0x28) : 0; }

		// The check the BeginLandingApproach handler itself uses: base object (+0x98) form type 'A' with flag bit 6.
		bool ShipLike(uptr r)
		{
			if (!r || !Readable(r, 0xA0)) return false;
			const uptr b = At<uptr>(r, 0x98);
			return b && Readable(b, 0x30) && At<char>(b, 0x2E) == 'A' && ((At<std::uint32_t>(b, 0x20) >> 6) & 1);
		}

		uptr PlayerShip()
		{
			const uptr player = *reinterpret_cast<uptr*>(Base() + addr::kPlayerCharacterPtr);
			return player ? EngineFn<uptr (*)(uptr, char)>(kShipOfRef)(player, 1) : 0;
		}

		float Landed(uptr ship)
		{
			const uptr av = *reinterpret_cast<uptr*>(Base() + kLandedAv);
			if (!av || !Readable(av, 0x30) || At<char>(av, 0x2E) != 'm') return -1.0f;
			const uptr owner = ship + 0x70;
			auto** vt = *reinterpret_cast<void***>(owner);
			return reinterpret_cast<float (*)(uptr, uptr)>(vt[1])(owner, av);
		}

		void TrackShip(uptr ship, const char* why)
		{
			if (!ship) return;
			std::lock_guard lk(g_lock);
			const ULONGLONG now = GetTickCount64();
			for (auto& t : g_tracks)
				if (t.ship == ship) { t.last = now; return; }
			Track* slot = &g_tracks[0];
			for (auto& t : g_tracks)
				if (!t.ship || t.last < slot->last) slot = &t;
			*slot = Track{ ship, FormId(ship), now, now };
			logger::Write("ShipProbe: tracking ship %08X (%s)", slot->id, why);
		}

		// ---- hooks: log, then the game's own handler ----
		std::uint64_t ApproachOnDetour(uptr ship)
		{
			logger::Write("ShipProbe: t=%.2f MSG BeginLandingApproach ship %08X", Seconds(), FormId(ship));
			TrackShip(ship, "BeginLandingApproach");
			return g_approachOn(ship);
		}
		std::uint64_t ApproachOffDetour(uptr ship)
		{
			logger::Write("ShipProbe: t=%.2f MSG approach-off ship %08X", Seconds(), FormId(ship));
			return g_approachOff(ship);
		}
		std::uint64_t TakeOffDetour(uptr ship)
		{
			logger::Write("ShipProbe: t=%.2f MSG TakeOff ship %08X landedAV %.0f", Seconds(), FormId(ship), Landed(ship));
			TrackShip(ship, "TakeOff");
			return g_takeOff(ship);
		}
		std::uint64_t LandDetour(uptr ship, uptr msg)
		{
			const std::uint32_t dest = msg && Readable(msg, 0x18) ? At<std::uint32_t>(msg, 0x10) : 0;
			const auto p = At<game::Vec3>(ship, off::kRefPosition);
			logger::Write("ShipProbe: t=%.2f MSG Land ship %08X dest %08X at (%.1f, %.1f, %.1f)", Seconds(), FormId(ship), dest, p.x, p.y, p.z);
			TrackShip(ship, "Land");
			return g_land(ship, msg);
		}
		std::uint64_t LandCoreDetour(uptr ship, uptr marker)
		{
			const auto p = At<game::Vec3>(ship, off::kRefPosition);
			const auto m = marker ? At<game::Vec3>(marker, off::kRefPosition) : game::Vec3{};
			const uptr caller = reinterpret_cast<uptr>(_ReturnAddress()) - Base() + 0x140000000ull;
			logger::Write("ShipProbe: t=%.2f LANDCORE ship %08X marker %08X from %llX: ship (%.1f, %.1f, %.1f) -> marker (%.1f, %.1f, %.1f), jump %.1f m",
				Seconds(), FormId(ship), FormId(marker), static_cast<unsigned long long>(caller), p.x, p.y, p.z, m.x, m.y, m.z,
				std::sqrt((p.x - m.x) * (p.x - m.x) + (p.y - m.y) * (p.y - m.y) + (p.z - m.z) * (p.z - m.z)));
			TrackShip(ship, "land core");
			const auto r = g_landCore(ship, marker);
			return r;
		}
		char SubmitDetour(uptr ref, uptr* req)
		{
			bool tracked = false;
			{
				std::lock_guard lk(g_lock);
				for (auto& t : g_tracks) tracked |= t.ship && t.ship == ref;
			}
			if (tracked && req && *req && Readable(*req, 0x1D0)) {
				const uptr q = *req;
				const bool open = At<uptr>(q, 0) == Base() + kOpenSpaceVtbl;
				logger::Write("ShipProbe: t=%.2f SUBMIT ship %08X request %s goal (%.1f, %.1f, %.1f) space %u%s",
					Seconds(), FormId(ref), open ? "OpenSpace" : "other", At<float>(q, 0x60), At<float>(q, 0x64), At<float>(q, 0x68), At<std::uint32_t>(q, 0x6C),
					open ? "" : "");
				if (open)
					logger::Write("ShipProbe:   OpenSpace +1C0 %.1f +1C4 %08X +1C8 %08X +1CC %u", At<float>(q, 0x1C0), At<std::uint32_t>(q, 0x1C4),
						At<std::uint32_t>(q, 0x1C8), At<std::uint8_t>(q, 0x1CC));
			}
			return g_submit(ref, req);
		}

		// ---- the 0.25 s sample ----
		void Sample(const Track& t)
		{
			const uptr s = t.ship;
			if (!Readable(s, 0x240) || FormId(s) != t.id) return;
			const auto p = At<game::Vec3>(s, off::kRefPosition);
			const auto r = At<game::Vec3>(s, off::kRefRotation);
			float rx = 0, ry = 0, rz = 0;
			const uptr loaded = At<uptr>(s, off::kRefLoadedData);
			const uptr root = loaded && Readable(loaded, 0x10) ? At<uptr>(loaded, off::kLoadedData3D) : 0;
			if (root && Readable(root, 0xC0)) { rx = At<float>(root, 0xB0); ry = At<float>(root, 0xB4); rz = At<float>(root, 0xB8); }
			traffic::Destination d;
			const bool path = traffic::DestinationOf(s, d);
			const uptr link = Readable(s + 0xC8, 8) && At<uptr>(s, 0xC8) ? EngineFn<uptr (*)(uptr, uptr, char)>(kLinkedRef)(At<uptr>(s, 0xC8), 0, 1) : 0;
			const uptr actor = EngineFn<uptr (*)(uptr)>(kShipActor)(s);
			logger::Write("ShipProbe: t=%.2f %08X pos (%.1f, %.1f, %.1f) yaw %.2f 3D (%.1f, %.1f, %.1f) landedAV %.0f actor %s link %08X path %s goal (%.1f, %.1f, %.1f) %.0f m left %u pts pkg %d",
				Seconds(), t.id, p.x, p.y, p.z, r.z, rx, ry, rz, Landed(s), actor == s ? "=ship" : (actor ? "other" : "none"), FormId(link),
				path ? "yes" : "no", d.x, d.y, d.z, d.remaining, d.points, d.packageType);
		}

		// ---- test B: fly-to-point on the ship's AI actor (space-combat recipe, no landing messages) ----
		bool FlyTo(uptr ship, const float goal[3])
		{
			const uptr actor = EngineFn<uptr (*)(uptr)>(kShipActor)(ship);  // borrowed (FUN_142115A60 releases its own temp)
			if (!actor) { logger::Write("ShipProbe: B: ship %08X has no AI actor", FormId(ship)); return false; }
			auto* req = static_cast<std::uint8_t*>(EngineFn<void* (*)(std::size_t)>(kAlloc)(0x1D0));
			if (!req) return false;
			std::memset(req, 0, 0x1D0);
			const uptr q = EngineFn<uptr (*)(void*)>(kOpenSpaceCtor)(req);
			_InterlockedIncrement(reinterpret_cast<volatile long*>(q + 0x10));
			At<float>(q, 0x1C0) = 0.0f;             // no grav-jump search
			At<std::uint32_t>(q, 0x1C4) = FormId(ship);
			At<std::uint32_t>(q, 0x1C8) = FormId(ship);
			At<std::uint8_t>(q, 0x1CC) = 0;          // no BeginLandingApproach / Land waypoints
			alignas(16) std::uint8_t buf[0x40] = {};
			auto* loc = reinterpret_cast<float*>(EngineFn<void* (*)(uptr, void*)>(kRefLocation)(ship, buf));
			float g[3] = { goal[0], goal[1], goal[2] };
			const std::uint32_t space = *reinterpret_cast<std::uint32_t*>(reinterpret_cast<uptr>(loc) + 0xC);
			const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(Base() + kSpaceCount);
			const uptr table = *reinterpret_cast<uptr*>(Base() + kSpaceOffsets);
			if (space < count && table)
				for (int i = 0; i < 3; ++i) g[i] -= At<float>(table, space * 12 + i * 4);
			std::uint8_t copy[0x18];
			std::memcpy(copy, loc, sizeof(copy));
			std::memcpy(copy, g, sizeof(g));
			std::uint64_t out = 0;
			uptr held = q;
			const float radius = *reinterpret_cast<float*>(Base() + kGoalRadius);
			EngineFn<void (*)(uptr, uptr*, void*, float, char, std::uint64_t*)>(kSetGoal)(actor, &held, copy, radius, 2, &out);
			const char ok = EngineFn<char (*)(uptr, uptr*)>(kSubmitActor)(actor, &held);
			if (const uptr cellRef = *reinterpret_cast<uptr*>(buf + 0x10)) EngineFn<void (*)(uptr)>(kLocRelease)(cellRef);
			if (_InterlockedDecrement(reinterpret_cast<volatile long*>(q + 0x10)) == 0) {
				auto** vt = *reinterpret_cast<void***>(q);
				reinterpret_cast<void (*)(uptr, int)>(vt[0])(q, 1);
			}
			logger::Write("ShipProbe: t=%.2f B: fly-to (%.1f, %.1f, %.1f) on ship %08X (actor %s): submit %s, radius %.1f",
				Seconds(), goal[0], goal[1], goal[2], FormId(ship), actor == ship ? "=ship" : "other", ok ? "accepted" : "REFUSED", radius);
			return ok != 0;
		}

		bool Eligible(uptr ship, const char* test)
		{
			// FUN_142116840(player, 1) = the ship the player is inside: equal = the player is aboard (0.22.x: loading screens), refused.
			if (!ship || ship == PlayerShip()) { logger::Write("ShipProbe: %s: no ship, or you are aboard it (get out first)", test); return false; }
			return true;
		}

		// The nearest ship-like reference in the loaded grid cells (within 3 km), landed or flying, not the one the player is in.
		uptr NearestShip()
		{
			const uptr player = *reinterpret_cast<uptr*>(Base() + addr::kPlayerCharacterPtr);
			const uptr tes = *reinterpret_cast<uptr*>(Base() + addr::kTESPtr);
			if (!player || !tes) return 0;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const uptr mine = PlayerShip();
			const auto entries = At<uptr*>(tes, off::kTESGridCells);
			const auto nCells = std::min(At<std::uint32_t>(tes, off::kTESGridCount), 64u);
			uptr best = 0;
			float bestD = 3000.0f;
			for (std::uint32_t i = 0; entries && i < nCells; ++i) {
				const uptr e = entries[i];
				if (!e) continue;
				if ((((At<std::uint32_t>(e, off::kGridEntryState) & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) continue;
				const uptr cell = At<uptr>(e, off::kGridEntryCell);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) continue;
				for (const int o : { 0x80, 0x88 }) {
					if (!Readable(cell + o, 16)) continue;
					const std::uint32_t size = At<std::uint32_t>(cell, o), cap = At<std::uint32_t>(cell, o + 4);
					uptr* arr = At<uptr*>(cell, o + 8);
					if (!arr || !size || size > cap || cap > 200000 || !Readable(reinterpret_cast<uptr>(arr), 8)) continue;
					if (!arr[0] || !Readable(arr[0], 0xC0) || At<uptr>(arr[0], off::kRefParentCell) != cell) continue;
					for (std::uint32_t k = 0; k < size && k < 8192; ++k) {
						const uptr r = arr[k];
						if (!r || r == mine || !Readable(r, 0x240) || (At<std::uint32_t>(r, 0x20) & 0x820) || !ShipLike(r)) continue;
						const auto rp = At<game::Vec3>(r, off::kRefPosition);
						const float dd = std::sqrt((rp.x - pp.x) * (rp.x - pp.x) + (rp.y - pp.y) * (rp.y - pp.y) + (rp.z - pp.z) * (rp.z - pp.z));
						if (dd < bestD) { bestD = dd; best = r; }
					}
					break;
				}
			}
			return best;
		}

		bool Pressed(int idx, int vk)
		{
			if (!vk) return false;
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool edge = down && !g_keyDown[idx];
			g_keyDown[idx] = down;
			return edge;
		}

		void TickBody()
		{
			const ULONGLONG now = GetTickCount64();
			if (Pressed(0, g_keySummon)) {
				// , : the Call Your Ship mod (installed) lands your home ship near you (its script: ~45 s, MoveTo + landing link + EnableWithLanding).
				const uptr console = *reinterpret_cast<uptr*>(Base() + addr::kConsoleManagerPtr);
				if (console) EngineFn<void (*)(uptr, const char*)>(addr::kConsoleExecute)(console, "cgf \"CallYourShip.Here\"");
				logger::Write("ShipProbe: t=%.2f , pressed: cgf \"CallYourShip.Here\" %s", Seconds(), console ? "sent" : "NOT sent (no console manager)");
			}
			if (Pressed(1, g_keyB) && !g_bStep) {
				// . : the nearest ship within 3 km; landed -> the game's take-off first, then the two fly-to points.
				const uptr s = NearestShip();
				if (!s) logger::Write("ShipProbe: . pressed: no ship within 3 km");
				else if (Eligible(s, "B")) {
					TrackShip(s, ". key");
					g_bShip = s;
					if (Landed(s) > 0.5f) {
						const auto p = At<game::Vec3>(s, off::kRefPosition);
						g_bStartZ = p.z;
						EngineFn<void (*)(uptr)>(kTakeOffCore)(s);
						logger::Write("ShipProbe: t=%.2f B: ship %08X is landed: take-off started (FUN_1421199B0); points follow once it is 30 m up", Seconds(), FormId(s));
						g_bStep = 10;
						g_bNext = now + 60000;
					}
					else g_bStep = 11;
				}
			}
			if (g_bStep == 10) {
				const float z = At<game::Vec3>(g_bShip, off::kRefPosition).z;
				if (Landed(g_bShip) < 0.5f && z > g_bStartZ + 30.0f) g_bStep = 11;
				else if (now >= g_bNext) { g_bStep = 0; logger::Write("ShipProbe: B: no take-off within 60 s (landed AV %.0f, %.0f m up); stopped", Landed(g_bShip), z - g_bStartZ); }
			}
			if (g_bStep == 11) {
				g_bStep = 0;
				const uptr s = g_bShip;
				const auto p = At<game::Vec3>(s, off::kRefPosition);
				const float yaw = At<game::Vec3>(s, off::kRefRotation).z;
				const float fx = std::sin(yaw), fy = std::cos(yaw);
				const float p1[3] = { p.x + fx * 400.0f, p.y + fy * 400.0f, p.z + 50.0f };
				g_bP2[0] = p1[0] + fy * 200.0f; g_bP2[1] = p1[1] - fx * 200.0f; g_bP2[2] = p1[2];  // 200 m to the right of point 1
				if (FlyTo(s, p1)) { g_bStep = 1; g_bNext = now + 4000; }
			}
			if (g_bStep == 1 && now >= g_bNext) {
				g_bStep = 2;
				if (Eligible(g_bShip, "B")) FlyTo(g_bShip, g_bP2);
				g_bNext = now + 30000;
			}
			if (g_bStep == 2 && now >= g_bNext) { g_bStep = 0; logger::Write("ShipProbe: B: done (30 s after point 2)"); }
			if (now - g_lastSample < 250) return;
			g_lastSample = now;
			Track copy[4];
			{ std::lock_guard lk(g_lock); std::copy(std::begin(g_tracks), std::end(g_tracks), copy); }
			for (auto& t : copy) {
				if (!t.ship) continue;
				if (now - t.since > 600000) { std::lock_guard lk(g_lock); for (auto& u : g_tracks) if (u.ship == t.ship) u = Track{}; continue; }
				Sample(t);
			}
		}
	}

	bool Install()
	{
		static const std::uint8_t kApproachBytes[14] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x81, 0x98, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0 };
		static const std::uint8_t kLandBytes[14] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x54 };
		static const std::uint8_t kTakeOffBytes[17] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0xC5, 0xF9, 0x57, 0xC0 };
		static const std::uint8_t kLandCoreBytes[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC };
		static const std::uint8_t kSubmitBytes[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
		int ok = 0;
		void* o = nullptr;
		if (hook::Install(Base() + kApproachOn, kApproachBytes, sizeof(kApproachBytes), reinterpret_cast<void*>(&ApproachOnDetour), &o)) { g_approachOn = reinterpret_cast<Ship1_t>(o); ++ok; }
		if (hook::Install(Base() + kApproachOff, kApproachBytes, sizeof(kApproachBytes), reinterpret_cast<void*>(&ApproachOffDetour), &o)) { g_approachOff = reinterpret_cast<Ship1_t>(o); ++ok; }
		if (hook::Install(Base() + kLand, kLandBytes, sizeof(kLandBytes), reinterpret_cast<void*>(&LandDetour), &o)) { g_land = reinterpret_cast<Ship2_t>(o); ++ok; }
		if (hook::Install(Base() + kTakeOff, kTakeOffBytes, sizeof(kTakeOffBytes), reinterpret_cast<void*>(&TakeOffDetour), &o)) { g_takeOff = reinterpret_cast<Ship1_t>(o); ++ok; }
		if (hook::Install(Base() + kLandCore, kLandCoreBytes, sizeof(kLandCoreBytes), reinterpret_cast<void*>(&LandCoreDetour), &o)) { g_landCore = reinterpret_cast<Ship2_t>(o); ++ok; }
		if (hook::Install(Base() + kSubmit, kSubmitBytes, sizeof(kSubmitBytes), reinterpret_cast<void*>(&SubmitDetour), &o)) { g_submit = reinterpret_cast<Submit_t>(o); ++ok; }
		// The user's test keys are , . / ' (never number keys). Taken by our own rule, not the free check.
		g_keySummon = VK_OEM_COMMA;
		g_keyB = VK_OEM_PERIOD;
		controlmap::Note("shipprobe", "summon home ship to land (test)", g_keySummon);
		controlmap::Note("shipprobe", "ship take-off + fly-to test (test)", g_keyB);
		logger::Write("ShipProbe: %d/6 hooks; test keys , summon . fly; combat re-issue settings %.2f s / %.1f m", ok,
			*reinterpret_cast<float*>(Base() + kComReissueTime), *reinterpret_cast<float*>(Base() + kComReissueDrift));
		return ok == 6;
	}

	void Tick()
	{
		__try {
			TickBody();
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			logger::Write("ShipProbe: raised an exception; skipped this frame");
		}
	}

	void Reset()
	{
		std::lock_guard lk(g_lock);
		for (auto& t : g_tracks) t = Track{};
		g_bStep = 0;
		g_bShip = 0;
	}
}

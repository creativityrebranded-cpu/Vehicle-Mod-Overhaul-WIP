#include "traffic.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "travellogic.h"
#include <vector>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Engine pieces (1.16.244, from the console command GetDistanceFromActorsPath, FUN_140C6FDA0):
//  - FUN_1419228E0(actor) -> movement controller (actor + 0x230, or the linked actor's).
//  - q = *(*(mc + 8) + 0x240): the path holder, refcount at q + 0xA8 (released by vtable[0](q, 1) at zero).
//  - FUN_140688B60(q + 0x48 value, &DAT_145F9D108) -> path follower interface; its vtable +0x10 (iface, &path) -> the
//    path, a smart pointer (int refcount at +0; release: FUN_14269DD00(path) + FUN_1422C7560(path, 0xD8)).
//  - path + 0x90 point count, + 0x98 point array, stride 0x58; point + 0x00 { float x, y, z; uint32 space }:
//    world = xyz + DAT_1461FAE88[space] (12-byte offsets, count DAT_1461FAE80) when space < count.
//  - FUN_14269FC90(path, from, to, flag) = length along the path (to < 0: the end).
namespace traffic
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kMovementController = 0x019228E0, kQueryInterface = 0x00688B60, kPathFollowerId = 0x05F9D108;
		constexpr std::uint32_t kPathRelease = 0x0269DD00, kFree = 0x022C7560, kPathLength = 0x0269FC90;
		constexpr std::uint32_t kSpaceOffsets = 0x061FAE88, kSpaceCount = 0x061FAE80;
		constexpr float kTripMetres = 150.0f;   // a goal this far away is a trip
		constexpr float kProbeRange = 150.0f;   // the probe logs actors this close to the player
		constexpr int kProbeMax = 12;

		ULONGLONG g_last = 0;

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

		void ReleasePath(uptr path)
		{
			if (!path) return;
			if (_InterlockedDecrement(reinterpret_cast<volatile long*>(path)) == 0) {
				EngineFn<void (*)(uptr)>(kPathRelease)(path);
				EngineFn<void (*)(uptr, std::uint64_t)>(kFree)(path, 0xD8);
			}
		}

		void ReleaseHolder(uptr q)
		{
			if (!q) return;
			if (_InterlockedDecrement(reinterpret_cast<volatile long*>(q + 0xA8)) == 0) {
				auto** vt = *reinterpret_cast<void***>(q);
				reinterpret_cast<void (*)(uptr, int)>(vt[0])(q, 1);
			}
		}

		std::vector<game::Vec3> g_doors;ULONGLONG g_doorsAt=0;
		void RefreshDoors(uptr world) {
			const auto now=GetTickCount64();if(now-g_doorsAt<2000)return;g_doorsAt=now;g_doors.clear();
			for(const auto r:modscene::LoadedRefs(8000)) {
				const auto base=At<uptr>(r,0x98),cell=At<uptr>(r,off::kRefParentCell);
				if(base && At<std::uint8_t>(base,0x2E)==0x25 && cell && At<uptr>(cell,off::kCellWorldspace)==world)
					g_doors.push_back(At<game::Vec3>(r,off::kRefPosition));
			}
		}
		uptr PointWorld(uptr point,bool& known,bool& interior) {
			// The location holds counted PathingCell/PathingSpace pointers at +0x10/+0x18.
			// FUN_140E7F6E0 constructs PathingSpace: +0x1C=0 is an interior cell, 1 an exterior world.
			for(const auto offset:{0x10u,0x18u}) {
				const auto space=At<uptr>(point,offset);if(!space||!Readable(space,0x20))continue;
				if(At<uptr>(space,0)==game::Base()+0x04C1ABC8) {
					const auto kind=At<std::int32_t>(space,0x1C);
					if(kind==0){known=true;interior=true;return 0;}
					if(kind==1){const auto form=At<uptr>(space,0x10);if(form&&At<std::uint8_t>(form,0x2E)==0x56){known=true;return form;}}
				}
			}
			return 0;
		}
		bool ReadPath(uptr actor, Destination& out)
		{
			const auto actorCell=At<uptr>(actor,off::kRefParentCell);
			const auto actorWorld=actorCell?At<uptr>(actorCell,off::kCellWorldspace):0;
			out.interior=!actorWorld;RefreshDoors(actorWorld);
			const uptr mc = EngineFn<uptr (*)(uptr)>(kMovementController)(actor);
			if (!mc) return false;
			const uptr mc8 = At<uptr>(mc, 8);
			if (!mc8) return false;
			const uptr q = At<uptr>(mc8, 0x240);
			if (!q) return false;
			_InterlockedIncrement(reinterpret_cast<volatile long*>(q + 0xA8));
			bool ok = false;
			uptr path = 0;
			__try {
			const uptr q9 = At<uptr>(q, 0x48);
			if (q9) {
				const uptr iface = EngineFn<uptr (*)(uptr, uptr)>(kQueryInterface)(q9, game::Base() + kPathFollowerId);
				if (iface) {
					auto** vt = *reinterpret_cast<void***>(iface);
					if (reinterpret_cast<char (*)(uptr, uptr*)>(vt[2])(iface, &path) && path) {
						const std::int32_t n = At<std::int32_t>(path, 0x90);
						const uptr pts = At<uptr>(path, 0x98);
						if (n > 0 && n < 100000 && pts && Readable(pts + std::uint64_t(n - 1) * 0x58, 0x20)) {
							const uptr last = pts + std::uint64_t(n - 1) * 0x58;
							float x = At<float>(last, 0), y = At<float>(last, 4), z = At<float>(last, 8);
							const std::uint32_t space = At<std::uint32_t>(last, 0xC);
							const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(game::Base() + kSpaceCount);
							const uptr table = Ptr(kSpaceOffsets);
							if (space < count && table) {
								x += At<float>(table, space * 12);
								y += At<float>(table, space * 12 + 4);
								z += At<float>(table, space * 12 + 8);
							}
							out.x = x; out.y = y; out.z = z;
							out.points = unsigned(n);
							out.contextKnown=n<=2048;
							for(int i=0;i<n&&i<2048;++i) {
								const auto point=pts+std::uint64_t(i)*0x58;bool known=false,inside=false;
								const auto world=PointWorld(point,known,inside);
								out.contextKnown=out.contextKnown&&known;out.interior=out.interior||inside;
								if(known&&!inside&&world!=actorWorld)out.changesSpace=true;
								float px=At<float>(point,0),py=At<float>(point,4),pz=At<float>(point,8);
								const auto index=At<std::uint32_t>(point,0xC);
								if(index<count&&table){px+=At<float>(table,index*12);py+=At<float>(table,index*12+4);pz+=At<float>(table,index*12+8);}
								for(const auto&door:g_doors)if(std::hypot(px-door.x,py-door.y)<5.0&&std::fabs(pz-door.z)<4.0){out.usesDoor=true;break;}
							}
							out.remaining = EngineFn<float (*)(uptr, float, float, char)>(kPathLength)(path, 0.0f, -1.0f, 0);
							ok = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
						}
					}
				}
			}
			} __finally {
			ReleasePath(path);
			ReleaseHolder(q);
			}
			return ok;
		}

		int PackageType(uptr actor)
		{
			const uptr proc = At<uptr>(actor, 0x228);
			if (!proc || !Readable(proc + 0x18, 0x18)) return -1;
			const uptr pkg = At<uptr>(proc, 0x18 + 0x08);
			if (!pkg || !Readable(pkg + 0x38, 8)) return -1;
			return At<std::int8_t>(pkg, 0x3C);
		}

		bool CellRefs(uptr cell, uptr*& data, std::uint32_t& count)
		{
			for (const int o : { 0x80, 0x88 }) {
				if (!Readable(cell + o, 16)) continue;
				const std::uint32_t size = At<std::uint32_t>(cell, o), cap = At<std::uint32_t>(cell, o + 4);
				uptr* arr = At<uptr*>(cell, o + 8);
				if (!arr || !size || size > cap || cap > 200000 || !Readable(reinterpret_cast<uptr>(arr), 8)) continue;
				const uptr first = arr[0];
				if (!first || !Readable(first, 0xC0) || At<uptr>(first, off::kRefParentCell) != cell) continue;
				data = arr;
				count = size;
				return true;
			}
			return false;
		}

		void ProbeBody()
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const uptr tes = Ptr(addr::kTESPtr);
			if (!player || !tes) return;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const auto entries = At<uptr*>(tes, off::kTESGridCells);
			const auto nCells = std::min(At<std::uint32_t>(tes, off::kTESGridCount), 64u);
			int logged = 0, seen = 0, travelling = 0;
			for (std::uint32_t i = 0; entries && i < nCells && logged < kProbeMax; ++i) {
				const uptr e = entries[i];
				if (!e) continue;
				const auto state = At<std::uint32_t>(e, off::kGridEntryState);
				if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) continue;
				const uptr cell = At<uptr>(e, off::kGridEntryCell);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) continue;
				uptr* refs = nullptr;
				std::uint32_t count = 0;
				if (!CellRefs(cell, refs, count)) continue;
				for (std::uint32_t k = 0; k < count && k < 8192 && logged < kProbeMax; ++k) {
					const uptr r = refs[k];
					if (!r || r == player || !Readable(r, 0x240) || At<char>(r, 0x2E) != 'K') continue;
					if (At<std::uint32_t>(r, 0x20) & 0x820) continue;  // disabled / deleted
					const auto ap = At<game::Vec3>(r, off::kRefPosition);
					if (std::hypot(ap.x - pp.x, ap.y - pp.y) > kProbeRange) continue;
					++seen;
					Destination d;
					d.packageType = PackageType(r);
					bool have = false;
					__try {
						have = ReadPath(r, d);
					}
					__except (EXCEPTION_EXECUTE_HANDLER) {
						have = false;
					}
					if (!have) continue;
					const float away = std::hypot(d.x - ap.x, d.y - ap.y);
					d.kind = away >= kTripMetres ? Kind::Trip : Kind::Short;
					++travelling;
					++logged;
					if (logger::Verbose()) logger::Write("Traffic: NPC %08X at (%.0f, %.0f) -> goal (%.0f, %.0f, %.0f) %.0f m away, %.0f m of path, %u points, package type %d%s",
						At<std::uint32_t>(r, 0x28), ap.x, ap.y, d.x, d.y, d.z, away, d.remaining, d.points, d.packageType, d.kind == Kind::Trip ? " TRIP" : "");
				}
			}
			static ULONGLONG lastSummary = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - lastSummary >= 30000) {
				lastSummary = now;
				if (logger::Verbose()) logger::Write("Traffic: probe: %d NPCs within %.0f m, %d with a path", seen, kProbeRange, travelling);
			}
		}
	}

	bool DestinationOf(std::uintptr_t actor, Destination& out)
	{
		if (!actor) return false;
		out = Destination{};
		out.packageType = PackageType(actor);
		bool have = false;
		__try {
			have = ReadPath(actor, out);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			have = false;
		}
		if (!have) return false;
		const auto ap = At<game::Vec3>(actor, off::kRefPosition);
		const double direct=std::hypot(out.x-ap.x,out.y-ap.y);
		out.kind = direct >= kTripMetres ? Kind::Trip : Kind::Short;
		out.vehicleSuitable=travellogic::UseVehicle({!out.interior,out.contextKnown,out.changesSpace,out.usesDoor,direct,out.remaining});
		return true;
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_last < 5000) return;
		g_last = now;
		__try {
			ProbeBody();
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			logger::Write("Traffic: probe raised an exception; skipped");
		}
	}

	void Reset() { g_last = 0;g_doors.clear();g_doorsAt=0; }
}

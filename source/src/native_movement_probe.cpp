#include "native_movement_probe.h"
#include "addresses.h"
#include "game.h"
#include "log.h"
#include "vehicleworld.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace native_movement_probe
{
	namespace
	{
		using uptr = std::uintptr_t;
		constexpr ULONGLONG kEveryMs = 5000;
		constexpr std::size_t kMaxOthers = 3;
		constexpr uptr kLookup = 0x0068A800;     // (out[2], formId) -> component, counted DB handle
		constexpr uptr kRelease = 0x024047A0;    // (handle - 0x20) when the last reference goes
		constexpr uptr kInterface = 0x00688B60;  // (registry, key) -> interface or null
		constexpr uptr kSetGoalKey = 0x05F9D100;
		constexpr uptr kStoredOutputKey = 0x05F9D860;
		constexpr uptr kComponentTable = 0x04B35198;  // MovementControllerComponent vtable (140689180 writes it)
		constexpr std::uint8_t kLookupPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56 };
		constexpr std::uint8_t kReleasePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20 };
		constexpr std::uint8_t kInterfacePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 };

		ULONGLONG g_at = 0;
		int g_verified = 0;  // 0 unchecked, 1 ok, -1 bytes differ

		bool Verify()
		{
			if (!g_verified) {
				const uptr b = game::Base();
				g_verified = !std::memcmp(reinterpret_cast<const void*>(b + kLookup), kLookupPrologue, sizeof kLookupPrologue) &&
					!std::memcmp(reinterpret_cast<const void*>(b + kRelease), kReleasePrologue, sizeof kReleasePrologue) &&
					!std::memcmp(reinterpret_cast<const void*>(b + kInterface), kInterfacePrologue, sizeof kInterfacePrologue) ? 1 : -1;
				logger::Write("Native movement probe: %s", g_verified > 0 ? "ready (controller lookup every 5 s, read only)" : "OFF (engine bytes differ)");
			}
			return g_verified > 0;
		}

		struct Found
		{
			bool faulted = false;
			uptr component = 0, handle = 0, table = 0, registry = 0, data = 0, owner = 0, movt = 0, setter = 0, output = 0;
			std::uint32_t dataOwnerId = 0, movtId = 0, goalState = 0xFFFFFFFF;
			std::uint8_t mode = 0, movtType = 0;
			float speed = NAN;
		};

		template <class T> T At(uptr p, std::size_t o) { T v; std::memcpy(&v, reinterpret_cast<const void*>(p + o), sizeof v); return v; }

		void Release(uptr handle)
		{
			if (!handle) return;
			auto* count = reinterpret_cast<volatile long long*>(handle - 0x20);
			if (_InterlockedExchangeAdd64(count, -0x100000001LL) == 0x100000001LL)
				reinterpret_cast<void (*)(uptr)>(game::Base() + kRelease)(handle - 0x20);
		}

		// Separate from Probe so the SEH frame holds no objects with destructors.
		void Read(std::uint32_t formId, Found& f)
		{
			const uptr b = game::Base();
			uptr out[2] = {};
			__try {
				reinterpret_cast<void (*)(uptr*, std::uint32_t)>(b + kLookup)(out, formId);
				f.component = out[0];
				f.handle = out[1];
				if (!f.component) return;
				f.table = At<uptr>(f.component, 0) - b;
				f.registry = At<uptr>(f.component, 0x48);
				f.mode = At<std::uint8_t>(f.component, 0x5C);
				f.data = At<uptr>(f.component, 0x78);
				if (f.data) {
					f.dataOwnerId = reinterpret_cast<std::uint32_t (*)(uptr)>(At<uptr>(At<uptr>(f.data, 0), 0x10))(f.data);
					f.owner = At<uptr>(f.data, 0x70);
					f.movt = At<uptr>(f.data, 0x78);
					if (f.movt && !(f.movt & 1)) { f.movtId = At<std::uint32_t>(f.movt, 0x28); f.movtType = At<std::uint8_t>(f.movt, 0x2E); }
					else if (f.movt & 1) f.movtId = std::uint32_t(f.movt >> 1);
				}
				if (f.registry) {
					auto lookup = reinterpret_cast<uptr (*)(uptr, uptr)>(b + kInterface);
					f.setter = lookup(f.registry, b + kSetGoalKey);
					f.output = lookup(f.registry, b + kStoredOutputKey);
					if (f.setter) f.goalState = At<std::uint32_t>(f.setter, 0x48);    // arbiter+0x60: 1 pending, 2 path, 3 complete
					if (f.output) f.speed = At<float>(f.output, 0x20);                // agent+0x40
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				f.faulted = true;
			}
		}

		void Probe(const world::VehicleInfo& v, const char* who, float metres)
		{
			Found f;
			Read(v.formId, f);
			const uptr handle = f.handle;
			__try { Release(handle); } __except (EXCEPTION_EXECUTE_HANDLER) { f.faulted = true; }
			if (f.faulted) { logger::Write("Native movement probe: %s %08X base %08X (%.0f m): lookup FAULTED", who, v.formId, v.baseId, metres); return; }
			if (!f.component) {
				logger::Write("Native movement probe: %s %08X base %08X (%.0f m): NO engine controller (handle %s)",
					who, v.formId, v.baseId, metres, handle ? "set" : "none");
				return;
			}
			logger::Write("Native movement probe: %s %08X base %08X (%.0f m): controller %p vtable %s%llX, registry %s, mode %u, data %p owner %08X%s ref %s, MOVT %08X type %02X, set-goal %s state %u, stored output %s speed %.2f",
				who, v.formId, v.baseId, metres, reinterpret_cast<void*>(f.component),
				f.table == kComponentTable ? "MovementControllerComponent +" : "UNEXPECTED +", static_cast<unsigned long long>(f.table),
				f.registry ? "yes" : "NO", f.mode, reinterpret_cast<void*>(f.data), f.dataOwnerId,
				f.dataOwnerId == v.formId ? "" : " (NOT this vehicle)", f.owner == v.ref ? "matches" : "differs",
				f.movtId, f.movtType, f.setter ? "yes" : "no", f.goalState, f.output ? "yes" : "no", f.speed);
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_at < kEveryMs) return;
		g_at = now;
		if (!Verify()) return;
		const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
		world::Snapshot snap;
		if (!player || !world::Vehicles(snap)) return;
		const auto pp = *reinterpret_cast<const game::Vec3*>(player + off::kRefPosition);
		auto dist = [&](const world::VehicleInfo& v) { return float(std::hypot(v.pos[0] - pp.x, v.pos[1] - pp.y)); };
		std::vector<const world::VehicleInfo*> others;
		for (std::uint32_t i = 0; i < snap.count && i < 64; ++i) {
			const auto& v = snap.v[i];
			if (!v.formId) continue;
			if (v.who == world::VehicleInfo::Who::Player) Probe(v, "player vehicle", dist(v));
			else others.push_back(&v);
		}
		std::sort(others.begin(), others.end(), [&](const auto* a, const auto* b) { return dist(*a) < dist(*b); });
		for (std::size_t i = 0; i < others.size() && i < kMaxOthers; ++i)
			Probe(*others[i], others[i]->who == world::VehicleInfo::Who::Npc ? "NPC vehicle" : "vehicle", dist(*others[i]));
	}
}

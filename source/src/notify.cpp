#include "notify.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "anchor.h"
#include "telemetry.h"

#include <Windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace notify
{
	namespace
	{
		using game::Base;

		// Event sent by Debug.Notification (FUN_142003a60): { BSFixedString text; BSFixedString sound; uint32 flag; }
		struct HudNotification
		{
			void*         text = nullptr;
			void*         sound = nullptr;
			std::uint32_t flag = 1;
		};

		struct SinkCallback
		{
			void* const*           vtbl;
			const HudNotification* evt;
			std::uintptr_t         source;

			static void Noop(SinkCallback*) {}
			static int Invoke(SinkCallback* self, std::uintptr_t sink)
			{
				auto** sinkVtbl = *reinterpret_cast<void***>(sink);
				using ProcessEvent = int (*)(std::uintptr_t, const HudNotification*, std::uintptr_t);
				return reinterpret_cast<ProcessEvent>(sinkVtbl[1])(sink, self->evt, self->source);
			}
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop),
				reinterpret_cast<void*>(&Invoke), reinterpret_cast<void*>(&Noop) };
		};
	}

	void Log(const char* fmt, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (logger::Verbose()) logger::Write("Notify (log only): %s", buf);
	}

	void Show(const char* fmt, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		logger::Write("Notify: %s", buf);

		const auto create = reinterpret_cast<void (*)(void**, const char*, bool)>(Base() + addr::kFixedStringCreate);
		const auto release = reinterpret_cast<void (*)(void**)>(Base() + addr::kFixedStringRelease);
		const auto sourceFn = reinterpret_cast<std::uintptr_t (*)()>(Base() + addr::kHudNotificationSource);
		const auto notifyFn = reinterpret_cast<void (*)(std::uintptr_t, SinkCallback*)>(Base() + addr::kEventNotify);

		HudNotification evt;
		create(&evt.text, buf, true);
		const auto source = sourceFn();
		SinkCallback cb{ SinkCallback::kVtbl, &evt, source };
		notifyFn(source, &cb);
		release(&evt.sound);
		release(&evt.text);
	}
}

namespace readout
{
	namespace
	{
		std::uintptr_t g_lastCell = 0;
		game::Vec3     g_lastPos{};
		ULONGLONG      g_lastTime = 0;
		float          g_speed = 0.0f;   // m/s, smoothed
		float          g_maxClean = 0.0f;  // highest speed seen without a "cell not ready"

		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
	}

	// Planet coordinate (0x2C bytes): face at +0x00, pattern x/y at +0x04/+0x08, block x/y at +0x14/+0x18,
	// cell-in-block x/y at +0x24/+0x28. See the engine reference, "Planet-wide grid".
	struct PlanetCoord
	{
		std::uint8_t bytes[0x2C];
		std::uint8_t Face() const { return bytes[0]; }
		std::int32_t I(int o) const { return *reinterpret_cast<const std::int32_t*>(bytes + o); }
	};

	void Tick()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto tes = Ptr(addr::kTESPtr);
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		if (!player || !tes || !mgr) {
			return;
		}
		// Follow the anchor: the player outdoors, or their ship while they are inside it.
		const auto anchor = anchor::Current();
		if (!anchor.ref) {
			g_lastTime = 0;
			return;  // indoors
		}
		const auto pos = *reinterpret_cast<const game::Vec3*>(anchor.ref + off::kRefPosition);
		const ULONGLONG now = GetTickCount64();
		if (g_lastTime != 0 && now > g_lastTime) {
			const float dt = (now - g_lastTime) / 1000.0f;
			const float d = std::hypot(pos.x - g_lastPos.x, pos.y - g_lastPos.y);
			if (dt > 0.0f && d < 2000.0f) {  // ignore jumps from re-centres and teleports
				g_speed = 0.9f * g_speed + 0.1f * (d / dt);
			}
		}
		g_lastPos = pos;
		g_lastTime = now;

		const auto cell = anchor.cell;

		// Speed-test signal: entering a cell that is not loaded yet (rate-limited).
		static ULONGLONG lastWarn = 0;
		if (cell != g_lastCell) {
			g_lastCell = cell;
			if (*reinterpret_cast<std::uint8_t*>(cell + off::kCellState) != 10) {
				if (now - lastWarn > 3000) {
					lastWarn = now;
					notify::Log("CELL NOT READY at %.0f m/s (max clean %.0f)", g_speed, g_maxClean);
				}
				{  // every occurrence goes to telemetry; the on-screen warning is rate-limited
					telemetry::Record("cell_not_ready", "\"speed\":%.1f,\"max_clean\":%.1f", g_speed, g_maxClean);
				}
			}
			else if (g_speed > g_maxClean) {
				g_maxClean = g_speed;
			}
		}

		// Planet cell: the planet-wide block the player is in. World cell (0, 0) maps to the manager's origin
		// coordinate, so the player's coordinate is that plus their offset in cells.
		const auto planetId = *reinterpret_cast<std::uint32_t*>(mgr + off::kMgrPlanetId);
		const auto grid = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint32_t, char)>(game::Base() + addr::kGetPlanetGrid)(0, planetId, 0);
		if (!grid) {
			return;
		}
		PlanetCoord c;
		std::memcpy(c.bytes, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), sizeof(c.bytes));
		const std::int32_t offset[2] = { static_cast<std::int32_t>(std::floor(pos.x / off::kCellSize)),
			static_cast<std::int32_t>(std::floor(pos.y / off::kCellSize)) };
		reinterpret_cast<void (*)(std::uintptr_t, void*, const std::int32_t*)>(game::Base() + addr::kOffsetPlanetCoord)(grid, c.bytes, offset);

		static std::int32_t lastKey[5] = { -1, 0, 0, 0, 0 };
		const std::int32_t key[5] = { c.Face(), c.I(0x04), c.I(0x08), c.I(0x14), c.I(0x18) };
		if (std::memcmp(key, lastKey, sizeof(key)) != 0) {
			const bool first = lastKey[0] == -1;
			std::memcpy(lastKey, key, sizeof(key));
			notify::Log("%s planet cell: face %d, pattern (%d, %d), block (%d, %d) | %.0f m/s | max clean %.0f",
				first ? "In" : "Entered", key[0], key[1], key[2], key[3], key[4], g_speed, g_maxClean);
			telemetry::Record("planet_cell", "\"face\":%d,\"pattern\":[%d,%d],\"block\":[%d,%d],\"speed\":%.1f,\"max_clean\":%.1f",
				key[0], key[1], key[2], key[3], key[4], g_speed, g_maxClean);
		}
	}

	bool SetPlayerMarker(const float pos[3])
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		if (!player || !mgr) return false;
		__try {
			const auto holder = *reinterpret_cast<std::uintptr_t*>(mgr + off::kMgrMarkerWorld);
			const auto form = holder ? *reinterpret_cast<std::uintptr_t*>(holder) : 0;
			if (!form) return false;
			const auto state = *reinterpret_cast<std::uint32_t*>(form + 0x14);
			if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) return false;
			const auto worldspace = *reinterpret_cast<std::uintptr_t*>(form + 0x98);
			if (!worldspace) return false;
			float p[3] = { pos[0], pos[1], pos[2] };
			const auto setMarker = reinterpret_cast<void (*)(const float*, std::uintptr_t, std::uint32_t*, std::uintptr_t)>(game::Base() + addr::kSetCustomMarker);
			setMarker(p, worldspace, reinterpret_cast<std::uint32_t*>(player + off::kPlayerCustomMarker), player + off::kPlayerCustomMarkerData);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void TrackPlayerMarker()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 3000) {
			return;
		}
		last = now;

		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const auto anchor = anchor::Current();  // the player outdoors, or their ship while inside it
		if (!player || !mgr || !anchor.ref) {
			return;  // outdoors (or in the ship) on a planet only
		}
		if (*reinterpret_cast<std::uint8_t*>(anchor.cell + off::kCellState) != 10) {
			return;  // not while the anchor's cell is still loading
		}
		// Same world lookup and validity test as the game's own marker placement (FUN_1416d8420).
		const auto holder = *reinterpret_cast<std::uintptr_t*>(mgr + off::kMgrMarkerWorld);
		const auto form = holder ? *reinterpret_cast<std::uintptr_t*>(holder) : 0;
		if (!form) {
			return;
		}
		const auto state = *reinterpret_cast<std::uint32_t*>(form + 0x14);
		if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) {
			return;
		}
		const auto worldspace = *reinterpret_cast<std::uintptr_t*>(form + 0x98);
		if (!worldspace) {
			return;
		}
		float pos[3];
		std::memcpy(pos, reinterpret_cast<const void*>(anchor.ref + off::kRefPosition), sizeof(pos));
		const auto setMarker = reinterpret_cast<void (*)(const float*, std::uintptr_t, std::uint32_t*, std::uintptr_t)>(
			game::Base() + addr::kSetCustomMarker);
		setMarker(pos, worldspace, reinterpret_cast<std::uint32_t*>(player + off::kPlayerCustomMarker),
			player + off::kPlayerCustomMarkerData);
	}

	void OnRecentre(int cellsX, int cellsY, unsigned objects)
	{
		notify::Log("Re-centred %.1f km (%d, %d cells), %u objects", std::hypot(cellsX, cellsY) * 0.1, cellsX, cellsY, objects);
	}
}

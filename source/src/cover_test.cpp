#include "cover_test.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"

#include <Windows.h>

#include <cmath>
#include <cstring>

namespace covertest
{
	namespace
	{
		using uptr = std::uintptr_t;

		// 1.16.244 (notes/INTERACTIVE_COVER_PLAN.md).
		constexpr std::uint32_t kActorFlags = 0xF8;             // SetForceSneak (FUN_140c3a210) writes bit 0x08000000 here
		constexpr std::uint32_t kForceSneakBit = 0x08000000;
		constexpr std::uint32_t kSprintFlags = 0x112C;          // SprintHandler (FUN_1412be1c0) toggles bit 0x04 here
		constexpr std::uint8_t kSprintBit = 0x04;
		constexpr std::uint32_t kIsSneaking = 0x018E7660;       // (actor) -> sneaking
		constexpr std::uint8_t kIsSneakingBytes[8] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B };
		constexpr std::uint32_t kSlideStartVtbl = 0x04CC5710;   // SlideStartHandler::vftable, [1] = FUN_141aecf50 (player only)
		constexpr std::uint32_t kSlideStartFn = 0x01AECF50;

		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		// ---- SlideStart watch: count the game's own slide-start events (an animation annotation, player only).
		using Handler_t = std::uint64_t (*)(uptr, uptr, uptr);
		Handler_t g_slideOriginal = nullptr;
		volatile long g_slideCount = 0;

		std::uint64_t SlideStartDetour(uptr self, uptr actor, uptr name)
		{
			InterlockedIncrement(&g_slideCount);
			return g_slideOriginal(self, actor, name);
		}

		enum class Test { None, Crouch, Walk, Slide, Cover };
		constexpr int kTests = 4;
		constexpr const char* kNames[kTests + 1] = { "", "crouch", "walk forward", "sprint + slide", "enter-cover action" };

		struct State
		{
			Test test = Test::None;
			int next = 1;            // the test the next press runs
			ULONGLONG start = 0, lastLog = 0;
			bool keyDown = false;
			bool sneakWas = false;   // force-sneak bit before the test
			bool sprintWas = false;  // sprint request bit before the test
			bool sneakSet = false, sprintSet = false, moving = false;
			float lx = 0, ly = 0, lz = 0, maxSpeed = 0;
			long slideAtStart = 0;
			ULONGLONG lastPosAt = 0;
		} g;

		uptr Player() { return Ptr(addr::kPlayerCharacterPtr); }

		float* MoveData()
		{
			const uptr controls = Ptr(addr::kPlayerControlsPtr);
			return controls ? reinterpret_cast<float*>(controls + off::kPlayerControlsData) : nullptr;
		}

		bool IsSneaking(uptr player)
		{
			__try {
				if (std::memcmp(reinterpret_cast<void*>(game::Base() + kIsSneaking), kIsSneakingBytes, sizeof(kIsSneakingBytes)) != 0) return false;
				return (EngineFn<std::uint8_t (*)(uptr)>(kIsSneaking)(player) & 1) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void SetSneak(uptr player, bool on)
		{
			auto& f = At<std::uint32_t>(player, kActorFlags);
			f = on ? (f | kForceSneakBit) : (f & ~kForceSneakBit);
		}

		void SetSprint(uptr player, bool on)
		{
			auto& b = At<std::uint8_t>(player, kSprintFlags);
			b = on ? static_cast<std::uint8_t>(b | kSprintBit) : static_cast<std::uint8_t>(b & ~kSprintBit);
		}

		unsigned MoveState(uptr player) { return (At<std::uint32_t>(player, kActorFlags) >> 17) & 0xF; }

		void Console(const char* line)
		{
			const uptr console = Ptr(addr::kConsoleManagerPtr);
			if (!console) { logger::Write("Cover test: no console manager; \"%s\" not run", line); return; }
			EngineFn<void (*)(uptr, const char*)>(addr::kConsoleExecute)(console, line);
			logger::Write("Cover test: console \"%s\"", line);
		}

		// Ends the running test and puts back everything it changed.
		void Finish(const char* why)
		{
			const uptr player = Player();
			if (player) {
				__try {
					if (g.sneakSet) SetSneak(player, g.sneakWas);
					if (g.sprintSet) SetSprint(player, g.sprintWas);
					if (g.moving) {
						if (float* d = MoveData()) { d[0] = 0.0f; d[1] = 0.0f; }
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			if (g.test != Test::None) {
				const long slides = g_slideCount - g.slideAtStart;
				logger::Write("Cover test %s: end (%s); top speed %.2f m/s, SlideStart events %ld, sneaking now %d", kNames[static_cast<int>(g.test)],
					why, g.maxSpeed, slides, player ? IsSneaking(player) : -1);
				notify::Show("Cover test %s done: top %.1f m/s%s", kNames[static_cast<int>(g.test)], g.maxSpeed, slides ? ", SLIDE fired" : "");
			}
			const int ran = static_cast<int>(g.test);
			const int next = g.next;
			g = State{};
			g.next = ran ? next : 1;
			g.keyDown = true;  // the key may still be held
		}

		void Begin(uptr player)
		{
			const int n = g.next;
			g.test = static_cast<Test>(n);
			g.next = n % kTests + 1;
			g.start = g.lastLog = g.lastPosAt = GetTickCount64();
			g.sneakWas = (At<std::uint32_t>(player, kActorFlags) & kForceSneakBit) != 0;
			g.sprintWas = (At<std::uint8_t>(player, kSprintFlags) & kSprintBit) != 0;
			const auto& p = At<game::Vec3>(player, off::kRefPosition);
			g.lx = p.x; g.ly = p.y; g.lz = p.z;
			g.slideAtStart = g_slideCount;
			logger::Write("Cover test %d/%d %s: start at (%.1f, %.1f, %.1f); flags 0x%08X state %u sprint bit %d sneaking %d", n, kTests, kNames[n],
				p.x, p.y, p.z, At<std::uint32_t>(player, kActorFlags), MoveState(player), g.sprintWas, IsSneaking(player));
			notify::Show("Cover test %d/%d: %s (next press: %s)", n, kTests, kNames[n], kNames[g.next]);
			if (g.test == Test::Cover) {
				Console("help ActionEnterCover 4");
				Console("player.pa ActionEnterCover");
			}
		}

		void Step(uptr player, ULONGLONG now)
		{
			const ULONGLONG t = now - g.start;
			// Speed from the position, every 100 ms (horizontal).
			if (now - g.lastPosAt >= 100) {
				const auto& p = At<game::Vec3>(player, off::kRefPosition);
				const float dt = static_cast<float>(now - g.lastPosAt) / 1000.0f;
				const float v = std::hypot(p.x - g.lx, p.y - g.ly) / dt;
				if (v > g.maxSpeed && v < 60.0f) g.maxSpeed = v;
				g.lx = p.x; g.ly = p.y; g.lz = p.z;
				g.lastPosAt = now;
				if (g.test != Test::Cover) {
					const float* d = MoveData();
					logger::Write("Cover test %s t=%4llu ms: speed %.2f m/s, move in (%.2f, %.2f), flags 0x%08X state %u sprint bit %d sneaking %d slides %ld",
						kNames[static_cast<int>(g.test)], t, v, d ? d[0] : 0.0f, d ? d[1] : 0.0f, At<std::uint32_t>(player, kActorFlags),
						MoveState(player), (At<std::uint8_t>(player, kSprintFlags) & kSprintBit) != 0, IsSneaking(player), g_slideCount - g.slideAtStart);
				}
			}
			switch (g.test) {
			case Test::Crouch:
				if (!g.sneakSet) { SetSneak(player, true); g.sneakSet = true; }
				if (t >= 3000) Finish("3 s");
				break;
			case Test::Walk:
				if (float* d = MoveData()) { d[0] = 0.0f; d[1] = 1.0f; g.moving = true; }
				if (t >= 1000) Finish("1 s");
				break;
			case Test::Slide:
				if (float* d = MoveData()) { d[0] = 0.0f; d[1] = 1.0f; g.moving = true; }
				if (!g.sprintSet) { SetSprint(player, true); g.sprintSet = true; }
				if (t >= 1200 && !g.sneakSet) {
					SetSneak(player, true);
					g.sneakSet = true;
					logger::Write("Cover test sprint + slide: crouch now (t=%llu ms)", t);
				}
				if (t >= 2500) Finish("2.5 s");
				break;
			case Test::Cover:
				if (t >= 1500) Finish("console lines sent; open the console (~) to read them");
				break;
			default: break;
			}
		}
	}

	bool Install()
	{
		auto** vtbl = reinterpret_cast<void**>(game::Base() + kSlideStartVtbl);
		if (vtbl[1] != reinterpret_cast<void*>(game::Base() + kSlideStartFn)) {
			logger::Write("Cover test: SlideStart handler vtable slot is not the expected function; slide watch off");
			return false;
		}
		DWORD old = 0;
		VirtualProtect(&vtbl[1], sizeof(void*), PAGE_READWRITE, &old);
		g_slideOriginal = reinterpret_cast<Handler_t>(vtbl[1]);
		vtbl[1] = reinterpret_cast<void*>(&SlideStartDetour);
		VirtualProtect(&vtbl[1], sizeof(void*), old, &old);
		logger::Write("Cover test: SlideStart handler watched");
		return true;
	}

	void Tick()
	{
		__try {
			const uptr player = Player();
			if (!player) return;
			const ULONGLONG now = GetTickCount64();
			const bool down = Focused() && (GetAsyncKeyState(VK_OEM_7) & 0x8000) != 0;
			const bool pressed = down && !g.keyDown;
			g.keyDown = down;
			if (pressed) {
				if (g.test != Test::None) Finish("key pressed again");
				else Begin(player);
			}
			if (g.test != Test::None) Step(player, now);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			logger::Write("Cover test: raised an exception; test ended");
			g = State{};
		}
	}

	void Reset()
	{
		if (g.test != Test::None) Finish("game loaded");
	}
}

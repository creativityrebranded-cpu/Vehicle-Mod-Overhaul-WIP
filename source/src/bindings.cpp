#include "bindings.h"

#include "bindlogic.h"
#include "controlmap.h"
#include "log.h"
#include "notify.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Xinput.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace bindings
{
	namespace
	{
		struct Def
		{
			const wchar_t* key;        // the line in bindings.ini
			const char* spec;          // what a new bindings.ini says
			const char* what;          // its comment
			const char* gameContext;   // the game binding "default" follows (its controller button too), or null
			const char* gameEvent;
		};
		// In Action order.
		constexpr Def kDefs[] = {
			{ L"scanner", "default", "T: tap go / pick, double tap clear the route, hold 1 s ram; on foot in combat tap = focus fire", nullptr, nullptr },
			{ L"summon", "default", "N: summon your vehicle (tap), hold for its menu", nullptr, nullptr },
			{ L"cover", "default", "U: follow (out of combat) / cover (in combat)", nullptr, nullptr },
			{ L"stance", "default", "Defend / Attack / Hold fire for your vehicle and escorts", nullptr, nullptr },
			{ L"hover_toggle", "default", "hover on / off (double tapping vertical boost also works)", nullptr, nullptr },
			{ L"lift_up", "default", "hover: climb", nullptr, nullptr },
			{ L"lift_down", "default", "hover: descend", nullptr, nullptr },
			{ L"gear", "default", "Alt: tap = next gear, hold = descend", nullptr, nullptr },
			{ L"boost", "default", "LShift: boost", "Vehicle", "VehicleBoost" },
			{ L"accelerate", "default, Pad:RT", "the game's accelerate (W); also drives hover", "Vehicle", "VehicleAccel" },
			{ L"brake", "default, Pad:LT", "the game's brake (S); also hover", "Vehicle", "VehicleBrake" },
			{ L"turn_left", "default, Pad:LStickLeft", "the game's turn left (A); hover: roll left", "Vehicle", "VehicleTurnLeft" },
			{ L"turn_right", "default, Pad:LStickRight", "the game's turn right (D); hover: roll right", "Vehicle", "VehicleTurnRight" },
			{ L"vertical_boost", "default", "the game's vertical boost (Space); double tap = hover", "Vehicle", "VehicleVertBoost" },
			{ L"ride_along", "default", "next to a vehicle an NPC flies: ride along / get out", nullptr, nullptr },
			{ L"retry_trip", "default", "route: retry the trip", nullptr, nullptr },
			{ L"skip_stop", "default", "route: skip the stop", nullptr, nullptr },
			{ L"own_profile", "default", "your vehicle's profile", nullptr, nullptr },
			{ L"show_hints", "default", "J: show every hint and panel", nullptr, nullptr },
		};
		static_assert(std::size(kDefs) == std::size_t(Action::Count), "one bindings.ini line per action");

		std::mutex g_lock;
		std::array<bindlogic::Binding, std::size_t(Action::Count)> g_bind{};
		std::array<std::uint32_t, std::size_t(Action::Count)> g_gamePad{};  // the game's controller button for "default"
		std::uint32_t g_modifiers = 0;
		bool g_loaded = false;
		FILETIME g_stamp{};
		ULONGLONG g_fileAt = 0, g_gameAt = 0;

		std::atomic<std::uint32_t> g_pad{ 0 };
		std::atomic<bool> g_connected{ false };
		std::atomic<ULONGLONG> g_padAt{ 0 };
		std::mutex g_padLock;
		using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		GetState_t g_getState = nullptr;
		int g_user = -1;              // the controller in use
		ULONGLONG g_scanAt = 0;       // when the empty slots were last asked (slow on Windows: every 2 s at most)

		std::wstring IniPath()
		{
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return {};
			std::wstring dir = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle";
			CoTaskMemFree(docs);
			SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
			return dir + L"\\bindings.ini";
		}

		std::string Narrow(const wchar_t* w)
		{
			char buf[1024] = {};
			WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
			return buf;
		}

		void WriteTemplate(const std::wstring& path)
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return;
			std::fprintf(f,
				"; Flying Vehicle Overhaul - controls. Saved changes apply while the game runs.\n"
				";\n"
				"; Each line lists alternatives, separated by commas; any one of them counts:\n"
				";   T, F7, LShift, Space, Mouse4 ...  a key or mouse button\n"
				";   Pad:A                             a controller button: A B X Y LB RB LT RT LS RS Start View\n"
				";                                     DUp DDown DLeft DRight, LStickUp/Down/Left/Right, RStickUp/Down/Left/Right\n"
				";   Pad:LB+DUp                        a chord: hold LB, press DUp. While a chord button is held, plain\n"
				";                                     controller bindings of other buttons stay quiet.\n"
				";   default                           the mod's own key for it (the game's binding where it follows one,\n"
				";                                     with the game's controller button)\n"
				";   none                              nothing\n"
				"; Example: scanner = default, Pad:LB+A\n"
				";\n"
				"; Controller buttons are also read by the game itself, so a plain button may do two things; chords with a button\n"
				"; the game uses little avoid that. A controller layout to start from (remove the ; to use it):\n"
				";   scanner = default, Pad:LB+A\n"
				";   summon = default, Pad:LB+DUp\n"
				";   cover = default, Pad:LB+DDown\n"
				";   stance = default, Pad:LB+DRight\n"
				";   hover_toggle = default, Pad:LB+Y\n"
				";   lift_up = default, Pad:RB\n"
				";   lift_down = default, Pad:LB+RB\n"
				";   gear = default, Pad:LB+X\n"
				";   show_hints = default, Pad:LB+View\n"
				"\n[bindings]\n");
			for (const auto& d : kDefs) std::fprintf(f, "; %s\n%s = %s\n", d.what, Narrow(d.key).c_str(), d.spec);
			std::fclose(f);
			logger::Write("Bindings: wrote %s with the default controls", Narrow(path.c_str()).c_str());
		}

		// bindings.ini -> g_bind (under g_lock). A missing line is added with its default.
		void Load(bool reloaded)
		{
			const std::wstring path = IniPath();
			if (path.empty()) return;
			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) WriteTemplate(path);
			WIN32_FILE_ATTRIBUTE_DATA fa{};
			if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) g_stamp = fa.ftLastWriteTime;
			int problems = 0;
			for (std::size_t i = 0; i < std::size(kDefs); ++i) {
				wchar_t w[512] = {};
				const DWORD n = GetPrivateProfileStringW(L"bindings", kDefs[i].key, L"\x01", w, 512, path.c_str());
				std::string spec;
				if (n == 1 && w[0] == L'\x01') {
					spec = kDefs[i].spec;
					const std::wstring def(spec.begin(), spec.end());
					WritePrivateProfileStringW(L"bindings", kDefs[i].key, def.c_str(), path.c_str());
				}
				else spec = Narrow(w);
				if (const auto semi = spec.find(';'); semi != std::string::npos && semi > 0 && spec[semi - 1] == ' ') spec.resize(semi);  // a trailing "; comment"
				std::string bad;
				g_bind[i] = bindlogic::Parse(spec, &bad);
				if (!bad.empty()) {
					++problems;
					logger::Write("Bindings: %s: not understood: %s (the rest of the line is used)", Narrow(kDefs[i].key).c_str(), bad.c_str());
				}
				logger::Write("Bindings: %-14s = %s", Narrow(kDefs[i].key).c_str(), spec.c_str());
			}
			g_modifiers = bindlogic::Modifiers(std::vector<bindlogic::Binding>(g_bind.begin(), g_bind.end()));
			g_loaded = true;
			if (reloaded) notify::Show(problems ? "Controls reloaded (%d line(s) partly not understood, see the log)" : "Controls reloaded from bindings.ini", problems);
		}

		void GamePads(ULONGLONG now)
		{
			if (g_gameAt && now - g_gameAt < 5000) return;
			g_gameAt = now;
			static bool told = false;
			for (std::size_t i = 0; i < std::size(kDefs); ++i) {
				if (!kDefs[i].gameContext) continue;
				const std::uint32_t code = controlmap::PadFor(kDefs[i].gameContext, kDefs[i].gameEvent);
				g_gamePad[i] = bindlogic::GamePadBit(code);
				if (!told) logger::Write("Bindings: the game's controller binding for %s:%s is code 0x%X -> %s", kDefs[i].gameContext, kDefs[i].gameEvent, code,
					g_gamePad[i] ? bindlogic::PadLabel(g_gamePad[i]).c_str() : "not used");
			}
			told = true;
		}

		void PollPad(ULONGLONG now)
		{
			if (now - g_padAt.load(std::memory_order_relaxed) < 8) return;
			std::unique_lock lk(g_padLock, std::try_to_lock);
			if (!lk.owns_lock()) return;
			g_padAt.store(now, std::memory_order_relaxed);
			if (!g_getState) {
				static bool tried = false;
				if (tried) return;
				tried = true;
				for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" }) {
					if (const HMODULE m = LoadLibraryW(dll)) {
						g_getState = reinterpret_cast<GetState_t>(GetProcAddress(m, "XInputGetState"));
						if (g_getState) { logger::Write("Bindings: controllers read through %s", Narrow(dll).c_str()); break; }
					}
				}
				if (!g_getState) { logger::Write("Bindings: no XInput on this system; controller bindings are off"); return; }
			}
			XINPUT_STATE st{};
			if (g_user >= 0 && g_getState(DWORD(g_user), &st) == ERROR_SUCCESS) {
				const auto& p = st.Gamepad;
				g_pad.store(bindlogic::PadBits(p.wButtons, p.bLeftTrigger, p.bRightTrigger, p.sThumbLX, p.sThumbLY, p.sThumbRX, p.sThumbRY), std::memory_order_relaxed);
				return;
			}
			if (g_user >= 0) { logger::Write("Bindings: controller %d disconnected", g_user); g_user = -1; g_connected = false; }
			g_pad.store(0, std::memory_order_relaxed);
			if (g_scanAt && now - g_scanAt < 2000) return;
			g_scanAt = now;
			for (DWORD u = 0; u < 4; ++u) {
				if (g_getState(u, &st) == ERROR_SUCCESS) {
					g_user = int(u);
					g_connected = true;
					logger::Write("Bindings: controller %u connected", u);
					return;
				}
			}
		}

		bool KeyDown(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		PollPad(now);
		std::lock_guard lk(g_lock);
		if (!g_loaded) { Load(false); return; }
		if (now - g_fileAt >= 1000) {
			g_fileAt = now;
			WIN32_FILE_ATTRIBUTE_DATA fa{};
			if (GetFileAttributesExW(IniPath().c_str(), GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &g_stamp) != 0) Load(true);
		}
		GamePads(now);
	}

	bool Held(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return false;
		PollPad(GetTickCount64());
		const std::uint32_t pad = g_pad.load(std::memory_order_relaxed);
		std::lock_guard lk(g_lock);
		if (!g_loaded) return KeyDown(defaultVk);  // before the first Tick: the module's own key
		return bindlogic::Held(g_bind[i], defaultVk, pad, g_modifiers, KeyDown, g_gamePad[i]);
	}

	std::string Label(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return "unbound";
		std::lock_guard lk(g_lock);
		if (!g_loaded) return defaultVk ? bindlogic::KeyLabel(defaultVk) : "unbound";
		return bindlogic::Label(g_bind[i], defaultVk, g_connected ? g_gamePad[i] : 0);
	}

	bool PadConnected() { return g_connected.load(); }
}

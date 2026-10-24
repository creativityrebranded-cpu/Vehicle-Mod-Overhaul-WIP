#include "bindings.h"

#include "addresses.h"
#include "bindlogic.h"
#include "controlmap.h"
#include "game.h"
#include "hoverui.h"
#include "inputgate.h"
#include "log.h"
#include "notify.h"
#include "passenger.h"
#include "vehicleflight.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Xinput.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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
		// Layout 3 (2026-10-24, the user approved the situation-based map with no chords): a controller button belongs to
		// the mod only in the situations named after '@' (bindlogic.h), and there the game's own action on it is switched
		// off (inputgate). On foot the mod's controller actions live in the scanner (LB), where the game leaves buttons free.
		constexpr Def kDefs[] = {
			{ L"scanner", "default, Pad:DLeft:tap@vehicle, Pad:Y@scanner", "T: tap go / pick, double tap clear the route, hold 1 s ram; driving outside the scanner tap = Auto on / off", nullptr, nullptr },
			{ L"summon", "default, Pad:DUp@vehicle, Pad:B@scanner", "N: summon your vehicle (tap), hold: with all escorts", nullptr, nullptr },
			{ L"cover", "default, Pad:DDown@vehicle, Pad:LS@scanner", "U: follow (out of combat) / cover (in combat); hold: the follower drives", nullptr, nullptr },
			{ L"stance", "default, Pad:DRight@vehicle, Pad:View:tap@scanner", "Defend / Attack / Hold fire for your vehicle and escorts", nullptr, nullptr },
			{ L"hover_toggle", "default, Pad:LS@vehicle", "hover on / off", nullptr, nullptr },
			{ L"lift_up", "default, Pad:Y@flying", "hover: climb", nullptr, nullptr },
			{ L"lift_down", "default, Pad:X@flying", "hover: descend", nullptr, nullptr },
			{ L"gear", "default, Pad:A@vehicle", "Alt: tap = next gear, hold = descend", nullptr, nullptr },
			{ L"boost", "default-key, Pad:RB@vehicle", "LShift: boost; double tap in hover = boost stays on until boost or brake", "Vehicle", "VehicleBoost" },
			{ L"accelerate", "default, Pad:LStickUp@flying|passenger", "the game's accelerate (W); also drives hover", "Vehicle", "VehicleAccel" },
			{ L"brake", "default, Pad:LStickDown@flying|passenger", "the game's brake (S); also hover", "Vehicle", "VehicleBrake" },
			{ L"turn_left", "default, Pad:LStickLeft", "the game's turn left (A); hover: roll left", "Vehicle", "VehicleTurnLeft" },
			{ L"turn_right", "default, Pad:LStickRight", "the game's turn right (D); hover: roll right", "Vehicle", "VehicleTurnRight" },
			{ L"vertical_boost", "default", "the game's vertical boost (Space)", "Vehicle", "VehicleVertBoost" },
			{ L"ride_along", "default, Pad:DRight:hold@scanner, Pad:A@passenger", "next to a vehicle an NPC flies: ride along / get out", nullptr, nullptr },
			{ L"retry_trip", "default, Pad:DLeft:tap@scanner", "route: retry the trip", nullptr, nullptr },
			{ L"skip_stop", "default, Pad:DRight:tap@scanner", "route: skip the stop", nullptr, nullptr },
			{ L"own_profile", "default, Pad:DLeft:hold@vehicle|scanner", "your vehicle's profile; driving: Stop / Orbit / Patrol / Cruise", nullptr, nullptr },
			{ L"show_hints", "default, Pad:RS:tap@vehicle, Pad:View:hold@scanner", "J: show every hint and panel", nullptr, nullptr },
		};
		static_assert(std::size(kDefs) == std::size_t(Action::Count), "one bindings.ini line per action");
		// Layout 2's lines (the LB chords), in Action order: a line still exactly at one of these, or at "default", takes
		// layout 3 once. A line the player edited is kept.
		constexpr const char* kLayout2[] = { "default, Pad:LB+A", "default, Pad:LB+DUp", "default, Pad:LB+DDown", "default, Pad:LB+DRight",
			"default, Pad:LB+Y", "default, Pad:RB", "default, Pad:LB+RB", "default, Pad:LB+X", "default", "default, Pad:RT", "default, Pad:LT",
			"default, Pad:LStickLeft", "default, Pad:LStickRight", "default", "default, Pad:LB+B", "default, Pad:LB+DLeft", "default, Pad:LB+RS",
			"default", "default, Pad:LB+View" };
		static_assert(std::size(kLayout2) == std::size(kDefs));

		std::mutex g_lock;
		std::array<bindlogic::Binding, std::size_t(Action::Count)> g_bind{};
		std::array<std::uint32_t, std::size_t(Action::Count)> g_gamePad{};  // the game's controller button for "default"
		std::array<int, std::size_t(Action::Count)> g_defaultVk{};
		std::array<bool, std::size_t(Action::Count)> g_defaultSeen{};
		std::atomic<unsigned> g_generation{0};
		int g_captureAction=-1;
		bindlogic::Capture g_capture;
		ULONGLONG g_captureAt=0;
		bool g_suppressInput=false;
		constexpr const char* kNames[]={"Focus target / scanner destination","Summon vehicle","Follow / cover","Combat stance","Toggle hover","Climb","Descend","Change gear / descend","Boost","Accelerate","Brake","Turn left","Turn right","Vertical boost","Ride along / exit","Retry trip","Skip route stop","Vehicle profile","Show hints"};
		static_assert(std::size(kNames)==std::size(kDefs));
		std::uint32_t g_modifiers = 0;
		bool g_loaded = false;
		FILETIME g_stamp{};
		ULONGLONG g_fileAt = 0, g_gameAt = 0;

		std::atomic<std::uint32_t> g_pad{ 0 };
		// The device the prompts name: it changes only on a fresh button press from the other device (bindlogic::DeviceTracker).
		// GetLastInputInfo was used before and flipped the prompts on any mouse move, even in another window.
		std::atomic<bool> g_padPressed{ false };      // PollPad -> Tick: a controller button or trigger was newly pressed
		std::atomic<bool> g_padShown{ false };        // Tick's tracker result, read by Label on any thread
		bindlogic::DeviceTracker g_device;            // Tick only
		std::array<bool, 256> g_keyWasDown{};         // Tick only: key and mouse button states for fresh presses
		std::uint32_t g_padPrev = 0;
		// The situation now (bindlogic kCtx*, Tick), the tap / hold results per action and alternative (Tick, under g_lock),
		// and the controller buttons whose game action is off right now (inputgate).
		std::atomic<std::uint8_t> g_ctx{ bindlogic::kCtxFoot };
		std::array<std::vector<bindlogic::GestureState>, std::size_t(Action::Count)> g_gestureState{};
		std::array<std::vector<bool>, std::size_t(Action::Count)> g_gestureOn{};
		std::uint32_t g_blockedShown = 0;
		int g_styleSetting = 0;                      // [display] pad_names: 0 auto, 1 xbox, 2 playstation
		std::atomic<int> g_detected{ 0 };             // auto: 2 when a Sony controller is attached
		ULONGLONG g_styleAt = 0;
		bool g_lastShownPad = false;
		bindlogic::PadStyle g_lastStyle = bindlogic::PadStyle::Xbox;
		std::atomic<bool> g_connected{ false };
		std::atomic<ULONGLONG> g_padAt{ 0 };
		std::mutex g_padLock;
		std::mutex g_axesLock;
		bindlogic::Axes g_axes;   // the controller's sticks and triggers (PollPad), for analog amounts
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
				";   Pad:DUp@vehicle                   a controller button only in these situations: foot, ground, hover,\n"
				";                                     vtol, scanner, passenger, flying (hover|vtol), vehicle (all three);\n"
				";                                     join with |. There the game's own action on that button is off.\n"
				";   Pad:DLeft:tap / Pad:DLeft:hold    a short press / a press held 0.5 s, so two actions can share a button\n"
				";   Pad:LB+DUp                        a chord: hold LB, press DUp. The game still acts on LB itself, so the\n"
				";                                     defaults use none.\n"
				";   default                           the mod's own key for it (the game's binding where it follows one,\n"
				";                                     with the game's controller button)\n"
				";   none                              nothing\n"
				"; Example: summon = default, Pad:DUp@vehicle, Pad:B@scanner\n"
				";\n"
				"; Controller defaults: driving, DUp summon, DDown follow / cover, DRight stance, DLeft tap Auto (hold: Stop /\n"
				"; Orbit / Patrol / Cruise), A gear (hold: descend), L3 hover, R3 hints, RB boost, hovering Y / X climb / descend.\n"
				"; In the scanner (LB, also on foot): Y go there, B summon, L3 follow / cover, View stance (hold: hints), DLeft\n"
				"; retry (hold: profile), DRight skip (hold: ride along). Prompts name the device you pressed a button on last;\n"
				"; [display] pad_names = auto / xbox / playstation.\n"
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
			// Layout 3 (2026-10-24): the situation-based controller map. A line still at "default" or at its layout 2 line
			// takes the new default once.
			const bool upgrade = GetPrivateProfileIntW(L"meta", L"layout", 1, path.c_str()) < 3;
			if (upgrade) WritePrivateProfileStringW(L"meta", L"layout", L"3", path.c_str());
			{
				wchar_t w[32] = {};
				if (GetPrivateProfileStringW(L"display", L"pad_names", L"", w, 32, path.c_str()) == 0) WritePrivateProfileStringW(L"display", L"pad_names", L"auto", path.c_str());
				const std::string v = Narrow(w);
				g_styleSetting = bindlogic::Same(v, "xbox") ? 1 : bindlogic::Same(v, "playstation") ? 2 : 0;
			}
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
				const auto was = bindlogic::Trim(spec);
				if (upgrade && (bindlogic::Same(was, "default") || bindlogic::Same(was, kLayout2[i])) && !bindlogic::Same(was, kDefs[i].spec)) {
					spec = kDefs[i].spec;
					const std::wstring def(spec.begin(), spec.end());
					WritePrivateProfileStringW(L"bindings", kDefs[i].key, def.c_str(), path.c_str());
					logger::Write("Bindings: %s upgraded to the controller default (%s)", Narrow(kDefs[i].key).c_str(), spec.c_str());
				}
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
			g_generation.fetch_add(1);
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
				const auto mapped=bindlogic::GamePadBit(code);
				if(g_gamePad[i]!=mapped){g_gamePad[i]=mapped;g_generation.fetch_add(1);}
				if (!told) logger::Write("Bindings: the game's controller binding for %s:%s is code 0x%X -> %s", kDefs[i].gameContext, kDefs[i].gameEvent, code,
					g_gamePad[i] ? bindlogic::PadLabel(g_gamePad[i]).c_str() : "not used");
			}
			if (!told) {
				// Evidence: which game actions share the mod's controller buttons (the game reads them too).
				std::uint32_t bits = 0;
				for (const auto& b : g_bind) for (const auto& a : b) bits |= a.pad | a.mod;
				for (std::uint32_t bit = 1; bit && bit <= 0x8000; bit <<= 1)
					if (bits & bit) logger::Write("Bindings: controller %s is also the game's: %s", bindlogic::PadLabel(bit).c_str(), controlmap::WhoUsesPad(bit).c_str());
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
				const auto bits = bindlogic::PadBits(p.wButtons, p.bLeftTrigger, p.bRightTrigger, p.sThumbLX, p.sThumbLY, p.sThumbRX, p.sThumbRY);
				if (bindlogic::PadPresses(g_padPrev, bits)) g_padPressed.store(true, std::memory_order_relaxed);
				g_padPrev = bits;
				g_pad.store(bits, std::memory_order_relaxed);
				{
					auto stick = [](SHORT v) { return std::clamp(double(v) / 32767.0, -1.0, 1.0); };
					std::lock_guard al(g_axesLock);
					g_axes = { stick(p.sThumbLX), stick(p.sThumbLY), stick(p.sThumbRX), stick(p.sThumbRY), p.bLeftTrigger / 255.0, p.bRightTrigger / 255.0 };
				}
				return;
			}
			if (g_user >= 0) { logger::Write("Bindings: controller %d disconnected", g_user); g_user = -1; g_connected = false; }
			g_pad.store(0, std::memory_order_relaxed);
			{ std::lock_guard al(g_axesLock); g_axes = {}; }
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

		// A Sony pad among the attached HID game controllers (XInput cannot tell; Steam / DS4Windows still show the hardware).
		void DetectStyle(ULONGLONG now)
		{
			if (g_styleAt && now - g_styleAt < 5000) return;
			g_styleAt = now;
			UINT n = 0;
			if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return;
			std::vector<RAWINPUTDEVICELIST> list(n);
			if (GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return;
			int found = 0;
			for (UINT i = 0; i < n; ++i) {
				if (list[i].dwType != RIM_TYPEHID) continue;
				RID_DEVICE_INFO info{};
				info.cbSize = sizeof(info);
				UINT size = sizeof(info);
				if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1)) continue;
				const auto& h = info.hid;
				if (h.usUsagePage != 1 || (h.usUsage != 4 && h.usUsage != 5)) continue;
				if (h.dwVendorId == 0x054C) found = 2;
				else if (!found) found = 1;
			}
			if (g_detected.exchange(found) != found) logger::Write("Bindings: controller names: %s", found == 2 ? "PlayStation (Sony controller attached)" : "Xbox");
		}
		bindlogic::PadStyle Style()
		{
			const int s = g_styleSetting ? g_styleSetting : g_detected.load();
			return s == 2 ? bindlogic::PadStyle::PlayStation : bindlogic::PadStyle::Xbox;
		}
		bool PadInUse() { return g_connected.load() && g_padShown.load(std::memory_order_relaxed); }
		// The one situation the player is in (main thread): the scanner wins, then riding as a passenger, then the
		// vehicle being driven (physical VTOL, hover, wheels), else on foot.
		std::uint8_t Situation()
		{
			if (hoverui::IsScannerOpen()) return bindlogic::kCtxScanner;
			if (passenger::RidingVehicle()) return bindlogic::kCtxPassenger;
			const auto player = *reinterpret_cast<std::uintptr_t*>(game::Base() + addr::kPlayerCharacterPtr);
			const auto driven = player ? reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(game::Base() + addr::kPlayerVehicle)(player) : 0;
			if (!driven) return bindlogic::kCtxFoot;
			if (vehicleflight::PhysicalCameraActive(driven)) return bindlogic::kCtxVtol;
			return vehicleflight::Active() ? bindlogic::kCtxHover : bindlogic::kCtxGround;
		}
		std::string PadList(std::uint32_t bits)
		{
			std::string s;
			for (const auto& n : bindlogic::PadNames()) if ((bits & n.bit) && std::strcmp(n.name, "Back") != 0) { if (!s.empty()) s += " "; s += n.name; }
			return s.empty() ? "none" : s;
		}
		// A key or mouse button newly pressed since the last call, counted only while the game has the focus.
		bool FreshKeyPress()
		{
			DWORD pid = 0;
			const HWND fg = GetForegroundWindow();
			const bool focused = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
			bool fresh = false;
			for (int vk = 1; vk < 256; ++vk) {
				const bool down = focused && KeyDown(vk);
				if (down && !g_keyWasDown[vk]) fresh = true;
				g_keyWasDown[vk] = down;
			}
			return fresh;
		}
		int CaptureKey()
		{
			for(int vk=1;vk<256;++vk){
				if(vk==VK_SHIFT||vk==VK_CONTROL||vk==VK_MENU)continue; // prefer the left/right key
				if(KeyDown(vk)&&(vk==VK_ESCAPE||bindlogic::KeyCode(bindlogic::KeyLabel(vk))==vk))return vk;
			}
			return 0;
		}
		bool SaveLocked(std::size_t i,const bindlogic::Binding& binding)
		{
			const auto spec=bindlogic::Spec(binding);const std::wstring wide(spec.begin(),spec.end());
			const auto path=IniPath();
			if(path.empty()||!WritePrivateProfileStringW(L"bindings",kDefs[i].key,wide.c_str(),path.c_str())){
				logger::Write("Bindings: could not save %s from Gameplay Options",kNames[i]);return false;
			}
			Load(false);return true;
		}
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
		if (g_connected.load()) DetectStyle(now);
		const bool keyPress = FreshKeyPress(), padPress = g_padPressed.exchange(false, std::memory_order_relaxed);
		if (!g_connected.load()) g_device.device = bindlogic::Device::Keyboard;
		else if (g_device.Update(keyPress, padPress)) logger::Write("Bindings: prompts now name the %s", g_device.device == bindlogic::Device::Pad ? "controller" : "keyboard and mouse");
		g_padShown.store(g_device.device == bindlogic::Device::Pad, std::memory_order_relaxed);
		{
			// The situation, the tap / hold gestures, and the buttons whose game action is off (layout 3).
			const std::uint8_t ctx = Situation();
			if (g_ctx.exchange(ctx) != ctx) g_generation.fetch_add(1);
			const std::uint32_t pad = g_pad.load(std::memory_order_relaxed);
			std::uint32_t blocked = 0;
			for (std::size_t i = 0; i < std::size(kDefs); ++i) {
				const auto& b = g_bind[i];
				auto& st = g_gestureState[i];
				auto& on = g_gestureOn[i];
				st.resize(b.size());
				on.assign(b.size(), false);
				for (std::size_t j = 0; j < b.size(); ++j)
					if (b[j].gesture != bindlogic::Gesture::Raw)
						on[j] = st[j].Update(bindlogic::InCtx(b[j], ctx) && bindlogic::PadOn(b[j], pad, g_modifiers), b[j].gesture, now);
				// The game's own vehicle actions the mod follows (boost, accelerate ...) stay the game's.
				if (!kDefs[i].gameContext) blocked |= bindlogic::GameBlockBits(b, ctx, pad);
			}
			if (!g_connected.load()) blocked = 0;
			inputgate::SetBlocked(blocked);
			if (blocked != g_blockedShown) {
				g_blockedShown = blocked;
				logger::Write("Bindings: situation %u, the game's controller actions off on: %s", unsigned(ctx), PadList(blocked).c_str());
			}
		}
		const bool shownPad = PadInUse();
		const auto style = Style();
		if (shownPad != g_lastShownPad || style != g_lastStyle) { g_lastShownPad = shownPad; g_lastStyle = style; g_generation.fetch_add(1); }
		if(g_captureAction<0&&g_suppressInput&&!CaptureKey()&&!g_pad.load())g_suppressInput=false;
	}

	bool Held(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return false;
		PollPad(GetTickCount64());
		const std::uint32_t pad = g_pad.load(std::memory_order_relaxed);
		std::lock_guard lk(g_lock);
		if(!g_defaultSeen[i]||g_defaultVk[i]!=defaultVk){g_defaultVk[i]=defaultVk;g_defaultSeen[i]=true;g_generation.fetch_add(1);}
		if(g_captureAction>=0||g_suppressInput)return false;
		if (!g_loaded) return KeyDown(defaultVk);  // before the first Tick: the module's own key
		return bindlogic::Held(g_bind[i], defaultVk, pad, g_modifiers, KeyDown, g_gamePad[i], g_ctx.load(std::memory_order_relaxed), &g_gestureOn[i]);
	}

	double Amount(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return 0.0;
		PollPad(GetTickCount64());
		const std::uint32_t pad = g_pad.load(std::memory_order_relaxed);
		bindlogic::Axes axes;
		{ std::lock_guard al(g_axesLock); axes = g_axes; }
		std::lock_guard lk(g_lock);
		if(!g_defaultSeen[i]||g_defaultVk[i]!=defaultVk){g_defaultVk[i]=defaultVk;g_defaultSeen[i]=true;g_generation.fetch_add(1);}
		if(g_captureAction>=0||g_suppressInput)return 0.0;
		if (!g_loaded) return KeyDown(defaultVk) ? 1.0 : 0.0;
		return bindlogic::Amount(g_bind[i], defaultVk, pad, g_modifiers, KeyDown, g_gamePad[i], g_ctx.load(std::memory_order_relaxed), &g_gestureOn[i], axes);
	}

	std::string Label(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return "unbound";
		std::lock_guard lk(g_lock);
		if(!g_defaultSeen[i]||g_defaultVk[i]!=defaultVk){g_defaultVk[i]=defaultVk;g_defaultSeen[i]=true;g_generation.fetch_add(1);}
		if (!g_loaded) return defaultVk ? bindlogic::KeyLabel(defaultVk) : "unbound";
		return bindlogic::Label(g_bind[i], defaultVk, g_gamePad[i], PadInUse() ? bindlogic::Show::Pad : bindlogic::Show::Keyboard, Style(),
			g_ctx.load(std::memory_order_relaxed));
	}

	std::string Label(Action a)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return "unbound";
		int vk = 0;
		{ std::lock_guard lk(g_lock); vk = g_defaultSeen[i] ? g_defaultVk[i] : 0; }
		return Label(a, vk);
	}

	bool PadConnected() { return g_connected.load(); }
	bool UsingPad() { return PadInUse(); }
	bool GameHeld(const char* context, const char* event, int fallbackVk)
	{
		PollPad(GetTickCount64());
		if (KeyDown(controlmap::KeyFor(context, event, fallbackVk))) return true;
		const auto bit = bindlogic::GamePadBit(controlmap::PadFor(context, event));
		return bit && (g_pad.load(std::memory_order_relaxed) & bit) != 0;
	}
	std::string GameLabel(const char* context, const char* event, int fallbackVk)
	{
		const auto bit = bindlogic::GamePadBit(controlmap::PadFor(context, event));
		if (bit && PadInUse()) return bindlogic::PadLabel(bit, Style());
		const int vk = controlmap::KeyFor(context, event, fallbackVk);
		return vk ? bindlogic::KeyLabel(vk) : "unbound";
	}
	std::string PadName(std::uint32_t bit) { return bindlogic::PadLabel(bit, Style()); }
	const char* ActionName(Action a){const auto i=std::size_t(a);return i<std::size(kNames)?kNames[i]:"Control";}
	const char* ActionDescription(Action a){const auto i=std::size_t(a);return i<std::size(kDefs)?kDefs[i].what:"";}
	unsigned Generation(){return g_generation.load();}
	std::string MenuLabel(Action a)
	{
		const auto i=std::size_t(a);if(i>=std::size(kDefs))return {};
		std::lock_guard lk(g_lock);if(!g_loaded)Load(false);
		if(g_captureAction==int(i))return "Press an input...";
		auto text=bindlogic::Label(g_bind[i],g_defaultVk[i],g_gamePad[i],bindlogic::Show::Both,Style());
		if(!g_defaultSeen[i])for(const auto& b:g_bind[i])if(b.useDefault&&b.defaultKeyboard){text=text=="unbound"?"Default":"Default / "+text;break;}
		return text;
	}
	void BeginCapture(Action a)
	{
		if(std::size_t(a)>=std::size(kDefs))return;
		std::lock_guard lk(g_lock);if(!g_loaded)Load(false);
		g_captureAction=int(a);g_capture={};g_captureAt=GetTickCount64();g_suppressInput=true;g_generation.fetch_add(1);
	}
	void CancelCapture()
	{
		std::lock_guard lk(g_lock);if(g_captureAction>=0){g_captureAction=-1;g_generation.fetch_add(1);}
	}
	bool Capturing(Action* action)
	{
		std::lock_guard lk(g_lock);if(g_captureAction<0)return false;
		if(action)*action=Action(g_captureAction);return true;
	}
	void CaptureTick()
	{
		DWORD process=0;GetWindowThreadProcessId(GetForegroundWindow(),&process);
		if(process!=GetCurrentProcessId()){CancelCapture();return;}
		const auto now=GetTickCount64();PollPad(now);
		std::lock_guard lk(g_lock);if(g_captureAction<0)return;
		const auto result=g_capture.Update(CaptureKey(),g_pad.load());
		if(result.kind==bindlogic::Capture::Kind::Wait&&now-g_captureAt<20000)return;
		const auto i=std::size_t(g_captureAction);
		if(result.kind==bindlogic::Capture::Kind::Key||result.kind==bindlogic::Capture::Kind::Pad)SaveLocked(i,bindlogic::RemapDevice(g_bind[i],result.alt));
		else if(result.kind==bindlogic::Capture::Kind::Default)SaveLocked(i,bindlogic::Parse(kDefs[i].spec));
		else if(result.kind==bindlogic::Capture::Kind::Clear)SaveLocked(i,{});
		g_captureAction=-1;g_generation.fetch_add(1);
	}
	void ResetAll()
	{
		std::lock_guard lk(g_lock);if(!g_loaded)Load(false);
		const auto path=IniPath();if(path.empty())return;
		bool ok=true;
		for(const auto& def:kDefs){const std::wstring spec(def.spec,def.spec+std::strlen(def.spec));ok=WritePrivateProfileStringW(L"bindings",def.key,spec.c_str(),path.c_str())&&ok;}
		g_captureAction=-1;g_suppressInput=true;Load(false);
		logger::Write("Bindings: Gameplay Options restore defaults %s",ok?"saved":"incomplete (write failed)");
	}

	namespace
	{
		const std::array<std::string, std::size_t(Action::Count)>& EventNames()
		{
			static const auto names = [] {
				std::array<std::string, std::size_t(Action::Count)> n;
				for (std::size_t i = 0; i < n.size(); ++i) n[i] = "FVO_" + Narrow(kDefs[i].key);
				return n;
			}();
			return names;
		}
		std::string InputLabel(const bindlogic::Input& in)
		{
			if (in.vk) return bindlogic::KeyLabel(in.vk);
			return in.mod ? bindlogic::PadLabel(in.mod) + "+" + bindlogic::PadLabel(in.pad) : bindlogic::PadLabel(in.pad);
		}
		// Writes every changed line, then reloads once (under g_lock).
		void SaveLines(const std::array<bindlogic::Binding, std::size_t(Action::Count)>& next)
		{
			const auto path = IniPath();
			if (path.empty()) return;
			for (std::size_t i = 0; i < next.size(); ++i) {
				if (bindlogic::Spec(next[i]) == bindlogic::Spec(g_bind[i])) continue;
				const auto spec = bindlogic::Spec(next[i]);
				const std::wstring wide(spec.begin(), spec.end());
				if (!WritePrivateProfileStringW(L"bindings", kDefs[i].key, wide.c_str(), path.c_str()))
					logger::Write("Bindings: could not save %s from the Controls page", kNames[i]);
			}
			Load(false);
		}
	}

	const char* EventName(Action a) { const auto i = std::size_t(a); return i < EventNames().size() ? EventNames()[i].c_str() : ""; }
	bool FromEvent(const char* event, Action* a)
	{
		if (!event) return false;
		for (std::size_t i = 0; i < EventNames().size(); ++i)
			if (EventNames()[i] == event) { if (a) *a = Action(i); return true; }
		return false;
	}
	std::vector<bindlogic::Input> Slots(Action a, bool pad)
	{
		const auto i = std::size_t(a);
		std::vector<bindlogic::Input> out;
		if (i >= std::size(kDefs)) return out;
		std::lock_guard lk(g_lock);
		if (!g_loaded) Load(false);
		for (const auto& s : bindlogic::DeviceSlots(g_bind[i], pad, g_defaultVk[i], g_gamePad[i])) out.push_back(s.in);
		return out;
	}
	std::string Assign(Action a, bool pad, int slot, bindlogic::Input in)
	{
		const auto i = std::size_t(a);
		if (i >= std::size(kDefs) || in.Empty()) return {};
		std::lock_guard lk(g_lock);
		if (!g_loaded) Load(false);
		auto next = g_bind;
		std::string moved;
		for (std::size_t j = 0; j < next.size(); ++j) {
			if (j == i) continue;
			next[j] = bindlogic::Without(next[j], in, g_defaultVk[j], g_gamePad[j]);
			if (bindlogic::Spec(next[j]) != bindlogic::Spec(g_bind[j])) { if (!moved.empty()) moved += ", "; moved += kNames[j]; }
		}
		// Already in another slot of this action: it moves into this one.
		const auto own = bindlogic::DeviceSlots(next[i], pad, g_defaultVk[i], g_gamePad[i]);
		for (int s = 0; s < int(own.size()); ++s)
			if (s != slot && own[std::size_t(s)].in == in) { next[i] = bindlogic::SetSlot(next[i], pad, s, {}, g_defaultVk[i], g_gamePad[i]); if (s < slot) --slot; break; }
		next[i] = bindlogic::SetSlot(next[i], pad, slot, in, g_defaultVk[i], g_gamePad[i]);
		logger::Write("Bindings: Controls page: %s %s slot %d = %s%s%s", kNames[i], pad ? "controller" : "keyboard", slot, InputLabel(in).c_str(),
			moved.empty() ? "" : ", moved from ", moved.c_str());
		SaveLines(next);
		return moved;
	}
	void ClearSlot(Action a, bool pad, int slot)
	{
		const auto i = std::size_t(a);
		if (i >= std::size(kDefs)) return;
		std::lock_guard lk(g_lock);
		if (!g_loaded) Load(false);
		auto next = g_bind;
		next[i] = bindlogic::SetSlot(next[i], pad, slot, {}, g_defaultVk[i], g_gamePad[i]);
		logger::Write("Bindings: Controls page: %s %s slot %d cleared", kNames[i], pad ? "controller" : "keyboard", slot);
		SaveLines(next);
	}
}

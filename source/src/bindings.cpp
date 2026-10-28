#include "bindings.h"
#include "commandinput.h"
#include "inputevents.h"
#include "hudprompts.h"

#include "addresses.h"
#include "bindlogic.h"
#include "controlmap.h"
#include "game.h"
#include "hoverui.h"
#include "inputgate.h"
#include "log.h"
#include "notify.h"
#include "passenger.h"
#include "realvehicles.h"
#include "vehicleflight.h"
#include "fvopaths.h"

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

		// Layout 2's lines (the LB chords), in Action order: a line still exactly at one of these, or at "default", takes
		// layout 3 once. A line the player edited is kept.
		constexpr const char* kLayout2[] = { "default, Pad:LB+A", "default, Pad:LB+DUp", "default, Pad:LB+DDown", "default, Pad:LB+DRight",
			"default, Pad:LB+Y", "default, Pad:RB", "default, Pad:LB+RB", "default, Pad:LB+X", "default", "default, Pad:RT", "default, Pad:LT",
			"default, Pad:LStickLeft", "default, Pad:LStickRight", "default", "default, Pad:LB+B", "default, Pad:LB+DLeft", "default, Pad:LB+RS",
			"default", "default, Pad:LB+View" };


        constexpr const char* kLayout3[] = {
            "default, Pad:DLeft:tap@vehicle, Pad:Y@scanner",
            "default, Pad:DUp@vehicle, Pad:B@scanner",
            "default, Pad:DDown@vehicle, Pad:LS@scanner",
            "default, Pad:DRight@vehicle, Pad:View:tap@scanner",
            "default, Pad:LS@vehicle",
            "default, Pad:Y@flying",
            "default, Pad:X@flying",
            "default, Pad:A@vehicle",
            "default-key, Pad:RB@vehicle",
            "default, Pad:LStickUp@flying|passenger",
            "default, Pad:LStickDown@flying|passenger",
            "default, Pad:LStickLeft",
            "default, Pad:LStickRight",
            "default",
            "default, Pad:DRight:hold@scanner, Pad:A@passenger",
            "default, Pad:DLeft:tap@scanner",
            "default, Pad:DRight:tap@scanner",
            "default, Pad:DLeft:hold@vehicle|scanner",
            "default, Pad:RS:tap@vehicle, Pad:View:hold@scanner",
        };

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
        const auto kNames = [] {std::array<const char*,std::size(kDefs)> n{};for(std::size_t i=0;i<n.size();++i)n[i]=kDefs[i].what;return n;}();
        commandinput::Frame g_commands;
        inputevents::Buffer<commandinput::Count> g_triggerEvents,g_startEvents;
        commandinput::Sample g_sample;
        std::array<std::uint8_t,std::size_t(Action::Count)> g_promptScopes{};

        bool g_allowed=false, g_capturePad=false;
        bool LayerOn(std::size_t i) {
            const auto layer=kDefs[i].layer;
            const bool recording=g_sample.placement;
            return layer==Layer::Placement?recording:layer==Layer::Target?!recording:true;
        }

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
			std::wstring dir = fvopaths::Dir();
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
                "; Flying Vehicle Overhaul 0.73.0: independent commands, binding layout 4.\n"
                "; Alternatives are comma separated. Each command has its own keyboard and controller slots.\n"
                "; Examples: F6:tap@scanner, Pad:Y:tap@scanner, Pad:A:hold@passenger.\n"
                "; Gestures: raw (held), :tap, :hold (0.5 s), :double (within 0.3 s).\n"
                "; Contexts: foot, ground, hover, vtol, flying, vehicle, scanner, passenger, editor; join with |.\n"
                "; default-key follows this command's keyboard default; default-pad follows its native pad default.\n"
                "; Rebinding/clearing one device slot leaves the other device and other contexts alone.\n"
                "; UI rebinding rejects real same-context/gesture conflicts. none unbinds this command.\n"
                "; Open the scanner and double tap View/Create for the controller HUD editor; Circle/B saves and closes.\n"
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
			const bool upgrade = GetPrivateProfileIntW(L"meta", L"layout", 1, path.c_str()) < 4;
            if(upgrade) CopyFileW(path.c_str(),(path+L".before-layout4").c_str(),TRUE);
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
				if (upgrade && (bindlogic::Same(was, "default") || (i<std::size(kLayout2)&&bindlogic::Same(was,kLayout2[i])) || (i<std::size(kLayout3)&&bindlogic::Same(was,kLayout3[i]))) && !bindlogic::Same(was, kDefs[i].spec)) {
					spec = kDefs[i].spec;
					const std::wstring def(spec.begin(), spec.end());
					WritePrivateProfileStringW(L"bindings", kDefs[i].key, def.c_str(), path.c_str());
					logger::Write("Bindings: %s upgraded to the controller default (%s)", Narrow(kDefs[i].key).c_str(), spec.c_str());
				}
				if (const auto semi = spec.find(';'); semi != std::string::npos && semi > 0 && spec[semi - 1] == ' ') spec.resize(semi);  // a trailing "; comment"
				std::string bad;
                g_bind[i] = bindlogic::Parse(spec, &bad);
                g_promptScopes[i]=hudprompts::Contexts(bindlogic::Parse(kDefs[i].spec));
                if(!g_defaultSeen[i])g_defaultVk[i]=kDefs[i].defaultVk; g_defaultSeen[i]=true;
                g_gestureOn[i].assign(g_bind[i].size(),false);

				if (!bad.empty()) {
					++problems;
					logger::Write("Bindings: %s: not understood: %s (the rest of the line is used)", Narrow(kDefs[i].key).c_str(), bad.c_str());
				}
				logger::Write("Bindings: %-14s = %s", Narrow(kDefs[i].key).c_str(), spec.c_str());
			}
			g_modifiers = bindlogic::Modifiers(std::vector<bindlogic::Binding>(g_bind.begin(), g_bind.end()));
            if(upgrade)WritePrivateProfileStringW(L"meta",L"layout",L"4",path.c_str());
            g_suppressInput=true;g_commands.Reset();g_triggerEvents.Reset();g_startEvents.Reset();
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
                const int key=controlmap::KeyFor(kDefs[i].gameContext,kDefs[i].gameEvent,kDefs[i].defaultVk);
                if(g_defaultVk[i]!=key){g_defaultVk[i]=key;g_generation.fetch_add(1);}
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
		bool PadInUse() { return g_padShown.load(std::memory_order_relaxed); }
		// The one situation the player is in (main thread): the scanner wins, then riding as a passenger, then the
		// vehicle being driven (physical VTOL, hover, wheels), else on foot.
		std::uint8_t Situation()
		{
            if(hoverui::Editing())return bindlogic::kCtxEditor;
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
				const bool down = focused && bindlogic::KeyboardMouseKey(unsigned(vk)) && KeyDown(vk);
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

	void Tick(bool gameplay)
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
        DWORD focusPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&focusPid);
        if(g_device.Observe(inputgate::LastDevicePress(),keyPress,padPress&&focusPid==GetCurrentProcessId()))
            logger::Write("Bindings: prompts now name the %s",g_device.device==bindlogic::Device::Pad?"controller":"keyboard and mouse");
		g_padShown.store(g_device.device == bindlogic::Device::Pad, std::memory_order_relaxed);
        {
            const auto ctx=Situation();
            const bool contextChanged=g_ctx.exchange(ctx)!=ctx;
            if(contextChanged)g_generation.fetch_add(1);
            const bool allowed=inputgate::GameplayAllowed();
            g_allowed=allowed;
            g_sample={};g_sample.context=ctx;g_sample.pad=g_pad.load();
            {std::lock_guard axes(g_axesLock);g_sample.axes=g_axes;}
            g_sample.allowed=allowed&&!g_suppressInput&&g_captureAction<0;
            const auto player=*reinterpret_cast<std::uintptr_t*>(game::Base()+addr::kPlayerCharacterPtr);
            const auto driven=player?reinterpret_cast<std::uintptr_t(*)(std::uintptr_t)>(game::Base()+addr::kPlayerVehicle)(player):0;
            g_sample.driving=driven&&!passenger::RidingVehicle();
            g_sample.placement=!g_sample.driving&&realvehicles::RecordingPlacements();
            for(int key=1;key<256;++key)g_sample.keys[std::size_t(key)]=KeyDown(key);
            g_commands.Update(g_bind,g_defaultVk,g_gamePad,g_sample,now);
            g_triggerEvents.Sample(g_commands.triggered,g_commands.enabled,ctx,g_sample.allowed);
            g_startEvents.Sample(g_commands.started,g_commands.enabled,ctx,g_sample.allowed);
            if(gameplay){g_triggerEvents.Publish();g_startEvents.Publish();}
            g_modifiers=g_commands.modifiers;
            std::uint32_t blocked=0;std::array<bool,256> keys{};
            for(std::size_t i=0;i<std::size(kDefs);++i){
                g_gestureOn[i]=g_commands.on[i];
                if(!g_sample.allowed||!g_sample.Available(i))continue;
                for(const auto& alt:g_bind[i])if(bindlogic::InCtx(alt,ctx)){
                    const int vk=alt.useDefault?(alt.defaultKeyboard?g_defaultVk[i]:0):alt.vk;
                    const auto pb=alt.useDefault?(alt.defaultController?g_gamePad[i]:0):alt.pad;
                    bool nativeKey=kDefs[i].gameContext&&vk==g_defaultVk[i];
                    bool nativePad=kDefs[i].gameContext&&(pb==g_gamePad[i]||(Action(i)==Action::Boost&&pb==bindlogic::kRB));
                    if(Action(i)==Action::DriverExit&&vehicleflight::Active())nativeKey=nativePad=false;
                    if(!nativeKey&&vk>0&&vk<256)keys[std::size_t(vk)]=true;
                    if(!nativePad&&pb&&(!alt.mod||(g_sample.pad&alt.mod)))blocked|=pb&bindlogic::kPadPressBits;
                }
            }
            // 2026-10-28 (the user: "restrict input from wasd" in the passenger seat): the driver's own job flies it; the game
            // never gets the passenger's movement, boost or jump keys.
            if(ctx==bindlogic::kCtxPassenger)for(const int vk:{int('W'),int('A'),int('S'),int('D'),VK_SPACE,VK_LSHIFT,VK_SHIFT})keys[std::size_t(vk)]=true;
            inputgate::SetBlocked(g_connected?blocked:0);
            inputgate::SetBlockedKeys(keys);
            if(blocked!=g_blockedShown){g_blockedShown=blocked;logger::Write("Bindings: situation %u, owned controller buttons: %s",unsigned(ctx),PadList(blocked).c_str());}
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
		(void)defaultVk; defaultVk=g_defaultVk[i];
		if(!g_allowed||!LayerOn(i)||g_captureAction>=0||g_suppressInput)return false;
		if (!g_loaded) return KeyDown(defaultVk);  // before the first Tick: the module's own key
        (void)pad;for(bool value:g_gestureOn[i])if(value)return true;return false;
	}

	double Amount(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return 0.0;
		PollPad(GetTickCount64());
		const std::uint32_t pad = g_pad.load(std::memory_order_relaxed);
        std::lock_guard lk(g_lock);(void)defaultVk;(void)pad;
        if(!g_allowed||!LayerOn(i)||g_captureAction>=0||g_suppressInput)return 0;
        return g_commands.Amount(i,g_bind,g_defaultVk,g_gamePad,g_sample);
	}

	std::string Label(Action a, int defaultVk)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return "unbound";
		std::lock_guard lk(g_lock);
		(void)defaultVk; defaultVk=g_defaultVk[i];
		if (!g_loaded) return defaultVk ? bindlogic::KeyLabel(defaultVk) : "unbound";
        const bool pad=PadInUse();const auto ctx=g_ctx.load();
        const auto text=bindlogic::Label(g_bind[i],defaultVk,g_gamePad[i],pad?bindlogic::Show::Pad:bindlogic::Show::Keyboard,Style(),ctx);
        if(pad&&ctx==bindlogic::kCtxFoot&&text=="unbound"){
            const auto scan=bindlogic::Label(g_bind[i],defaultVk,g_gamePad[i],bindlogic::Show::Pad,Style(),bindlogic::kCtxScanner);
            if(scan!="unbound")return "Scanner > "+scan;
        }
        return text;
	}

	std::string Label(Action a)
	{
		const std::size_t i = std::size_t(a);
		if (i >= std::size(kDefs)) return "unbound";
		int vk = 0;
		{ std::lock_guard lk(g_lock); vk = g_defaultSeen[i] ? g_defaultVk[i] : 0; }
		return Label(a, vk);
	}

    void ResetRuntime(){std::lock_guard lk(g_lock);g_commands.Reset();g_triggerEvents.Reset();g_startEvents.Reset();g_allowed=false;g_suppressInput=true;g_captureAction=-1;}
    void EndUpdate(){std::lock_guard lk(g_lock);g_triggerEvents.End();g_startEvents.End();}
    bool Triggered(Action a){const auto i=std::size_t(a);std::lock_guard lk(g_lock);return g_allowed&&g_triggerEvents.Active(i);}
    bool Started(Action a){const auto i=std::size_t(a);std::lock_guard lk(g_lock);return g_allowed&&g_startEvents.Active(i);}
    bool Allowed(){std::lock_guard lk(g_lock);return g_allowed&&!g_suppressInput&&g_captureAction<0;}
	bool PadConnected() { return g_connected.load(); }
	std::uint32_t PadState() { PollPad(GetTickCount64()); return g_pad.load(std::memory_order_relaxed); }
	bool UsingPad() { return PadInUse(); }
	bool GameHeld(const char* context, const char* event, int fallbackVk)
	{
        if(!Allowed())return false;
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
    std::string GameLabelFor(const char* context,const char* event,bool controller){
        if(controller){
            const auto code=controlmap::PadFor(context,event);if(code==0xB)return "Left stick";if(code==0xC)return "Right stick";
            const auto bit=bindlogic::GamePadBit(code);return bit?bindlogic::PadLabel(bit,Style()):"";
        }
        return controlmap::KeyboardMouseLabel(context,event);
    }
	const char* ActionName(Action a){const auto i=std::size_t(a);return i<std::size(kNames)?kNames[i]:"Control";}
	const char* ActionDescription(Action a){const auto i=std::size_t(a);return i<std::size(kDefs)?kDefs[i].what:"";}
	unsigned Generation(){return g_generation.load();}
    HudState HudSnapshot(bool controller,bool preview){
        std::lock_guard lk(g_lock);HudState state;if(!g_loaded)return state;
        state.context=g_sample.context;state.allowed=g_allowed&&!g_suppressInput&&g_captureAction<0;
        state.driving=g_sample.driving;state.placement=g_sample.placement;
        for(std::size_t i=0;i<std::size(kDefs);++i){
            const auto context=preview?bindlogic::kCtxAll:state.context;
            if(preview||g_sample.Available(i))state.resolved[i]=inlinehints::Resolve(g_bind[i],g_defaultVk[i],g_gamePad[i],controller,Style(),std::uint8_t(context));
            state.input[i]=hudprompts::Input(g_bind[i],g_defaultVk[i],g_gamePad[i],controller,Style(),std::uint8_t(context));
            state.available[i]=(preview||g_sample.Available(i))&&!state.input[i].empty();
            const auto scopes=g_bind[i].empty()?g_promptScopes[i]:hudprompts::Contexts(g_bind[i]);
            state.applicable[i]=preview||(g_sample.Available(i)&&(scopes&state.context));
            if(controller&&!preview&&state.context==bindlogic::kCtxFoot)
                state.scannerInput[i]=hudprompts::Input(g_bind[i],g_defaultVk[i],g_gamePad[i],true,Style(),bindlogic::kCtxScanner);
        }
        const auto edit=std::size_t(Action::HudEdit);
        state.editorClose=hudprompts::Input(g_bind[edit],g_defaultVk[edit],g_gamePad[edit],controller,Style(),bindlogic::kCtxEditor);
        return state;
    }
    std::string PromptInput(Action action,bool controller,std::uint8_t context){
        const auto i=std::size_t(action);std::lock_guard lk(g_lock);if(!g_loaded||i>=std::size(kDefs))return "";
        return hudprompts::Input(g_bind[i],g_defaultVk[i],g_gamePad[i],controller,Style(),context);
    }
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
		g_capturePad=PadInUse();g_captureAction=int(a);g_capture={};g_captureAt=GetTickCount64();g_suppressInput=true;g_generation.fetch_add(1);
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
        bindlogic::Capture::Result result;Action action;bool targetPad=false;
        {
            std::lock_guard lk(g_lock);if(g_captureAction<0)return;
            result=g_capture.Update(CaptureKey(),g_pad.load());
            if(result.kind==bindlogic::Capture::Kind::Wait&&now-g_captureAt<20000)return;
            action=Action(g_captureAction);targetPad=g_capturePad;g_captureAction=-1;g_generation.fetch_add(1);
        }
        if(result.kind==bindlogic::Capture::Kind::Key||result.kind==bindlogic::Capture::Kind::Pad)
            Assign(action,result.kind==bindlogic::Capture::Kind::Pad,0,{result.alt.vk,result.alt.pad,result.alt.mod});
        else if(result.kind==bindlogic::Capture::Kind::Clear)ClearSlot(action,targetPad,0);
        else if(result.kind==bindlogic::Capture::Kind::Default){
            std::lock_guard lk(g_lock);const auto i=std::size_t(action);
            const auto next=commandinput::RestoreDevice(g_bind[i],bindlogic::Parse(kDefs[i].spec),targetPad);
            SaveLocked(i,next);
        }
    }

	void ResetAll()
	{
		std::lock_guard lk(g_lock);if(!g_loaded)Load(false);
		const auto path=IniPath();if(path.empty())return;
		bool ok=true;
		for(std::size_t i=0;i<std::size(kDefs);++i){
            const bool pad=PadInUse();const auto next=commandinput::RestoreDevice(g_bind[i],bindlogic::Parse(kDefs[i].spec),pad);
            const auto spec=bindlogic::Spec(next);const std::wstring wide(spec.begin(),spec.end());ok=WritePrivateProfileStringW(L"bindings",kDefs[i].key,wide.c_str(),path.c_str())&&ok;
        }
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
    bool CanAssign(Action action,bool pad,int slot,bindlogic::Input in){
        const auto i=std::size_t(action);if(i>=std::size(kDefs)||in.Empty())return false;
        std::lock_guard lk(g_lock);if(!g_loaded)Load(false);
        const auto proposed=commandinput::ScopedSlot(g_bind[i],bindlogic::Parse(kDefs[i].spec),pad,slot,in,g_defaultVk[i],g_gamePad[i]);
        return commandinput::FindConflict(i,proposed,g_bind,g_defaultVk,g_gamePad,pad)==commandinput::Count;
    }

	bool Assign(Action action, bool pad, int slot, bindlogic::Input in)
	{
		const auto i = std::size_t(action);
		if (i >= std::size(kDefs) || in.Empty()) return false;
		std::lock_guard lk(g_lock);
		if (!g_loaded) Load(false);
		auto next = g_bind;
		std::string moved;
        const auto proposed=commandinput::ScopedSlot(next[i],bindlogic::Parse(kDefs[i].spec),pad,slot,in,g_defaultVk[i],g_gamePad[i]);
        const auto conflict=commandinput::FindConflict(i,proposed,next,g_defaultVk,g_gamePad,pad);
        if(conflict!=commandinput::Count){
            notify::Show("Binding unchanged: conflicts with %s",kNames[conflict]);
            logger::Write("Bindings: assignment refused: %s overlaps %s",kNames[i],kNames[conflict]);return false;
        }
        next[i]=proposed;
		logger::Write("Bindings: Controls page: %s %s slot %d = %s%s%s", kNames[i], pad ? "controller" : "keyboard", slot, InputLabel(in).c_str(),
			moved.empty() ? "" : ", moved from ", moved.c_str());
		SaveLines(next);
		return true;
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

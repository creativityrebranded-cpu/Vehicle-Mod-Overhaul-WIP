#include "controlmap.h"

#include "game.h"
#include "log.h"
#include "fvopaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace controlmap
{
	namespace
	{
		constexpr std::uint32_t kControlMapPtr = 0x061DE668;    // ControlMap*
		constexpr std::uint32_t kContextNames = 0x04A554E0;     // const char* [78] ("MainGameplay", ...)
		constexpr std::uint32_t kFixedStringData = 0x028CAE80;  // (handle) -> chars, for BSFixedString entries stored out of line
		constexpr int kContexts = 78;

		struct Binding
		{
			std::string context, event;
			std::uint32_t key = 0, key2 = 0xFF;
			int device = 0;  // 0 keyboard, 1 mouse
		};

		std::mutex g_lock;
		std::vector<Binding> g_keys;  // keyboard bindings
		std::vector<Binding> g_pads;  // gamepad bindings (device 2), read on their own so an odd layout never costs the keyboard map
		bool g_loaded = false;

		[[maybe_unused]] std::string FixedString(std::uintptr_t s)
		{
			if (!s) {
				return {};
			}
			const char* p = nullptr;
			if (((*reinterpret_cast<const std::uint32_t*>(s + 0x14) >> 1) & 1) == 0) {
				p = reinterpret_cast<const char*>(s + 0x18);
			}
			else {
				p = reinterpret_cast<const char* (*)(std::uintptr_t)>(game::Base() + kFixedStringData)(*reinterpret_cast<const std::uintptr_t*>(s + 8));
			}
			return p ? std::string(p) : std::string();
		}

		struct Raw
		{
			int context;
			const char* event;
			std::uint32_t key, key2;
			int device;
		};

		const char* FixedChars(std::uintptr_t s)
		{
			if (!s) {
				return nullptr;
			}
			if (((*reinterpret_cast<const std::uint32_t*>(s + 0x14) >> 1) & 1) == 0) {
				return reinterpret_cast<const char*>(s + 0x18);
			}
			return reinterpret_cast<const char* (*)(std::uintptr_t)>(game::Base() + kFixedStringData)(*reinterpret_cast<const std::uintptr_t*>(s + 8));
		}

		// Raw read under SEH (no C++ objects here): returns the number of entries, -1 on a fault.
		int ReadRaw(std::uintptr_t cm, Raw* out, int max)
		{
			int n = 0;
			__try {
				for (int i = 0; i < kContexts; ++i) {
					const auto ctx = *reinterpret_cast<const std::uintptr_t*>(cm + 0x10 + i * 8);
					if (!ctx) {
						continue;
					}
					for (int dev = 0; dev < 2; ++dev) {  // 0 keyboard, 1 mouse
						const auto count = *reinterpret_cast<const std::uint32_t*>(ctx + dev * 0x10);
						const auto data = *reinterpret_cast<const std::uintptr_t*>(ctx + 8 + dev * 0x10);
						for (std::uint32_t e = 0; data && e < count && e < 4096 && n < max; ++e) {
							const auto entry = data + e * 40;
							out[n].context = i;
							out[n].event = FixedChars(*reinterpret_cast<const std::uintptr_t*>(entry));
							out[n].key = *reinterpret_cast<const std::uint32_t*>(entry + 8);
							out[n].key2 = *reinterpret_cast<const std::uint32_t*>(entry + 0xC);
							out[n].device = dev;
							++n;
						}
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
			return n;
		}

		// Gamepad list of every context (device 2: +0x20 count, +0x28 entries, the keyboard / mouse lists' shape). Its own SEH read.
		int ReadPadRaw(std::uintptr_t cm, Raw* out, int max)
		{
			int n = 0;
			__try {
				for (int i = 0; i < kContexts; ++i) {
					const auto ctx = *reinterpret_cast<const std::uintptr_t*>(cm + 0x10 + i * 8);
					if (!ctx) continue;
					const auto count = *reinterpret_cast<const std::uint32_t*>(ctx + 0x20);
					const auto data = *reinterpret_cast<const std::uintptr_t*>(ctx + 0x28);
					for (std::uint32_t e = 0; data && e < count && e < 4096 && n < max; ++e) {
						const auto entry = data + e * 40;
						out[n].context = i;
						out[n].event = FixedChars(*reinterpret_cast<const std::uintptr_t*>(entry));
						out[n].key = *reinterpret_cast<const std::uint32_t*>(entry + 8);
						out[n].key2 = *reinterpret_cast<const std::uint32_t*>(entry + 0xC);
						out[n].device = 2;
						++n;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
			return n;
		}

		bool Load()
		{
			if (g_loaded) {
				return !g_keys.empty();
			}
			const auto cm = *reinterpret_cast<const std::uintptr_t*>(game::Base() + kControlMapPtr);
			if (!cm) {
				return false;  // not built yet: try again later
			}
			static Raw raw[8192];
			const int n = ReadRaw(cm, raw, 8192);
			g_loaded = true;
			if (n < 0) {
				logger::Write("Control map: could not be read (layout differs); no key is assumed free");
				return false;
			}
			for (int i = 0; i < n; ++i) {
				const auto namePtr = *reinterpret_cast<const char* const*>(game::Base() + kContextNames + raw[i].context * 8);
				g_keys.push_back({ namePtr ? namePtr : "?", raw[i].event ? raw[i].event : "", raw[i].key, raw[i].key2, raw[i].device });
			}
			g_pads.clear();
			const int np = ReadPadRaw(cm, raw, 8192);
			for (int i = 0; i < np; ++i) {
				const auto namePtr = *reinterpret_cast<const char* const*>(game::Base() + kContextNames + raw[i].context * 8);
				g_pads.push_back({ namePtr ? namePtr : "?", raw[i].event ? raw[i].event : "", raw[i].key, raw[i].key2, 2 });
			}
			{
				static bool saidPad = false;
				if (!saidPad) logger::Write("Control map: %s", np < 0 ? "gamepad bindings unreadable (layout differs)" : (std::to_string(np) + " gamepad bindings read").c_str());
				saidPad = true;
			}
			{
				static bool said = false;  // it is re-read every few seconds: say it once
				if (!said || logger::Verbose()) logger::Write("Control map: %zu keyboard and mouse bindings read", g_keys.size());
				said = true;
			}
			return !g_keys.empty();
		}

		// Mouse buttons: the game's mouse codes are the button index (0 left, 1 right, 2 middle, 3/4 side buttons;
		// wheel 8/9). -1: not a mouse button.
		int MouseCode(int vk)
		{
			switch (vk) {
			case VK_LBUTTON: return 0;
			case VK_RBUTTON: return 1;
			case VK_MBUTTON: return 2;
			case VK_XBUTTON1: return 3;
			case VK_XBUTTON2: return 4;
			default: return -1;
			}
		}

		bool Matches(const Binding& b, int vk)
		{
			const int mc = MouseCode(vk);
			if (mc >= 0) {
				return b.device == 1 && (b.key == std::uint32_t(mc) || b.key2 == std::uint32_t(mc));
			}
			// Keyboard codes in the control map are Windows virtual-key codes (0.49.2 compared scan codes: F7 matched "A").
			return b.device == 0 && (b.key == std::uint32_t(vk) || b.key2 == std::uint32_t(vk));
		}

		std::uint32_t ScanOf(int vk)
		{
			return MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
		}

		bool Gameplay(const std::string& context) { return context.find("Menu") == std::string::npos; }
	}

	bool KeyFree(int vk)
	{
		std::lock_guard lk(g_lock);
		if (!Load()) {
			return false;
		}
		for (const auto& b : g_keys) {
			if (Gameplay(b.context) && Matches(b, vk)) {
				return false;
			}
		}
		return true;
	}

	int KeyFor(const char* context, const char* event, int fallback)
	{
		std::lock_guard lk(g_lock);
		static ULONGLONG lastRead = 0;
		const ULONGLONG now = GetTickCount64();
		if (g_loaded && now - lastRead >= 5000) {
			g_loaded = false;
			g_keys.clear();
		}
		if (!Load()) {
			return fallback;
		}
		if (!lastRead || now - lastRead >= 5000) lastRead = now;
		for (const auto& b : g_keys) {
			if (b.device == 0 && b.context == context && b.event == event && b.key && b.key != 0xFF) {
				return int(b.key);
			}
		}
		return fallback;
	}

	std::uint32_t PadFor(const char* context, const char* event)
	{
		std::lock_guard lk(g_lock);
		if (!Load()) return 0;
		for (const auto& b : g_pads) {
			if (b.context == context && b.event == event && b.key && b.key != 0xFF) return b.key;
		}
		return 0;
	}

	bool KeyFreeIn(int vk, const char* context)
	{
		std::lock_guard lk(g_lock);
		if (!Load()) {
			return false;
		}
		for (const auto& b : g_keys) {
			if (b.context == context && Matches(b, vk)) {
				return false;
			}
		}
		return true;
	}

	namespace
	{
		struct Claimed { std::string owner, action; int vk; };
		std::vector<Claimed> g_claimed;
		std::mutex g_claimLock;
		const Claimed* ClaimOf(int vk)
		{
			for (const auto& c : g_claimed) {
				if (c.vk == vk) return &c;
			}
			return nullptr;
		}
	}

	bool Claim(const char* owner, const char* action, int vk)
	{
		if (vk <= 0) return false;
		std::lock_guard lk(g_claimLock);
		if (const Claimed* c = ClaimOf(vk)) {
			logger::Write("Keys: %s/%s wanted %s, already %s/%s", owner, action, KeyName(vk).c_str(), c->owner.c_str(), c->action.c_str());
			return false;
		}
		if (!KeyFree(vk)) return false;
		g_claimed.push_back({ owner, action, vk });
		return true;
	}

	int ClaimFirst(const char* owner, const char* action, std::initializer_list<int> vks)
	{
		for (const int vk : vks) {
			{
				std::lock_guard lk(g_claimLock);
				if (ClaimOf(vk)) continue;
			}
			if (Claim(owner, action, vk)) return vk;
		}
		return 0;
	}

	void Note(const char* owner, const char* action, int vk)
	{
		if (vk <= 0) return;
		std::lock_guard lk(g_claimLock);
		if (const Claimed* c = ClaimOf(vk)) {
			if (c->owner != owner) logger::Write("Keys: %s/%s uses %s, also %s/%s", owner, action, KeyName(vk).c_str(), c->owner.c_str(), c->action.c_str());
			return;
		}
		g_claimed.push_back({ owner, action, vk });
	}

	void LogTable()
	{
		std::lock_guard lk(g_claimLock);
		for (const auto& c : g_claimed) logger::Write("Keys: %-8s %-18s %s", c.owner.c_str(), c.action.c_str(), KeyName(c.vk).c_str());
	}

	bool DebugKeys()
	{
		static ULONGLONG lastRead = 0;
		static bool value = false;
		const ULONGLONG now = GetTickCount64();
		if (!lastRead || now - lastRead >= 5000) {
			lastRead = now;
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const std::wstring ini = fvopaths::Settings();
				CoTaskMemFree(docs);
				value = GetPrivateProfileIntW(L"keys", L"debug", 0, ini.c_str()) != 0;
			}
		}
		return value;
	}

	int FirstFree(std::initializer_list<int> vks)
	{
		for (const int vk : vks) {
			if (KeyFree(vk)) {
				return vk;
			}
		}
		return 0;
	}

	std::string WhoUses(int vk)
	{
		std::lock_guard lk(g_lock);
		if (!Load()) {
			return "control map unreadable";
		}
		std::string out;
		for (const auto& b : g_keys) {
			if (Matches(b, vk)) {
				if (!out.empty()) out += ", ";
				out += b.context + ":" + b.event;
			}
		}
		return out.empty() ? "nothing" : out;
	}

	std::string WhoUsesPad(std::uint32_t code, const char* only)
	{
		std::lock_guard lk(g_lock);
		if (!Load()) return "control map unreadable";
		std::string out;
		for (const auto& b : g_pads) {
			if (only && b.context != only) continue;
			if (b.key == code || b.key2 == code) { if (!out.empty()) out += ", "; out += b.context + ":" + b.event; }
		}
		return out.empty() ? "nothing" : out;
	}

	std::string KeyName(int vk)
	{
		switch (vk) {
		case VK_LBUTTON: return "Left mouse";
		case VK_RBUTTON: return "Right mouse";
		case VK_MBUTTON: return "Middle mouse";
		case VK_XBUTTON1: return "Mouse back";
		case VK_XBUTTON2: return "Mouse forward";
		default: break;
		}
		wchar_t w[32] = {};
		const LONG lparam = static_cast<LONG>(ScanOf(vk) << 16);
		if (GetKeyNameTextW(lparam, w, 32) > 0) {
			char a[64] = {};
			WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), nullptr, nullptr);
			return a;
		}
		char buf[16];
		std::snprintf(buf, sizeof(buf), "VK %02X", vk);
		return buf;
	}

	void Dump()
	{
		static bool done = false;
		std::lock_guard lk(g_lock);
		if (done || !Load()) {
			return;
		}
		done = true;
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return;
		}
		std::wstring path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessPlanets\\controlmap.txt";
		CoTaskMemFree(docs);
		FILE* f = _wfopen(path.c_str(), L"w");
		if (!f) {
			return;
		}
		std::fprintf(f, "context\tdevice\tevent\tcode\tkey name\tsecond code\n");
		for (const auto& b : g_keys) {
			if (b.device == 1) {
				static const char* names[] = { "Left mouse", "Right mouse", "Middle mouse", "Mouse back", "Mouse forward", "Mouse 6", "Mouse 7", "Mouse 8", "Wheel up", "Wheel down" };
				std::fprintf(f, "%s\tmouse\t%s\t0x%02X\t%s\t0x%02X\n", b.context.c_str(), b.event.c_str(), b.key, b.key < 10 ? names[b.key] : "?", b.key2);
				continue;
			}
			const UINT vk = b.key;
			wchar_t w[32] = {};
			char name[64] = "-";
			const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
			if (b.key && b.key != 0xFF && scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), w, 32) > 0) {
				WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof(name), nullptr, nullptr);
			}
			std::fprintf(f, "%s\tkeyboard\t%s\t0x%02X\t%s (VK %02X)\t0x%02X\n", b.context.c_str(), b.event.c_str(), b.key, name, vk, b.key2);
		}
		for (const auto& b : g_pads) std::fprintf(f, "%s\tgamepad\t%s\t0x%04X\t-\t0x%04X\n", b.context.c_str(), b.event.c_str(), b.key, b.key2);
		std::fclose(f);
		logger::Write("Control map: %zu keyboard and %zu gamepad bindings written to controlmap.txt", g_keys.size(), g_pads.size());
	}
}

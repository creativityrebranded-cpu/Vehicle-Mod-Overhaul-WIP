#include "controlspage.h"

#include "bindings.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "notify.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace controlspage
{
	namespace
	{
		using uptr = std::uintptr_t;
		constexpr std::uint32_t kRvaValidate = 0x022EEA90, kRvaApply = 0x022EF4C0;
		// Both start: mov r11,rsp / mov [r11+8],rbx / mov [r11+10h],rsi / mov [r11+18h],rdi (15 bytes, no RIP-relative).
		constexpr std::uint8_t kPrologue[15] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x49, 0x89, 0x73, 0x10, 0x49, 0x89, 0x7B, 0x18 };
		constexpr std::uint32_t kRvaTranslatorVtbl = 0x04D7FF08, kRvaTranslate = 0x02565670;  // .?AVScaleformImpl@BSScaleformTranslator@@, slot 2
		constexpr std::uint32_t kRvaSetResult = 0x031CE850;   // (TranslateInfo*, const wchar_t*, size_t len or -1)
		constexpr std::uint32_t kRvaFixedStringData = 0x028CAE80;

		using Map_t = std::uint8_t (*)(void* map, uptr* event, std::uint8_t device, std::int32_t* codes, std::uint8_t slot, std::uint8_t context);
		using Translate_t = void (*)(void* self, void* info);
		using SetResult_t = void (*)(void* info, const wchar_t* text, std::size_t len);
		Map_t g_validate = nullptr, g_apply = nullptr;
		Translate_t g_translate = nullptr;
		std::atomic<bool> g_installed{ false };

		const char* FixedString(const uptr* fs)
		{
			if (!fs || !*fs) return nullptr;
			const uptr e = *fs;
			if (((*reinterpret_cast<const std::uint32_t*>(e + 0x14) >> 1) & 1) == 0) return reinterpret_cast<const char*>(e + 0x18);
			return reinterpret_cast<const char* (*)(uptr)>(game::Base() + kRvaFixedStringData)(*reinterpret_cast<const uptr*>(e + 8));
		}
		bool Ours(uptr* event, bindings::Action* a)
		{
			__try { return bindings::FromEvent(FixedString(event), a); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool IsShoulder(std::uint32_t b) { return b == bindlogic::kLB || b == bindlogic::kRB || b == bindlogic::kLT || b == bindlogic::kRT; }
		// The game's captured codes -> our input. Keyboard codes are virtual keys; mouse codes count the buttons from 0;
		// controller codes are XInput button masks (bindlogic::GamePadBit). Two controller codes are a chord.
		bindlogic::Input FromGame(std::uint8_t device, const std::int32_t* codes)
		{
			const std::uint32_t c0 = std::uint32_t(codes[0]), c1 = std::uint32_t(codes[1]);
			if (device == 0) return { int(c0 & 0xFF), 0, 0 };
			if (device == 1) {
				constexpr int kMouseVk[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
				return c0 < 5 ? bindlogic::Input{ kMouseVk[c0], 0, 0 } : bindlogic::Input{};
			}
			if (device != 2) return {};
			const std::uint32_t a = bindlogic::GamePadBit(c0), b = c1 != 0xFF ? bindlogic::GamePadBit(c1) : 0;
			if (!a) return {};
			if (!b) return { 0, a, 0 };
			if (IsShoulder(b) && !IsShoulder(a)) return { 0, a, b };
			return { 0, b, a };
		}

		std::uint8_t ValidateDetour(void* map, uptr* event, std::uint8_t device, std::int32_t* codes, std::uint8_t slot, std::uint8_t context)
		{
			bindings::Action a{};
			if (!Ours(event, &a)) return g_validate(map, event, device, codes, slot, context);
			const auto in = FromGame(device, codes);
			logger::Write("Controls page: capture for %s: device %u slot %u codes 0x%X 0x%X context 0x%X -> %s", bindings::ActionName(a), device, slot,
				std::uint32_t(codes[0]), std::uint32_t(codes[1]), context, in.Empty() ? "refused" : "free");
			return in.Empty() || !bindings::CanAssign(a,device==2,slot?1:0,in) ? 1 : 0;
		}

		std::uint8_t ApplyDetour(void* map, uptr* event, std::uint8_t device, std::int32_t* codes, std::uint8_t slot, std::uint8_t context)
		{
			bindings::Action a{};
			if (!Ours(event, &a)) return g_apply(map, event, device, codes, slot, context);
			const auto in = FromGame(device, codes);
			if (in.Empty()) return 0;
            return bindings::Assign(a,device==2,slot?1:0,in)?1:0;
		}

		// "$FlyingVehicleOverhaul" -> the header; "$FlyingVehicleOverhaul_FVO_scanner[_KBM]" -> that action's name.
		const wchar_t* OurText(const wchar_t* key)
		{
			static const std::wstring header = L"FLYING VEHICLE OVERHAUL";
			static const auto names = [] {
				std::array<std::wstring, std::size_t(bindings::Action::Count)> n;
				for (std::size_t i = 0; i < n.size(); ++i) {
					wchar_t w[128] = {};
					MultiByteToWideChar(CP_UTF8, 0, bindings::ActionName(bindings::Action(i)), -1, w, 127);
					n[i] = w;
				}
				return n;
			}();
			static const std::wstring prefix = [] { std::wstring p = L"$"; for (const char* c = kContextName; *c; ++c) p += wchar_t(*c); return p; }();
			if (!key || std::wcsncmp(key, prefix.c_str(), prefix.size()) != 0) return nullptr;
			const wchar_t* rest = key + prefix.size();
			if (!*rest) return header.c_str();
			if (*rest != L'_') return nullptr;
			std::string ev;
			for (const wchar_t* c = rest + 1; *c && *c < 0x80; ++c) ev += char(*c);
			if (ev.size() > 4 && ev.compare(ev.size() - 4, 4, "_KBM") == 0) ev.resize(ev.size() - 4);
			bindings::Action a{};
			return bindings::FromEvent(ev.c_str(), &a) ? names[std::size_t(a)].c_str() : nullptr;
		}

		void TranslateDetour(void* self, void* info)
		{
			const wchar_t* text = nullptr;
			__try { text = OurText(*static_cast<const wchar_t* const*>(info)); }
			__except (EXCEPTION_EXECUTE_HANDLER) { text = nullptr; }
			if (!text) { g_translate(self, info); return; }
			reinterpret_cast<SetResult_t>(game::Base() + kRvaSetResult)(info, text, std::size_t(-1));
			auto* flags = static_cast<std::uint8_t*>(info) + 0x18;
			if (*flags & 4) *flags |= 2;
		}

		bool PatchTranslator()
		{
			auto** slot = reinterpret_cast<void**>(game::Base() + kRvaTranslatorVtbl) + 2;
			if (reinterpret_cast<uptr>(*slot) != game::Base() + kRvaTranslate) {
				logger::Write("Controls page: the translator's slot is not the game's (%p); not patched", *slot);
				return false;
			}
			DWORD old = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
			g_translate = reinterpret_cast<Translate_t>(*slot);
			*slot = reinterpret_cast<void*>(&TranslateDetour);
			VirtualProtect(slot, sizeof(void*), old, &old);
			return true;
		}
	}

	void Install()
	{
		if (g_installed.load()) return;
		void* orig = nullptr;
		const bool validate = hook::Install(game::Base() + kRvaValidate, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&ValidateDetour), &orig);
		if (validate) g_validate = reinterpret_cast<Map_t>(orig);
		const bool apply = validate && hook::Install(game::Base() + kRvaApply, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&ApplyDetour), &orig);
		if (apply) g_apply = reinterpret_cast<Map_t>(orig);
		const bool translate = apply && PatchTranslator();
		g_installed.store(validate && apply && translate);
		logger::Write("Controls page: remap validate %s, apply %s, translator %s -> vehicle controls %s the game's Controls page", validate ? "hooked" : "NOT hooked",
			apply ? "hooked" : "NOT hooked", translate ? "hooked" : "NOT hooked", g_installed.load() ? "on" : "NOT on");
	}

	bool Installed() { return g_installed.load(); }
}

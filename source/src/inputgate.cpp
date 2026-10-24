#include "inputgate.h"

#include "bindlogic.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "inputownership.h"

#include <Windows.h>

#include <atomic>

namespace inputgate
{
	namespace
	{
		// Starfield.exe 1.16.244 (7e9adb14...): the queue add's call of the resolver (bytes E8 -> 0x1422F1840).
		constexpr std::uint32_t kResolveCall = 0x022DAC60;
		constexpr std::uint32_t kResolve = 0x022F1840;    // (ControlMap*, InputEvent*): fills the event's user event
		// InputEvent: +0x08 u8 device (2 gamepad), +0x10 u8 type (0 button), +0x30 u32 code (XInput mask, 9 / 10 the
		// triggers), +0x34 u8 block reason (0 = delivered; 4 = the enable manager's, the one the game itself uses).
		constexpr std::uint8_t kGamepad = 2, kButton = 0, kBlockedReason = 4;

		using Resolve_t = void (*)(std::uintptr_t, std::uintptr_t);
		Resolve_t g_resolve = nullptr;
		std::atomic<std::uint32_t> g_blocked{ 0 };
		std::atomic<unsigned> g_count{ 0 };
        std::array<std::atomic<bool>,256> g_keys{};
        std::array<std::array<std::uint8_t,256>,3> g_owner{}; // input queue is serialized by the game's queue lock

		bool IsBlocked(std::uint32_t code, std::uint32_t bits)
		{
			if (code == 9) return (bits & bindlogic::kLT) != 0;
			if (code == 10) return (bits & bindlogic::kRT) != 0;
			return code && code <= 0xFFFFu && (code & (code - 1)) == 0 && (bits & code) != 0;
		}

		void Gate(std::uintptr_t event)
		{
			const std::uint32_t bits = g_blocked.load(std::memory_order_relaxed);
			__try {
				auto* reason = reinterpret_cast<std::uint8_t*>(event + 0x34);
                if(*reinterpret_cast<std::uint8_t*>(event+0x10)!=kButton)return;
                const auto device=*reinterpret_cast<std::uint8_t*>(event+8);
                const auto code=*reinterpret_cast<std::uint32_t*>(event+0x30);
                if(device>2)return;
                std::uint32_t index=code;
                bool requested=false;
                if(device==kGamepad){
                    const auto bit=bindlogic::GamePadBit(code);if(!bit)return;
                    index=0;auto scan=bit;while(scan>>=1)++index;
                    requested=IsBlocked(code,bits);
                }else if(device==0){if(code>=256)return;requested=g_keys[code].load();}
                else{constexpr int mouseKeys[]={1,2,4,5,6};if(code>=5)return;requested=g_keys[mouseKeys[code]].load();}
                const float value=*reinterpret_cast<float*>(event+0x48);
                const float seconds=*reinterpret_cast<float*>(event+0x4C);
                requested=requested&&GameplayAllowed();
                const bool owned=inputownership::Own(g_owner[device][index],value>0,seconds==0,requested);
                if(!owned||*reason)return; // never replace a native block reason
				*reason = kBlockedReason;
				g_count.fetch_add(1, std::memory_order_relaxed);
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		void Resolve(std::uintptr_t controlMap, std::uintptr_t event)
		{
			g_resolve(controlMap, event);
			if (event) Gate(event);
		}
	}

	bool Install()
	{
		g_resolve = reinterpret_cast<Resolve_t>(game::Base() + kResolve);
		const bool ok = hook::RedirectCall(game::Base() + kResolveCall, reinterpret_cast<void*>(g_resolve), reinterpret_cast<void*>(&Resolve));
		logger::Write("Input gate: %s", ok ? "on (the game's action is switched off on the mod's controller buttons)"
			: "NOT installed (the resolver call differs); the game still acts on the mod's controller buttons");
		return ok;
	}

	void SetBlocked(std::uint32_t padBits) { g_blocked.store(padBits, std::memory_order_relaxed); }
    void SetBlockedKeys(const std::array<bool,256>& keys){
        for(std::size_t i=0;i<keys.size();++i)g_keys[i].store(keys[i]);
        if(keys[VK_CONTROL]){g_keys[VK_LCONTROL]=true;g_keys[VK_RCONTROL]=true;}
        if(keys[VK_SHIFT]){g_keys[VK_LSHIFT]=true;g_keys[VK_RSHIFT]=true;}
        if(keys[VK_MENU]){g_keys[VK_LMENU]=true;g_keys[VK_RMENU]=true;}
    }
    bool GameplayAllowed()
    {
        DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(pid!=GetCurrentProcessId())return false;
        __try{
            const auto cm=*reinterpret_cast<std::uintptr_t*>(game::Base()+0x061DE668);
            if(!cm)return false;
            const auto count=*reinterpret_cast<std::uint32_t*>(cm+0x2A8);
            const auto stack=*reinterpret_cast<std::uintptr_t*>(cm+0x2B0);
            if(count>128||(count&&!stack))return false;
            for(std::uint32_t i=0;i<count;++i){const auto id=*reinterpret_cast<std::uint8_t*>(stack+i*16);if(id==0x4E||id==0x11)return false;}
            return true;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
	unsigned TakeBlockedCount() { return g_count.exchange(0, std::memory_order_relaxed); }
}

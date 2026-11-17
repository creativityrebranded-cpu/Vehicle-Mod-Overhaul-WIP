#include "inputgate.h"

#include "bindlogic.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "inputownership.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>

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
        std::atomic<std::uint64_t> g_devicePress{0};
        std::atomic<ULONGLONG> g_mapEditUntil{0}, g_mapAllUntil{0}, g_wheelUntil{0};
        std::atomic<int> g_wheel{0};
        // The map's camera keys and the ways out stay with the map while the route editor holds it.
        bool MapKeyAllowed(std::uint32_t vk)
        {
            switch(vk){
            case VK_TAB: case VK_ESCAPE: case 'W': case 'A': case 'S': case 'D':
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: return true;
            default: return false;
            }
        }

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
                const auto device=*reinterpret_cast<std::uint8_t*>(event+8);
                const auto code=*reinterpret_cast<std::uint32_t*>(event+0x30);
                const auto kind=*reinterpret_cast<std::uint8_t*>(event+0x10);
                if(device==1&&code!=0&&GetTickCount64()<g_mapEditUntil.load(std::memory_order_relaxed)){
                    // What the wheel really sends is only known from a live run (2026-10-30, scroll height): the first few events are logged.
                    static std::atomic<int> told{0};
                    if(told.fetch_add(1,std::memory_order_relaxed)<16)logger::WriteUnlocked("Input gate: mouse event while the route editor is open: type %u, code %u, value %.2f, held %.2f",
                        unsigned(kind),code,*reinterpret_cast<float*>(event+0x48),*reinterpret_cast<float*>(event+0x4C));
                }
                if(kind!=kButton)return;
                if(device>2)return;
                if(device==1&&(code==8||code==9)&&GetTickCount64()<g_wheelUntil.load(std::memory_order_relaxed)){
                    if(*reinterpret_cast<float*>(event+0x4C)==0)g_wheel.fetch_add(code==8?1:-1,std::memory_order_relaxed);
                    if(!*reason){*reason=kBlockedReason;g_count.fetch_add(1,std::memory_order_relaxed);}
                    return;
                }
                if(GetTickCount64()<g_mapEditUntil.load(std::memory_order_relaxed)){
                    // keyboard (0): all but the camera keys and the exits; mouse (1): the left button only
                    const bool all=GetTickCount64()<g_mapAllUntil.load(std::memory_order_relaxed);
                    // The pad belongs to the editor too (2026-10-30, Traffic Manager V1): D-pad, A B X Y, LB RB, Start, View and the triggers. The sticks and the
                    // right stick's click stay with the map, so the map still turns and the editor still closes.
                    const bool dpad=device==kGamepad&&(code==1||code==2||code==4||code==8||code==0x10||code==0x20||code==0x100||code==0x200||code==0x1000||code==0x2000||code==0x4000||code==0x8000||code==9||code==10);
                    const bool hold=(device==0&&(all||!MapKeyAllowed(code)))||(device==1&&code==0)||dpad;
                    if(hold){if(!*reason){*reason=kBlockedReason;g_count.fetch_add(1,std::memory_order_relaxed);}return;}
                }
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
                if(value>0&&seconds==0){
                    DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
                    if(pid==GetCurrentProcessId()){
                        const auto previous=g_devicePress.load(std::memory_order_relaxed);
                        g_devicePress.store(((previous&~1ull)+2)|(device==kGamepad?1ull:0ull),std::memory_order_release);
                    }
                }
                requested=requested&&ModKeysAllowed();
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
    std::uint64_t LastDevicePress(){return g_devicePress.load(std::memory_order_acquire);}
    namespace
    {
        // The control map's input context stack (+0x2A8 count, +0x2B0 entries of 16 bytes, id in the first byte).
        // Returns the count, or -1 when it cannot be read; the game window must have the focus.
        int ContextStack(std::uint8_t (&ids)[128])
        {
            DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
            if(pid!=GetCurrentProcessId())return -1;
            __try{
                const auto cm=*reinterpret_cast<std::uintptr_t*>(game::Base()+0x061DE668);
                if(!cm)return -1;
                const auto count=*reinterpret_cast<std::uint32_t*>(cm+0x2A8);
                const auto stack=*reinterpret_cast<std::uintptr_t*>(cm+0x2B0);
                if(count>128||(count&&!stack))return -1;
                for(std::uint32_t i=0;i<count;++i)ids[i]=*reinterpret_cast<std::uint8_t*>(stack+i*16);
                return int(count);
            }__except(EXCEPTION_EXECUTE_HANDLER){return -1;}
        }
    }
    bool GameplayAllowed()
    {
        std::uint8_t ids[128];const int n=ContextStack(ids);
        return n>=0&&inputownership::Gameplay(ids,std::size_t(n));
    }
    bool ModKeysAllowed()
    {
        std::uint8_t ids[128];const int n=ContextStack(ids);
        const bool on=n>=0&&inputownership::ModKeys(ids,std::size_t(n));
        // Trace (diagnostic builds): the stack each time the answer changes, so a live run shows which contexts a menu uses.
        // Also each new readable stack (first 400), so a stack that keeps the keys from the mod is named in the log.
        static std::atomic<int> last{-1};static std::atomic<std::uint64_t> lastStack{0};static std::atomic<int> stackLines{0};
        std::uint64_t h=1469598103934665603ull;for(int i=0;i<n;++i)h=(h^ids[i])*1099511628211ull;h^=std::uint64_t(n<0?0xFFFF:n)<<48;
        const bool changed=last.exchange(on?1:0,std::memory_order_relaxed)!=(on?1:0);
        const bool newStack=n>=0&&lastStack.exchange(h,std::memory_order_relaxed)!=h&&stackLines.fetch_add(1,std::memory_order_relaxed)<400;
        if(changed||newStack){
            char text[128*3+1]{};int at=0;
            for(int i=0;i<n&&at<int(sizeof(text))-4;++i)at+=std::snprintf(text+at,sizeof(text)-at,"%s%02X",i?" ":"",ids[i]);
            logger::WriteUnlocked("Input gate: mod keys %s (contexts: %s)",on?"on":"given back to the game",n<0?"unreadable or unfocused":text);
        }
        return on;
    }
	unsigned TakeBlockedCount() { return g_count.exchange(0, std::memory_order_relaxed); }
	void HoldMapForEditor() { g_mapEditUntil.store(GetTickCount64() + 300, std::memory_order_relaxed); }
	void HoldMapAll() { const auto t = GetTickCount64() + 300; g_mapEditUntil.store(t, std::memory_order_relaxed); g_mapAllUntil.store(t, std::memory_order_relaxed); }
	void WheelForEditor(bool on) { g_wheelUntil.store(on ? GetTickCount64() + 300 : 0, std::memory_order_relaxed); if (!on) g_wheel.store(0, std::memory_order_relaxed); }
	int TakeWheel() { return g_wheel.exchange(0, std::memory_order_relaxed); }
}

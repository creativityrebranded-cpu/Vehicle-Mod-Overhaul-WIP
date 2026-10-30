#include "secretunpause.h"
#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "npcvehicles.h"
#include "routeedit.h"

#include <Windows.h>
#include <cstdint>
#include <vector>

// Read in Starfield 1.16.244 (2026-10-29, notes/start-swap-unpause-20261029/RESULT.md):
//  - UI singleton pointer at 0x145FD9B80 (the UI constructor 14253E170 stores it, its destructor clears it).
//  - UI +0x4B4: the menu pause count. FUN_142544A40 (UI, const BSFixedString* name, bool pausing) adds or removes one
//    and sends MenuPauseCounterChangeEvent (UI +0x98) and, on 0 <-> 1, MenuPauseChangeEvent (UI +0x70).
//  - FUN_142545B60 = SetMenuPausesGame(UI, const BSFixedString* menuName, bool pauses): finds the menu by name, does
//    nothing if its flag already matches, else sets/clears IMenu flag 0x2 (+0xC0) and, if the menu is open, changes the
//    count through FUN_142544A40. The menu-close path only removes a pause while flag 0x2 is set, so lifting and giving
//    back through this function can never leave the count wrong, even when the map closes in between.
//  - UI +0x430 count / +0x438 data: the open menu list (IMenu* each). IMenu +0xB0 = its name (BSFixedString),
//    +0xC0 = flags (0x2 pauses the game, 0x40 open).
namespace secretunpause
{
    namespace
    {
        using uptr = std::uintptr_t;
        constexpr std::uint32_t kUiSingleton = 0x05FD9B80, kSetMenuPausesGame = 0x02545B60;
        constexpr std::uint32_t kPauseCount = 0x4B4, kMenuCount = 0x430, kMenuData = 0x438, kMenuName = 0xB0, kMenuFlags = 0xC0;
        constexpr std::uint32_t kPausesGame = 0x2, kOpen = 0x40;
        constexpr ULONGLONG kMaxMs = 20000;  // hard cap: the pause always comes back
        using SetPauses_t = void (*)(uptr ui, const uptr* name, bool pauses);

        bool g_active = false;
        bool g_promptAfterCombat = false;  // Start/Swap was refused (or cut short) by combat: prompt once it is over
        ULONGLONG g_calmSince = 0;
        ULONGLONG g_since = 0;
        std::vector<uptr> g_names;  // BSFixedString values (pooled entries) of the menus whose pause was lifted

        uptr Ui()
        {
            __try { return *reinterpret_cast<uptr*>(game::Base() + kUiSingleton); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
        int PauseCount(uptr ui)
        {
            __try { return ui ? *reinterpret_cast<int*>(ui + kPauseCount) : -1; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        }
        // Names of the open menus that hold a pause right now.
        bool PausingMenus(uptr ui, std::vector<uptr>& out)
        {
            out.clear();
            __try {
                const std::uint32_t n = *reinterpret_cast<std::uint32_t*>(ui + kMenuCount);
                const uptr data = *reinterpret_cast<uptr*>(ui + kMenuData);
                if (n > 64 || (n && !data)) return false;
                for (std::uint32_t i = 0; i < n; ++i) {
                    const uptr menu = *reinterpret_cast<uptr*>(data + 8ull * i);
                    if (!menu) continue;
                    const std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(menu + kMenuFlags);
                    const uptr name = *reinterpret_cast<uptr*>(menu + kMenuName);
                    if ((flags & kPausesGame) && (flags & kOpen) && name) out.push_back(name);
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { out.clear(); return false; }
        }
        bool SetPauses(uptr ui, uptr name, bool pauses)
        {
            __try { reinterpret_cast<SetPauses_t>(game::Base() + kSetMenuPausesGame)(ui, &name, pauses); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        // FUN_142545B60's first instructions in 1.16.244; any other bytes (another game version) and nothing is called.
        bool FunctionMatches()
        {
            static const std::uint8_t kPrologue[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x10, 0x48,0x89,0x68,0x18, 0x56, 0x57, 0x41,0x56 };
            __try {
                const auto* at = reinterpret_cast<const std::uint8_t*>(game::Base() + kSetMenuPausesGame);
                for (std::size_t i = 0; i < sizeof kPrologue; ++i) if (at[i] != kPrologue[i]) return false;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    void RefusedForCombat() { g_promptAfterCombat = true; g_calmSince = 0; }

    bool PlayerInCombat()
    {
        uptr player = 0;
        __try { player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }  // unknown: refuse
        return !player || npcvehicles::IsInCombat(player);
    }

    Result Begin()
    {
        if (g_active) return Result::Started;
        if (PlayerInCombat()) {
            notify::Show("In combat: traffic can't be updated. Exit combat first.");
            logger::Write("Secret unpause: refused, the player is in combat");
            RefusedForCombat();
            return Result::InCombat;
        }
        if (!FunctionMatches()) { logger::Write("Secret unpause: the game's SetMenuPausesGame bytes differ (other game version); the map stays paused"); return Result::Unavailable; }
        const uptr ui = Ui();
        std::vector<uptr> menus;
        if (!ui || !PausingMenus(ui, menus)) { logger::Write("Secret unpause: UI not readable; the map stays paused"); return Result::Unavailable; }
        const int before = PauseCount(ui);
        if (menus.empty() || before <= 0) { logger::Write("Secret unpause: nothing holds a pause (count %d)", before); return Result::NotPaused; }
        g_names.clear();
        for (const uptr name : menus) if (SetPauses(ui, name, false)) g_names.push_back(name);
        g_active = !g_names.empty();
        g_since = GetTickCount64();
        const int after = PauseCount(ui);
        logger::Write("Secret unpause: lifted the pause of %zu open menu(s) under the Start/Swap panel; pause count %d -> %d%s",
            g_names.size(), before, after, after > 0 ? " (another pause remains: the world may still be held)" : "");
        return g_active ? Result::Started : Result::Unavailable;
    }

    void End(const char* why)
    {
        if (!g_active) return;
        g_active = false;
        const uptr ui = Ui();
        const int before = PauseCount(ui);
        if (ui) for (const uptr name : g_names) SetPauses(ui, name, true);
        logger::Write("Secret unpause: pause given back (%s) after %.1f s; pause count %d -> %d", why ? why : "done",
            (GetTickCount64() - g_since) / 1000.0, before, PauseCount(ui));
        g_names.clear();
    }

    bool Active() { return g_active; }
    void Reset() { End("world reset"); g_promptAfterCombat = false; g_calmSince = 0; }

    void Tick()
    {
        // After combat refused or interrupted Start/Swap: once the player has been out of combat for 3 s, say it can be done now.
        if (g_promptAfterCombat && !g_active) {
            if (PlayerInCombat()) g_calmSince = 0;
            else if (!g_calmSince) g_calmSince = GetTickCount64();
            else if (GetTickCount64() - g_calmSince > 3000) {
                g_promptAfterCombat = false;
                notify::Show("Combat over: open the map and press Start/Swap to update traffic.");
                logger::Write("Secret unpause: combat over; prompted the player to update traffic");
            }
        }
        if (!g_active) return;
        if (GetTickCount64() - g_since > kMaxMs) { End("time cap"); return; }
        if (!routeedit::MapOpen(1000)) { End("the map closed"); return; }
        if (PlayerInCombat()) {
            End("combat started");
            notify::Show("Combat started: traffic update paused. Exit combat first.");
            RefusedForCombat();
        }
    }
}

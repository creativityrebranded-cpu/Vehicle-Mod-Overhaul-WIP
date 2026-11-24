#include "addresses.h"
#include "lifecycle.h"
#include "spottest.h"
#include "features.h"
#include "framepacing.h"
#include "updateschedule.h"
#include "resources.h"
#include "context.h"
#include "core_api.h"
#include "crash.h"
#include "flighttune.h"
#include "game.h"
#include "hoverui.h"
#include "routeedit.h"
#include "trafficprofile.h"
#include "flightsave.h"
#include "notify.h"
#include "mapguard.h"
#include "cellbake.h"
#include "routesurvey_run.h"
#include "hazardfield.h"
#include "disableguard.h"
#include "realvehicles.h"
#include "runtimecells.h"
#include "ringroad.h"
#include "trafficpool.h"
#include "trafficarrival.h"
#include "routeprobes.h"
#include "log.h"
#include "modscene.h"
#include "navmap.h"
#include "navfield.h"
#include "environment.h"
#include "npcvehicles.h"
#include "pilotquest.h"
#include "simtier.h"
#include "passenger.h"
#include "traffic.h"
#include "profiles.h"
#include "scantarget.h"
#include "bindings.h"
#include "inputgate.h"
#include "loadedarea.h"
#include "vehiclediag.h"
#include "native_movement_probe.h"
#include "mappoints.h"
#include "secretunpause.h"
#include "vehicleworld.h"
#include "vehiclehits.h"
#include "navseed.h"
#include "navprobe.h"
#include "navmesh.h"
#include "vehicleaim.h"
#include "plugininfo.h"
#include "shipmodel.h"
#include "shipvehicle.h"
#include "telemetry.h"
#include "vehicle_probe.h"
#include "vehicleflight.h"
#include "viewdist.h"
#include "vehiclekeep.h"

#include "sfse/PluginAPI.h"

#include "hook.h"
#include <atomic>
#include <cstring>
#include <algorithm>
#include <Windows.h>
#include "costparts.h"

namespace plugininfo
{
	const wchar_t* Name() { return L"FlyingVehicleOverhaul"; }  // the log: FlyingVehicleOverhaul.log
	const wchar_t* TelemetryPrefix() { return L"vehicle-"; }
}

namespace seamless { void SetApi(const CoreApi* api); }

namespace
{
	PluginHandle g_handle = static_cast<PluginHandle>(kPluginHandle_Invalid);
	SFSEMessagingInterface* g_messaging = nullptr;
	SFSETaskInterface* g_tasks = nullptr;
	std::atomic_bool g_started = false;
	bool g_standalone = false;
    bool g_worldReady = false;
    // 2026-10-28 crashes (01:16, 01:44): Starfield.exe+0x1817391 is inside the game's load-game routine (FUN_141815620), at
    // "Form '%s' (%08X) was saved with form type %s, but currently has form type %s": the save loads on a background thread
    // while cells already attach, and FVO had started traffic and filled its pilot-quest aliases (quest + package changes)
    // 3-4 s before each fault, mid-load. Nothing that changes the world runs from PreLoadGame until PostLoadGame + a settle.
    std::atomic<ULONGLONG> g_loadBegan{0}, g_loadDone{0};
    // 2026-11-15 hotfix (Nexus: crash loading a save from a running game; first load fine). As in the rewrite's load rule:
    // the moment a load starts, the next frame drops everything FVO holds from the old world (Reset), instead of waiting
    // until the planet manager is seen at state 0/1 (about a second later, with hooks still holding old pointers).
    std::atomic<bool> g_dropAll{false};
    constexpr ULONGLONG kLoadSettleMs = 3000, kLoadTimeoutMs = 180000;
    // SFSE never sent PreLoadGame / PostLoadGame here (01:53 run: no "Save load" lines), so the game's own load routine
    // FUN_141815620 ("***Loading game: %s (%s)") is wrapped: it runs the whole load on a background thread.
    constexpr std::uint32_t kLoadGame = 0x01815620;
    using LoadGame_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint64_t, std::uint64_t);
    LoadGame_t g_loadGameOriginal = nullptr;
    std::uintptr_t LoadGameDetour(std::uintptr_t a, std::uintptr_t b, std::uint64_t c, std::uint64_t d)
    {
        g_loadBegan = GetTickCount64();
        // Before anything else: work queued against the old world never runs against the one being read. The frame's reset comes later, or not at all.
        lifecycle::Close();
        trafficprofile::BeginLoad();
        trafficarrival::Reset();
        flightsave::OnLoad(reinterpret_cast<const char*>(b));  // FUN_141815620 param_2: the save's name
        vehiclediag::NoteBeforeLoad();  // no-chassis trace: which vehicles stay in memory through the load
        g_dropAll = true;
        crash::Crumb("save load: routine started");
        logger::Important("Save load: the game's load routine started; FVO world actions paused and held objects dropped");
        const auto r = g_loadGameOriginal(a, b, c, d);
        lifecycle::Open();
        g_loadDone = GetTickCount64();
        crash::Crumb("save load: routine returned");
        logger::Important("Save load: the game's load routine returned after %.1f s", (g_loadDone.load() - g_loadBegan.load()) / 1000.0);
        return r;
    }
    void InstallLoadHook()
    {
        static const std::uint8_t kPrologue[15] = { 0x48,0x8B,0xC4, 0x44,0x88,0x48,0x20, 0x44,0x88,0x40,0x18, 0x48,0x89,0x50,0x10 };
        void* original = nullptr;
        if (hook::Install(game::Base() + kLoadGame, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&LoadGameDetour), &original)) {
            g_loadGameOriginal = reinterpret_cast<LoadGame_t>(original);
            logger::Write("Save load: load-game hook installed (141815620)");
        }
        else logger::Write("Save load: load-game hook NOT installed (bytes differ); FVO does not pause during loads");
    }
    using SaveGame_t = void (*)(std::int64_t*, std::uint64_t, std::int64_t, std::uint64_t);
    SaveGame_t g_saveGameOriginal = nullptr;
    void BeforeTrafficSave() { if (trafficpool::AddonAttached()) trafficprofile::BeforeSave(); }
    void AfterTrafficSave() { trafficprofile::AfterSave(); }
    void SaveGameDetour(std::int64_t* a, std::uint64_t b, std::int64_t c, std::uint64_t d)
    {
        flightsave::OnSave(reinterpret_cast<const char*>(d));  // FUN_141814220 param_4: the save's name ("Finished saving game: %s")
        BeforeTrafficSave();
        struct Finish { ~Finish() { AfterTrafficSave(); } } finish;
        g_saveGameOriginal(a, b, c, d);
    }
    // 2026-11-17 (user: the hook system belongs to the core; the Traffic Addon only manages what the core has): the core owns
    // the SaveGame hook in every standalone install, installed with the load hook. It records the core's own per-save state
    // (saved hover) and, only while a Traffic Addon is attached, takes the traffic snapshot as in 1.2.1. Replaces 1.2.1's
    // addon-only install ("Do not add a save hook to core-only installs"). Save files and settings are unchanged.
    void InstallSaveHook()
    {
        static const std::uint8_t prologue[15] = {0x4C,0x89,0x4C,0x24,0x20,0x4C,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10};
        void* original = nullptr;
        if (hook::Install(game::Base() + addr::kSaveGame, prologue, sizeof prologue, reinterpret_cast<void*>(&SaveGameDetour), &original)) {
            g_saveGameOriginal = reinterpret_cast<SaveGame_t>(original);
            trafficprofile::SaveReady(true);
            logger::Write("Save hook: native SaveGame hook installed (core per-save state; traffic snapshots while the Traffic Addon is attached)");
        } else {
            trafficprofile::SaveReady(false);
            logger::Important("Save hook: SaveGame hook unavailable (bytes differ); saved hover is off and traffic remains paused");
        }
    }
    bool Loading(ULONGLONG now)
    {
        const ULONGLONG began = g_loadBegan.load(), done = g_loadDone.load();
        if (!began) return false;
        if (done >= began) {
            if (now - done < kLoadSettleMs) return true;
            g_loadBegan = 0;
            logger::Write("Save load: finished %.1f s ago; FVO world actions resume", (now - done) / 1000.0);
            return false;
        }
        if (now - began < kLoadTimeoutMs) return true;
        g_loadBegan = 0;
        logger::Write("Save load: no PostLoadGame %.0f s after PreLoadGame; FVO world actions resume anyway", kLoadTimeoutMs / 1000.0);
        return false;
    }
    // The game's loading screen (2026-10-29, read in 1.16): BeginLoadingScreen 14057F020 is the only writer of 1 to this byte,
    // EndLoadingScreen 14057F430 the only writer of 0, just before it closes the screen (after its wait for the world).
    // Arrival traffic is placed while it is up; FVO never holds the screen open.
    constexpr std::uint32_t kLoadingScreenFlag = 0x05E60AFA;
    bool LoadingScreenUp()
    {
        __try { return *reinterpret_cast<volatile std::uint8_t*>(game::Base() + kLoadingScreenFlag) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // 2026-11-17 no-chassis research: the cell attach FUN_140B02C80 skips RuntimeComponentDBFactory::ReferenceAttach for every
    // reference of an exterior cell while TES +0xE8 (the interior cell, null outdoors) is set, and for an interior cell that is not
    // that one (cVar11 -> FUN_140B02630 param_3). Without the event no vehicle gets a component. Logged during loads to see the value.
    std::uint32_t TesInteriorCellId()
    {
        __try {
            const auto tes = *reinterpret_cast<std::uintptr_t*>(game::Base() + addr::kTESPtr);
            const auto cell = tes ? *reinterpret_cast<std::uintptr_t*>(tes + off::kTESInteriorCell) : 0;
            return cell ? *reinterpret_cast<std::uint32_t*>(cell + off::kFormId) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
    }
    // The arrival window: the loading screen is up, the save (if any) has been read and the planet manager has the world.
    bool ArrivalWindow(bool worldReady)
    {
        const ULONGLONG now = GetTickCount64(), began = g_loadBegan.load(), done = g_loadDone.load();
        const bool screen = LoadingScreenUp(), readingSave = began && done < began && now - began < kLoadTimeoutMs;
        const bool window = screen && !readingSave && worldReady;
        static bool wasScreen = false, wasWindow = false;
        static ULONGLONG screenAt = 0, windowAt = 0, windowMs = 0;
        static std::uint32_t seedAtOpen = 0;
        if (screen && !wasScreen) {
            screenAt = now; windowMs = 0; int a = 0, b = 0; trafficpool::ArrivalChecks(a, b, true);
            seedAtOpen = navseed::CompletedPasses(); trafficarrival::seeded = false;
            logger::Write("Loading screen: up%s", readingSave ? " (a save is being read)" : "");
        }
        if (window && !wasWindow) {
            windowAt = now;
            logger::Write("Loading screen: world ready behind it %.1f s after it opened; arrival traffic is placed until it closes", (now - screenAt) / 1000.0);
        }
        if (!window && wasWindow) windowMs += now - windowAt;
        if (!screen && wasScreen) {
            trafficarrival::closedAt.store(now);  // T-1: the one cover owner records the close (the spot test reads it)
            int rays = 0, mapOnly = 0; trafficpool::ArrivalChecks(rays, mapOnly, false);
            logger::Write("Loading screen: closed after %.1f s (arrival window %.1f s); arrival traffic %d of %d route positions placed or held (%d ray-checked, %d nav-map only)",
                (now - screenAt) / 1000.0, windowMs / 1000.0, trafficarrival::ready.load(), trafficarrival::expected.load(), rays, mapOnly);
            // CC-1 (2026-11-07, user: "do within mandate"): the one cover owner reports the close frame to the traffic addon while the
            // cover still holds (Window is switched off below, LoadingMenu is still listed). Health judges with that fact and Retirement
            // removes under its own rule. Optional export: an older addon without it is left as it was.
            {   using CoverClosing = int (*)(int);
                const auto addon = trafficpool::AddonAttached() ? GetModuleHandleW(L"FVO_Traffic.dll") : nullptr;
                const auto told = addon ? reinterpret_cast<CoverClosing>(GetProcAddress(addon, "FVO_Traffic_CoverClosingV1")) : nullptr;
                const int result = told ? told(3) : -2;
                logger::Write("OBS system=cover.closing event=told kind=3 addon_export=%d result=%d covered=%d", told ? 1 : 0, result, int(trafficarrival::Covered()));
            }
        }
        if (window && !trafficarrival::seeded && navseed::CompletedPasses() > seedAtOpen) {
            trafficarrival::seeded = true;
            logger::Write("Loading screen: nav map seeded behind it %.1f s after it opened; arrival traffic placement begins", (now - screenAt) / 1000.0);
        }
        {   // CM (2026-11-07, user: "measure first"): read-only. Does anything step behind the loading screen, and what holds the pause?
            // Logged 4 times a second while the screen is up and for 3 s after it closes, and at once on any change of pause or menus.
            static ULONGLONG measureAt = 0, closedAt = 0; static std::uint64_t stepsWas = 0; static int pauseWas = -2; static char menusWas[512] = {};
            if (!screen && wasScreen) closedAt = now;
            const bool watch = screen || (closedAt && now - closedAt <= 3000);
            if (watch) {
                char menus[512]; const int pause = secretunpause::Describe(menus, int(sizeof menus));
                int bound = 0; const std::uint64_t steps = vehicleflight::TotalPhysicsSteps(&bound);
                const bool changed = pause != pauseWas || std::strcmp(menus, menusWas) != 0 || screen != wasScreen;
                if (changed || now - measureAt >= 250) {
                    measureAt = now;
                    logger::Write("OBS system=cover.measure screen=%d window=%d reading_save=%d world_ready=%d pause=%d %s vehicle_steps=%llu new_steps=%llu bound_pilots=%d since_open_ms=%lld since_close_ms=%lld change=%d",
                        int(screen), int(window), int(readingSave), int(worldReady), pause, menus, (unsigned long long)steps, (unsigned long long)(steps - stepsWas), bound,
                        screenAt ? (long long)(now - screenAt) : -1LL, !screen && closedAt ? (long long)(now - closedAt) : -1LL, int(changed));
                    stepsWas = steps;
                }
                pauseWas = pause; std::strncpy(menusWas, menus, sizeof menusWas - 1);
                trafficarrival::fade.store(std::strstr(menus, "FaderMenu") != nullptr);  // T-1: the fade-in, kept by the one cover owner
            } else trafficarrival::fade.store(false);
        }
        wasScreen = screen; wasWindow = window;
        trafficarrival::Window(window);
        crash::SetGameplay(worldReady && !screen && !readingSave);
        return window;
    }
    std::atomic_flag g_inCallback=ATOMIC_FLAG_INIT;
    framepacing::Window g_pacing;
    updateschedule::Gate g_schedule;
    updateschedule::Pulse g_coreUpdates;
    double g_scheduleBegan=0,g_scheduleWork=0;
    void ResetCostWindow();

	void Reset()
	{
        crash::Crumb("reset: begin");
        logger::Write("Load trace: reset begins (thread %lu, world_ready %d)", GetCurrentThreadId(), int(g_worldReady));
        trafficprofile::Allow(false);
        trafficpool::WorldActionsAllowed(false);
        lifecycle::Reset();  // first: a queued callback must not refill what the resets below clear
        secretunpause::Reset();
        g_worldReady=false;g_pacing={};g_schedule.Reset();g_scheduleBegan=g_scheduleWork=0;ResetCostWindow();
        framebudget::Main().ResetAll();
        runtimecells::Reset();
		bindings::ResetRuntime();
		vehicleflight::LoadReset();
		shipvehicle::Reset();
		shipmodel::Reset();
		vehicle_probe::Reset();
		realvehicles::Reset();
		vehiclediag::Reset();
		ringroad::Reset();
		routeprobes::Reset();
		trafficpool::Reset();
        pilotquest::Reset();
		npcvehicles::Reset();
		passenger::Reset();
		scantarget::Reset();
		traffic::Reset();
		context::Reset();
		viewdist::Reset();
		vehiclekeep::Reset();
		profiles::Reset();
		world::Reset();
		vehiclehits::Reset();
		navseed::Reset();
		cellbake::Reset();
		navmap::Reset();
		navfield::Reset();
        environment::Reset();
        crash::Crumb("reset: done");
        logger::Write("Load trace: reset done");
	}

    struct FrameCost {
        double sum[8]{},max[8]{};unsigned count=0;ULONGLONG began=0;
        // PF-1 (2026-11-21): updates over 8 / 12 / 20 ms, the flight and traffic pool parts, and the energy plan's gathering per update.
        unsigned over8=0,over12=0,over20=0;double partSum[costparts::kParts]{},partMax[costparts::kParts]{},gatherSum=0,gatherMax=0,poolSum=0,poolMax=0;
        void Finish(const double sample[8]) {
            const auto now=GetTickCount64();if(!began)began=now;++count;
            for(int i=0;i<8;++i){sum[i]+=sample[i];max[i]=std::max(max[i],sample[i]);}
            if(sample[7]>8.0)++over8;if(sample[7]>12.0)++over12;if(sample[7]>20.0)++over20;
            double pool=0;for(int p=0;p<costparts::kParts;++p){partSum[p]+=costparts::g_frame[p];partMax[p]=std::max(partMax[p],costparts::g_frame[p]);if(p>=costparts::PoolLook)pool+=costparts::g_frame[p];}
            poolSum+=pool;poolMax=std::max(poolMax,pool);
            const double gather=costparts::g_gatherMs.exchange(0.0);gatherSum+=gather;gatherMax=std::max(gatherMax,gather);
            if(now-began<60000)return;
            {   using namespace costparts;
                const double c=double(count),secs=double(now-began)/1000.0,surf=std::max(1e-9,g_surfaceMs.load());
                auto avg=[&](int p){return partSum[p]/c;};
                logger::Important("Performance: %u updates in %.0f s (%.1f a second); mod %.2f ms avg / %.1f worst; over 8 ms %u, over 12 ms %u, over 20 ms %u; "
                    "flight %.2f / %.1f [pilots %.2f / %.1f, of which world gathering %.2f / %.1f (%u gathers, %u reused road answers; surface %.0f%% of it: road answers %.0f%%, field boxes %.0f%%, floor map %.0f%%); "
                    "fleet rails %.2f / %.1f, snapshot %.2f, environment %.2f, player %.2f, body bind %.2f]; "
                    "traffic pool %.2f / %.1f [add-on tick %.2f / %.1f, visibility %.2f / %.1f, traffic look %.2f, driver watch %.2f]; "
                    "NPC decisions %.2f / %.1f; map+navigation %.2f / %.1f; identity+world %.2f; "
                    "planning passes 20 Hz %u, 10 Hz %u (near but out of view %u), 5 Hz or slower %u (far out of view %u)",
                    count,secs,c/std::max(1.0,secs),sum[7]/c,max[7],over8,over12,over20,
                    sum[5]/c,max[5],avg(FlightPilots),partMax[FlightPilots],gatherSum/c,gatherMax,g_gathers.load(),g_surfaceReused.load(),
                    100.0*surf/std::max(1e-9,gatherSum),100.0*g_surfProvMs.load()/surf,100.0*g_surfBoxMs.load()/surf,100.0*g_surfReachMs.load()/surf,
                    avg(FlightRails),partMax[FlightRails],avg(FlightSnapshot),avg(FlightEnv),avg(FlightPlayer),avg(FlightBind),
                    poolSum/c,poolMax,avg(PoolAddon),partMax[PoolAddon],avg(PoolVisibility),partMax[PoolVisibility],avg(PoolLook),avg(PoolWatch),
                    sum[4]/c,max[4],sum[2]/c,max[2],sum[0]/c,
                    g_pass[0],g_pass[1],g_passNearOut,g_pass[2],g_passFarOut);
                ResetMinute();
            }
            logger::Write("Vehicle update cost: total %.3f ms avg / %.3f worst; identity+world %.3f / %.1f, hits %.3f / %.1f, map+navigation %.3f / %.1f, context+profiles %.3f / %.1f, NPC decisions+squads+rams %.3f / %.1f, flight %.3f / %.1f, other (vehicle registry, placement, scanner, traffic) %.3f / %.1f (avg / worst ms)",
                sum[7]/count,max[7],sum[0]/count,max[0],sum[1]/count,max[1],sum[2]/count,max[2],sum[3]/count,max[3],sum[4]/count,max[4],sum[5]/count,max[5],sum[6]/count,max[6]);
            *this={};began=now;
        }
    } g_frameCost;
    void ResetCostWindow(){g_frameCost={};}
    void Update()
    {
        LARGE_INTEGER frequency,start,previous,stamp;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);previous=start;
        auto elapsedNow=[&](){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return 1000.0*double(t.QuadPart-start.QuadPart)/frequency.QuadPart;};
        double cost[8]{};auto mark=[&](int i){QueryPerformanceCounter(&stamp);cost[i]=1000.0*double(stamp.QuadPart-previous.QuadPart)/frequency.QuadPart;previous=stamp;};
        framebudget::Main().BeginFrame();
        costparts::BeginFrame();
        modscene::NewFrame();
        features::Tick();bindings::Tick();
        vehicleflight::BeginFrame(); // decision owners must renew their commands in this frame
        npcvehicles::BeginFrame();
        world::Publish();mark(0);
        vehiclehits::Tick();mark(1);
        // Per-part timing (the stutter pass, 2026-10-27): a spike line names the slowest part of "map+navigation" and "other".
        const char* slowName[2]={"",""};double slowMs[2]={0,0};
        auto part=[&](int bucket,const char* name,auto&& fn){LARGE_INTEGER a{},b{};QueryPerformanceCounter(&a);fn();QueryPerformanceCounter(&b);
            const double ms=1000.0*double(b.QuadPart-a.QuadPart)/frequency.QuadPart;if(ms>slowMs[bucket]){slowMs[bucket]=ms;slowName[bucket]=name;}};
        const bool isLoading = Loading(GetTickCount64());
        auto& gov = framebudget::Main();
        if(isLoading || gov.Allow(framebudget::CellBake, elapsedNow())) part(0,"cell bake",[&]{cellbake::Tick(isLoading);});
        if(isLoading || gov.Allow(framebudget::RouteSurvey, elapsedNow())) part(0,"route survey",[&]{routesurvey::Tick(isLoading);});
        if(isLoading || gov.Allow(framebudget::HazardField, elapsedNow())) part(0,"hazard boxes",[&]{hazardfield::Tick(isLoading);});
        part(0,"navmesh input probe",[&]{navprobe::Tick(isLoading);});
        if(isLoading || gov.Allow(framebudget::NavMesh, elapsedNow())) part(0,"car navmesh",[&]{navmesh::Tick(isLoading);});
        if(isLoading || gov.Allow(framebudget::NavSeed, elapsedNow())) part(0,"collision-box scan",[]{navseed::Tick();});
        part(0,"flight field",[]{navfield::Service();});
        part(0,"surface checks",[]{environment::Tick();});part(0,"nav map",[]{navmap::Tick();});mark(2);
        profiles::Tick();context::Tick();mark(3);
        npcvehicles::Tick();passenger::Tick();mark(4);
        // (The native movement-controller probe did its job, 2026-10-27: every vehicle has a controller. Off: it took an engine lock every 5 s.)
        vehicleflight::Tick();vehiclediag::Tick();loadedarea::Tick();mark(5);
        gov.NoteGameplayMs(cost[4] + cost[5]);
        part(1,"ship vehicle",[]{shipvehicle::Tick();});part(1,"vehicle registry",[]{realvehicles::Tick();});part(1,"ring road",[]{ringroad::Tick();});part(1,"route markers",[]{routeprobes::Tick();});part(1,"traffic pool",[]{trafficpool::Tick();});
        part(1,"scanner target",[]{scantarget::Tick();});
        part(1,"traffic destinations",[]{traffic::Tick();});part(1,"view distance",[]{viewdist::Tick();});part(1,"keep in sight",[]{vehiclekeep::Tick();});
        part(1,"log writer",[]{logger::Service();});mark(6);
        bindings::EndUpdate();
        cost[7]=1000.0*double(stamp.QuadPart-start.QuadPart)/frequency.QuadPart;
        gov.EndFrame(cost[7]);
        g_frameCost.Finish(cost);
        {
            static ULONGLONG archAt = 0;
            const ULONGLONG nowArch = GetTickCount64();
            if (!archAt) archAt = nowArch;
            else if (nowArch - archAt >= 10000) {
                archAt = nowArch;
                const auto& ctx = simtier::Context();
                auto& ap = archproof::State();
                logger::Write("OBS system=arch.proof tier_near=%u tier_rail=%u tier_unloaded=%u rail_active=%u rail_blk=%u/%u/%u "
                    "ctx_hits=%llu/%llu hud_cache=%llu budget_over=%u/%u pilots_cap=%u bg_runs=%u,%u,%u,%u,%u bg_defers=%u,%u,%u,%u,%u starve=%u "
                    "life_player=%u life_dyn=%u life_refile=%u viol_kf=%u viol_fg=%u viol_exp=%u collider_r=%.0f broadphase_r=%.0f",
                    ctx.tierNear.load(), ctx.tierRail.load(), ctx.tierUnloaded.load(), ap.railActive.load(),
                    ap.railBlockedNoPath.load(), ap.railBlockedNoChassis.load(), ap.railBlockedBirthHold.load(),
                    static_cast<unsigned long long>(ctx.hits.load()), static_cast<unsigned long long>(ctx.lookups.load()),
                    static_cast<unsigned long long>(ap.hudFollowerCacheUses.load()),
                    gov.framesOverBudget, gov.frames, gov.pilotsCapped,
                    gov.runs[0], gov.runs[1], gov.runs[2], gov.runs[3], gov.runs[4],
                    gov.defers[0], gov.defers[1], gov.defers[2], gov.defers[3], gov.defers[4], gov.starveGrants,
                    ap.lifeToPlayer.load(), ap.lifeEnsureDyn.load(), ap.lifeUnseatRefile.load(),
                    ap.violKeyframedPlayer.load(), ap.violFallguard.load(), ap.violExpiredLane.load(),
                    ctx.colliderRadiusM, ctx.broadphaseRadiusM);
                logger::Write("OBS system=arch.corridor rail_clamped=%u plan_clamped=%u phys_wall_arrest=%u tyre_yaw_damped=%u",
                    ap.corridorRailClamped.load(), ap.corridorPlanClamped.load(),
                    ap.corridorPhysWallArrest.load(), ap.corridorTyreYawDamped.load());
                gov.ResetWindow();
                ap.ResetWindow();
            }
        }
        // An admitted update, not a rendered frame: name slow work at most every two seconds.
        static ULONGLONG spikeAt=0;
        if(cost[7]>12.0&&GetTickCount64()-spikeAt>=2000){spikeAt=GetTickCount64();
            logger::Write("Vehicle update spike: %.1f ms (identity+world %.1f, hits %.1f, map+navigation %.1f [slowest: %s %.1f], context+profiles %.1f, NPC decisions %.1f, flight %.1f, other %.1f [slowest: %s %.1f])",
                cost[7],cost[0],cost[1],cost[2],slowName[0],slowMs[0],cost[3],cost[4],cost[5],cost[6],slowName[1],slowMs[1]);}
    }
    void Frame()
    {
        trafficprofile::Report();
        // Engine calls may pump SFSE tasks recursively. Never re-enter our
        // mutable world/AI pipeline; this guard skips rather than waits.
        if(g_inCallback.test_and_set(std::memory_order_acquire))return;
        struct Release {~Release(){g_inCallback.clear(std::memory_order_release);}} release;
        // Paused/map frames still prove the callback is alive. Measuring only
        // full simulation updates mislabels a paused menu as a hung thread.
        crash::Beat();
        if (g_dropAll.exchange(false) && g_standalone) {
            crash::Crumb("save load: drop held objects");
            Reset();
            g_worldReady = false;
            logger::Important("Save load: FVO dropped every vehicle, pilot and pointer it held from the previous world");
        }
        static const double ticksPerMs=[] {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart)/1000.0;}();
        LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);const double now=double(start.QuadPart)/ticksPerMs;
        g_pacing.Begin(now); // all raw callbacks, including rejected duplicate polls
        const auto state = game::ReadPlanetState();
        if(g_standalone) {
			// Reset only for a real world replacement (manager state 0/1: a load or a landing). State 3/4 is the routine
			// terrain re-centre every ~250 m of travel; resetting there switched hover off (SeamlessWatch csv 2026-10-17:
			// states 3 occur with the loading screen off, loads show 0/1 with it on; 19 resets matched the shifts in the log).
			if (g_worldReady && state.haveManager && (state.managerState == 0 || state.managerState == 1)) {
				Reset();
				g_worldReady = false;
			}
			if (state.haveManager && state.managerState == 2) g_worldReady = true;
        }
        if(state.haveManager&&state.managerState==2)g_worldReady=true;
        else if(!state.haveManager||state.managerState==0||state.managerState==1)g_worldReady=false;
        const auto pulse=g_standalone?vehicle_probe::UpdateSerial():g_coreUpdates.Serial();
        auto work=g_schedule.Poll(now,pulse);
        const bool loading=Loading(GetTickCount64());
        trafficpool::WorldActionsAllowed(!loading&&g_worldReady);
        const bool arrival=ArrivalWindow(g_worldReady);
        trafficprofile::Allow(((!loading && g_worldReady) || arrival) && trafficpool::AddonAttached());
        logger::Observe("runtime",0,unsigned(loading)|(std::uint64_t(g_worldReady)<<1)|(std::uint64_t(arrival)<<2)|(std::uint64_t(work)<<8),"loading=%d world_ready=%d arrival=%d schedule=%u manager_state=%d",loading,g_worldReady,arrival,unsigned(work),state.haveManager?int(state.managerState):-1);
        if(arrival&&loading&&work!=updateschedule::Work::Skip){
            // The save settle runs no full update. Only the vehicle list (reads) and the traffic addon (arrival placement
            // behind the loading screen) run here; flight, AI and everything else wait for gameplay as before.
            world::Publish();navseed::Tick();trafficpool::Tick();  // the nav map is seeded before anything is placed
        }
        // 2026-11-15 hotfix: the full simulation runs only with a world: none at the main menu (manager state 0), while a save
        // loads, or behind the loading screen (the rewrite's rule). 1.2 ran it at the main menu after play and faulted in game
        // functions on the player and escorts (2026-11-01 run: ConditionFloat, HealthOf).
        // 2026-11-17 audit: the loading screen alone does not hold it. Door / fast-travel / landing screens (no save load)
        // keep 1.2's full update, which places arrival traffic behind them; the crash evidence was a save loading or no world.
        const bool worldSafe=!loading&&g_worldReady;
        {
            static int lastSafe=-1;
            if(lastSafe!=int(worldSafe)){
                lastSafe=int(worldSafe);crash::Crumb(worldSafe?"world: simulation on":"world: simulation held",state.haveManager?int(state.managerState):-1);
                logger::Write("World: simulation %s (loading %d, world ready %d, loading screen %d, manager state %d, TES interior cell %08X)",worldSafe?"on":"held",int(loading),int(g_worldReady),int(LoadingScreenUp()),state.haveManager?int(state.managerState):-1,TesInteriorCellId());
            }
        }
        if(!worldSafe){  // load trace: once a second while the simulation is held
            static ULONGLONG heldAt=0;const ULONGLONG t=GetTickCount64();
            if(t-heldAt>=1000){heldAt=t;
                static std::uint64_t steps0=0,updates0=0;
                const auto steps=vehicleflight::NativeStepCount(),updates=g_standalone?vehicle_probe::UpdateSerial():0;
                logger::Write("Load trace: frame held (thread %lu) loading %d world_ready %d loading_screen %d manager_state %d TES_interior %08X schedule %u load_began %llu load_done %llu; hooks since last line: vehicle steps %llu, component updates %llu",
                GetCurrentThreadId(),int(loading),int(g_worldReady),int(LoadingScreenUp()),state.haveManager?int(state.managerState):-1,TesInteriorCellId(),unsigned(work),
                static_cast<unsigned long long>(g_loadBegan.load()),static_cast<unsigned long long>(g_loadDone.load()),
                static_cast<unsigned long long>(steps-steps0),static_cast<unsigned long long>(updates-updates0));
                steps0=steps;updates0=updates;}
        }
        // 2026-11-17: from the load routine's return until the world's first live frame, re-attach the vehicles the load kept
        // in memory without a component (vehiclediag::AfterLoadTick), so they have a chassis before the world runs.
        if(!worldSafe&&g_standalone&&g_worldReady){const auto began=g_loadBegan.load(),done=g_loadDone.load();if(began&&done>=began)vehiclediag::AfterLoadTick();}
        if(work==updateschedule::Work::Full&&!worldSafe)work=updateschedule::Work::Housekeeping;
        if(work==updateschedule::Work::Full)Update();
        else if(work==updateschedule::Work::Housekeeping) {
            // UI bindings/settings and logs keep working while simulation is
            // paused. Never enumerate the scene or queue placement/AI work here.
            features::Tick();bindings::Tick(false,worldSafe);logger::Service();
            if(!loading&&g_worldReady)trafficpool::RushTick(); // never mutate a world during a save load
        }
        if(work!=updateschedule::Work::Skip) {
            mapguard::Tick(Loading(GetTickCount64()));  // the persistent cell report and the wheel dust cleanup
            if(worldSafe){  // 2026-11-15 hotfix (ported 2026-11-18): these reach the player, the camera or cast rays; never without a world
                secretunpause::Tick();  // Start/Swap's running world: back to paused on combat, a closed map or the time cap
                routeedit::PhotoKeyTick();  // F7: photo mode + Traffic Manager in one key, and back out
                routeedit::GroundTick();  // the editor's ground asks (new points sit 2 m over what is under the cursor), paused or not
                routeedit::ItemsMainTick();  // THREAD-1: F7 route items' reference scan on the main frame task (2 cells a frame), only while F7 asks
            }
            if(!g_scheduleBegan)g_scheduleBegan=now;
            if(now-g_scheduleBegan>=10000) {
                const auto counts=g_schedule.TakeCounts();const auto elapsed=now-g_scheduleBegan;
                logger::Write("Vehicle scheduler: %.1f raw polls/s, %.1f full updates/s; %llu full, %llu housekeeping, %llu skipped, %llu native updates coalesced, %llu clock fallback; %.2f ms task wall time/s (not CPU or present time)",
                    double(counts.polls)*1000/elapsed,double(counts.full)*1000/elapsed,
                    static_cast<unsigned long long>(counts.full),static_cast<unsigned long long>(counts.housekeeping),
                    static_cast<unsigned long long>(counts.skipped),static_cast<unsigned long long>(counts.coalesced),static_cast<unsigned long long>(counts.fallback),g_scheduleWork*1000/elapsed);
                g_scheduleBegan=now;g_scheduleWork=0;
            }
            if(g_standalone)vehicle_probe::ReportUpdateCost();
            resources::Tick();
            if(g_pacing.Ready()) {
                logger::Write("Vehicle callback spacing: %.2f ms avg / %.2f worst; %.2f ms worst outside previous frame task; intervals <=16.7/25/33.4/50/100/>100 ms: %llu/%llu/%llu/%llu/%llu/%llu (includes engine/render/scheduling/pauses; not present timing)",
                    g_pacing.sum/g_pacing.count,g_pacing.worst,g_pacing.outside,
                    static_cast<unsigned long long>(g_pacing.buckets[0]),static_cast<unsigned long long>(g_pacing.buckets[1]),
                    static_cast<unsigned long long>(g_pacing.buckets[2]),static_cast<unsigned long long>(g_pacing.buckets[3]),
                    static_cast<unsigned long long>(g_pacing.buckets[4]),static_cast<unsigned long long>(g_pacing.buckets[5]));
                g_pacing.Next();
            }
        }
        // Include reporting/logging, housekeeping and rejected polls when
        // measuring task work subtracted from the next raw callback interval.
        QueryPerformanceCounter(&end);const double elapsed=double(end.QuadPart-start.QuadPart)/ticksPerMs;
        g_scheduleWork+=elapsed;g_pacing.Finish(elapsed);
    }
    void CorePlayerUpdate(std::uintptr_t player)
    {
        shipmodel::OnPlayerUpdate(player);g_coreUpdates.Post();
    }

	class FrameTask final : public SFSETaskInterface::ITaskDelegate
	{
	public:
		void Run() override
		{

			Frame();
		}
		void Destroy() override {}
	} g_frameTask;

    void InstallFeatureSystems() {
        world::SetServices({realvehicles::ForEachLoadedStep,vehicleflight::OwnRoverId,npcvehicles::VehicleDriverId,npcvehicles::UsesVehicle,vehiclekeep::ForEachHeld});
        hoverui::Install();routeedit::Install();mapguard::Install();disableguard::Install();inputgate::Install();mappoints::Install();loadedarea::Install();
        npcvehicles::Install();profiles::Install();vehicleaim::Install();
        vehiclediag::Install();vehiclehits::InstallEnemyBar();flighttune::Load();
    }
	void WithCore(const seamless::CoreApi& api)
	{
		if (g_started.exchange(true)) return;
		seamless::SetApi(&api);
		if (!vehicleflight::Install()) {
			logger::Write("Vehicle: physics hook unavailable; disabled");
			return;
		}
        InstallFeatureSystems();
		api.OnBeforeSave(&BeforeTrafficSave);
		api.OnAfterSave(&AfterTrafficSave);
		trafficprofile::SaveReady(true);
		api.OnFrame(&Frame);
		api.OnPlayerUpdate(&CorePlayerUpdate);
		api.OnLoadReset(&Reset);
		logger::Write("Vehicle: core API v%u attached; full updates gated by native player-update pulses", api.version);
	}

	void WithoutCore()
	{
		if (g_started.exchange(true)) return;
		if (!g_tasks || !vehicle_probe::Install()) {
			logger::Write("Vehicle: standalone update unavailable; disabled");
			return;
		}
		if (!vehicleflight::Install()) {
			logger::Write("Vehicle: standalone physics hook unavailable; disabled");
			return;
		}
		g_standalone = true;
		InstallSaveHook();  // the core owns the save hook in every standalone install (2026-11-17); with the Seamless core its own hook saves
        InstallFeatureSystems();
		g_tasks->AddTaskPermanent(&g_frameTask);
		logger::Write("Vehicle: standalone ground and physics service started; native pulse scheduling with 60 Hz fallback, 240 Hz admission ceiling, no catch-up queue");
	}

	void CoreMessage(SFSEMessagingInterface::Message* msg)
	{
		if (msg->type != seamless::kMessageApi || g_started) return;
		const auto* api = static_cast<const seamless::CoreApi*>(msg->data);
		if (!api || msg->dataLen < sizeof(seamless::CoreApi) || api->version != seamless::kApiVersion) {
			g_started = true;  // a core owns the update hook; never install a second one
			logger::Write("Vehicle: incompatible core API; disabled");
			return;
		}
		WithCore(*api);
	}

	void SfseMessage(SFSEMessagingInterface::Message* msg)
	{
		switch (msg->type) {
		case SFSEMessagingInterface::kMessage_PostLoad:
			g_messaging->RegisterListener(g_handle, seamless::kCorePluginName, CoreMessage);
			break;
		case SFSEMessagingInterface::kMessage_PostDataLoad:
			if (!g_started) WithoutCore();
			break;
		case SFSEMessagingInterface::kMessage_PreLoadGame:
			g_loadBegan = GetTickCount64();
			trafficprofile::BeginLoad();
			lifecycle::Reset();
			crash::Crumb("save load: SFSE PreLoadGame");
			logger::Write("Save load: SFSE PreLoadGame (thread %lu); FVO world actions paused until it finishes", GetCurrentThreadId());
			if (g_standalone) Reset();
			break;
		case SFSEMessagingInterface::kMessage_PostLoadGame:
			g_loadDone = GetTickCount64();
			crash::Crumb("save load: SFSE PostLoadGame");
			logger::Write("Save load: PostLoadGame (%s, thread %lu)", msg->data ? "ok" : "failed", GetCurrentThreadId());
			break;
		default:
			break;
		}
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion, 122, "FlyingVehicleOverhaul", "seemless",
	SFSEPluginVersionData::kAddressIndependence_Signatures,
	SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
	{ addr::kRuntimeVersion, 0 }, 0, 0, 0,
};

__declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
	logger::Init();
	logger::Write("OBS system=runtime event=trace-start schema=1 cadence_ms=5000 transition_min_ms=250 correlation=seq,tick,pid,tid,epoch");
	telemetry::Init();
	crash::Install();
	if (sfse->runtimeVersion != addr::kRuntimeVersion) {
		logger::Write("Vehicle: unsupported Starfield runtime; disabled");
		return false;
	}
	g_handle = sfse->GetPluginHandle();
	g_messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
	g_tasks = static_cast<SFSETaskInterface*>(sfse->QueryInterface(kInterface_Task));
	if (!g_messaging || !g_tasks || !g_messaging->RegisterListener(g_handle, "SFSE", SfseMessage)) return false;
	// Finish fallible SFSE registration before installing any early detours.
	// Persistent vehicle callbacks precede the game's normal listener initialization.
	if (!vehiclediag::PrepareForDataLoad()) return false;
	InstallLoadHook();
	lifecycle::InstallCover();  // traffic's commands run behind the loading screen (lifecycle_cover.h)
	// Before the game reads its plugins: FVO_CityTraffic.esm rebuilt from the recordings when they changed (citystartup.h).
	// citystartup::Run() rebuilt FVO_CityTraffic.esm at every launch; the user removed City Traffic for its hitches (2026-10-27), so
	// core no longer writes it back.
	logger::Important("Flying Vehicle Overhaul (FVO) 1.3.1 loaded; core optional");
	logger::Write("Autopilot revision: live-heading-air-recovery-20261028 (shared chassis heading; hover escapes use airspace)");
	logger::Write("Boarding revision: pre-entry-actor-scope-20261028 (no late player seat teardown)");
	return true;
}
}

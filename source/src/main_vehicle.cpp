#include "addresses.h"
#include "features.h"
#include "framepacing.h"
#include "updateschedule.h"
#include "resources.h"
#include "airtraffic.h"
#include "context.h"
#include "core_api.h"
#include "crash.h"
#include "flighttune.h"
#include "game.h"
#include "hoverui.h"
#include "realvehicles.h"
#include "runtimecells.h"
#include "ringroad.h"
#include "routeprobes.h"
#include "citytraffic.h"
#include "citystartup.h"
#include "log.h"
#include "modscene.h"
#include "navmap.h"
#include "navfield.h"
#include "groundnav.h"
#include "environment.h"
#include "npcvehicles.h"
#include "pilotquest.h"
#include "passenger.h"
#include "traffic.h"
#include "profiles.h"
#include "routeplan.h"
#include "scantarget.h"
#include "bindings.h"
#include "inputgate.h"
#include "vehiclediag.h"
#include "native_movement_probe.h"
#include "mappoints.h"
#include "vehicleworld.h"
#include "vehiclehits.h"
#include "navseed.h"
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
#include <algorithm>
#include <Windows.h>

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
    constexpr ULONGLONG kLoadSettleMs = 3000, kLoadTimeoutMs = 180000;
    // SFSE never sent PreLoadGame / PostLoadGame here (01:53 run: no "Save load" lines), so the game's own load routine
    // FUN_141815620 ("***Loading game: %s (%s)") is wrapped: it runs the whole load on a background thread.
    constexpr std::uint32_t kLoadGame = 0x01815620;
    using LoadGame_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint64_t, std::uint64_t);
    LoadGame_t g_loadGameOriginal = nullptr;
    std::uintptr_t LoadGameDetour(std::uintptr_t a, std::uintptr_t b, std::uint64_t c, std::uint64_t d)
    {
        g_loadBegan = GetTickCount64();
        logger::Write("Save load: the game's load routine started; FVO world actions paused until it finishes");
        const auto r = g_loadGameOriginal(a, b, c, d);
        g_loadDone = GetTickCount64();
        logger::Write("Save load: the game's load routine returned after %.1f s", (g_loadDone.load() - g_loadBegan.load()) / 1000.0);
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
    std::atomic_flag g_inCallback=ATOMIC_FLAG_INIT;
    framepacing::Window g_pacing;
    updateschedule::Gate g_schedule;
    updateschedule::Pulse g_coreUpdates;
    double g_scheduleBegan=0,g_scheduleWork=0;
    void ResetCostWindow();

	void Reset()
	{
        g_worldReady=false;g_pacing={};g_schedule.Reset();g_scheduleBegan=g_scheduleWork=0;ResetCostWindow();
        runtimecells::Reset();
		bindings::ResetRuntime();
		vehicleflight::LoadReset();
		shipvehicle::Reset();
		shipmodel::Reset();
		vehicle_probe::Reset();
		realvehicles::Reset();
		ringroad::Reset();
		routeprobes::Reset();
		citytraffic::Reset();
        airtraffic::Reset();
        pilotquest::Reset();
		npcvehicles::Reset();
		passenger::Reset();
		scantarget::Reset();
		traffic::Reset();
		context::Reset();
		viewdist::Reset();
		vehiclekeep::Reset();
		profiles::Reset();
		routeplan::Reset();
		world::Reset();
		vehiclehits::Reset();
		navseed::Reset();
		navmap::Reset();
		navfield::Reset();
		groundnav::Reset();
        environment::Reset();
	}

    struct FrameCost {
        double sum[8]{},max[8]{};unsigned count=0;ULONGLONG began=0;
        void Finish(const double sample[8]) {
            const auto now=GetTickCount64();if(!began)began=now;++count;
            for(int i=0;i<8;++i){sum[i]+=sample[i];max[i]=std::max(max[i],sample[i]);}
            if(now-began<60000)return;
            logger::Write("Vehicle update cost: total %.3f ms avg / %.3f worst; identity+world %.3f / %.1f, hits %.3f / %.1f, map+navigation %.3f / %.1f, context+profiles %.3f / %.1f, NPC decisions+squads+rams %.3f / %.1f, flight %.3f / %.1f, other (vehicle registry, placement, scanner, traffic) %.3f / %.1f (avg / worst ms)",
                sum[7]/count,max[7],sum[0]/count,max[0],sum[1]/count,max[1],sum[2]/count,max[2],sum[3]/count,max[3],sum[4]/count,max[4],sum[5]/count,max[5],sum[6]/count,max[6]);
            *this={};began=now;
        }
    } g_frameCost;
    void ResetCostWindow(){g_frameCost={};}
    void Update()
    {
        LARGE_INTEGER frequency,start,previous,stamp;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);previous=start;
        double cost[8]{};auto mark=[&](int i){QueryPerformanceCounter(&stamp);cost[i]=1000.0*double(stamp.QuadPart-previous.QuadPart)/frequency.QuadPart;previous=stamp;};
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
        part(0,"collision-box scan",[]{navseed::Tick();});part(0,"lane planner",[]{routeplan::Frame();});part(0,"flight field",[]{navfield::Service();});
        part(0,"ground planner",[]{groundnav::Service();});part(0,"surface checks",[]{environment::Tick();});part(0,"nav map",[]{navmap::Tick();});mark(2);
        profiles::Tick();context::Tick();mark(3);
        npcvehicles::Tick();passenger::Tick();mark(4);
        // (The native movement-controller probe did its job, 2026-10-27: every vehicle has a controller. Off: it took an engine lock every 5 s.)
        vehicleflight::Tick();vehiclediag::Tick();mark(5);
        part(1,"ship vehicle",[]{shipvehicle::Tick();});part(1,"vehicle registry",[]{realvehicles::Tick();});part(1,"ring road",[]{ringroad::Tick();});part(1,"route markers",[]{routeprobes::Tick();});
        part(1,"city traffic",[]{citytraffic::Tick();});part(1,"air traffic",[]{airtraffic::Tick();});part(1,"scanner target",[]{scantarget::Tick();});
        part(1,"traffic destinations",[]{traffic::Tick();});part(1,"view distance",[]{viewdist::Tick();});part(1,"keep in sight",[]{vehiclekeep::Tick();});
        part(1,"log writer",[]{logger::Service();});mark(6);
        bindings::EndUpdate();
        cost[7]=1000.0*double(stamp.QuadPart-start.QuadPart)/frequency.QuadPart;g_frameCost.Finish(cost);
        // An admitted update, not a rendered frame: name slow work at most every two seconds.
        static ULONGLONG spikeAt=0;
        if(cost[7]>12.0&&GetTickCount64()-spikeAt>=2000){spikeAt=GetTickCount64();
            logger::Write("Vehicle update spike: %.1f ms (identity+world %.1f, hits %.1f, map+navigation %.1f [slowest: %s %.1f], context+profiles %.1f, NPC decisions %.1f, flight %.1f, other %.1f [slowest: %s %.1f])",
                cost[7],cost[0],cost[1],cost[2],slowName[0],slowMs[0],cost[3],cost[4],cost[5],cost[6],slowName[1],slowMs[1]);}
    }
    void Frame()
    {
        // Engine calls may pump SFSE tasks recursively. Never re-enter our
        // mutable world/AI pipeline; this guard skips rather than waits.
        if(g_inCallback.test_and_set(std::memory_order_acquire))return;
        struct Release {~Release(){g_inCallback.clear(std::memory_order_release);}} release;
        static const double ticksPerMs=[] {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart)/1000.0;}();
        LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);const double now=double(start.QuadPart)/ticksPerMs;
        g_pacing.Begin(now); // all raw callbacks, including rejected duplicate polls
        if(g_standalone) {
			const auto state = game::ReadPlanetState();
			// Reset only for a real world replacement (manager state 0/1: a load or a landing). State 3/4 is the routine
			// terrain re-centre every ~250 m of travel; resetting there switched hover off (SeamlessWatch csv 2026-10-17:
			// states 3 occur with the loading screen off, loads show 0/1 with it on; 19 resets matched the shifts in the log).
			if (g_worldReady && state.haveManager && (state.managerState == 0 || state.managerState == 1)) {
				Reset();
				g_worldReady = false;
			}
			if (state.haveManager && state.managerState == 2) g_worldReady = true;
        }
        const auto pulse=g_standalone?vehicle_probe::UpdateSerial():g_coreUpdates.Serial();
        auto work=g_schedule.Poll(now,pulse);
        if(work==updateschedule::Work::Full&&Loading(GetTickCount64()))work=updateschedule::Work::Housekeeping;  // a save is still loading
        if(work==updateschedule::Work::Full)Update();
        else if(work==updateschedule::Work::Housekeeping) {
            // UI bindings/settings and logs keep working while simulation is
            // paused. Never enumerate the scene or queue placement/AI work here.
            features::Tick();bindings::Tick(false);logger::Service();
        }
        if(work!=updateschedule::Work::Skip) {
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
        world::SetServices({realvehicles::ForEachLoadedStep,vehicleflight::OwnRoverId,npcvehicles::VehicleDriverId,npcvehicles::UsesVehicle});
        hoverui::Install();inputgate::Install();mappoints::Install();
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
			logger::Write("Save load: started; FVO world actions paused until it finishes");
			if (g_standalone) Reset();
			break;
		case SFSEMessagingInterface::kMessage_PostLoadGame:
			g_loadDone = GetTickCount64();
			logger::Write("Save load: PostLoadGame (%s)", msg->data ? "ok" : "failed");
			break;
		default:
			break;
		}
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion, 85, "FlyingVehicleOverhaul", "seemless",
	SFSEPluginVersionData::kAddressIndependence_Signatures,
	SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
	{ addr::kRuntimeVersion, 0 }, 0, 0, 0,
};

__declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
	logger::Init();
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
	// Before the game reads its plugins: FVO_CityTraffic.esm rebuilt from the recordings when they changed (citystartup.h).
	// citystartup::Run() rebuilt FVO_CityTraffic.esm at every launch; the user removed City Traffic for its hitches (2026-10-27), so
	// core no longer writes it back.
	logger::Write("Flying Vehicle Overhaul (FVO) 0.8.5 loaded; core optional");
	logger::Write("Autopilot revision: live-heading-air-recovery-20261028 (shared chassis heading; hover escapes use airspace)");
	logger::Write("Boarding revision: pre-entry-actor-scope-20261028 (no late player seat teardown)");
	return true;
}
}

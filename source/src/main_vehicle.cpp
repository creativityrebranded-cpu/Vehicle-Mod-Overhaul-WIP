#include "addresses.h"
#include "features.h"
#include "context.h"
#include "core_api.h"
#include "crash.h"
#include "flighttune.h"
#include "game.h"
#include "hoverui.h"
#include "realvehicles.h"
#include "log.h"
#include "modscene.h"
#include "navmap.h"
#include "navfield.h"
#include "environment.h"
#include "npcvehicles.h"
#include "passenger.h"
#include "traffic.h"
#include "profiles.h"
#include "routeplan.h"
#include "scantarget.h"
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

#include "sfse/PluginAPI.h"

#include <atomic>
#include <algorithm>
#include <Windows.h>

namespace plugininfo
{
	const wchar_t* Name() { return L"SeamlessVehicle"; }
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

	void Reset()
	{
		vehicleflight::LoadReset();
		shipvehicle::Reset();
		shipmodel::Reset();
		vehicle_probe::Reset();
		realvehicles::Reset();
		npcvehicles::Reset();
		passenger::Reset();
		scantarget::Reset();
		traffic::Reset();
		context::Reset();
		viewdist::Reset();
		profiles::Reset();
		routeplan::Reset();
		world::Reset();
		vehiclehits::Reset();
		navseed::Reset();
		navmap::Reset();
		navfield::Reset();
        environment::Reset();
	}

    struct FrameCost {
        double sum[8]{},max[8]{};unsigned count=0;ULONGLONG began=0;
        void Finish(const double sample[8]) {
            const auto now=GetTickCount64();if(!began)began=now;++count;
            for(int i=0;i<8;++i){sum[i]+=sample[i];max[i]=std::max(max[i],sample[i]);}
            if(now-began<60000)return;
            logger::Write("Vehicle frame cost: total %.3f ms avg / %.3f worst; identity+world %.3f / %.1f, hits %.3f / %.1f, map+navigation %.3f / %.1f, context+profiles %.3f / %.1f, NPC decisions+squads+rams %.3f / %.1f, flight %.3f / %.1f, other (vehicle registry, placement, scanner, traffic) %.3f / %.1f (avg / worst ms)",
                sum[7]/count,max[7],sum[0]/count,max[0],sum[1]/count,max[1],sum[2]/count,max[2],sum[3]/count,max[3],sum[4]/count,max[4],sum[5]/count,max[5],sum[6]/count,max[6]);
            *this={};began=now;
        }
    } g_frameCost;
    void Frame()
    {
        LARGE_INTEGER frequency,start,previous,stamp;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);previous=start;
        double cost[8]{};auto mark=[&](int i){QueryPerformanceCounter(&stamp);cost[i]=1000.0*double(stamp.QuadPart-previous.QuadPart)/frequency.QuadPart;previous=stamp;};
        modscene::NewFrame();
        features::Tick();
        vehicleflight::BeginFrame(); // decision owners must renew their commands in this frame
        npcvehicles::BeginFrame();
        world::Publish();mark(0);
        vehiclehits::Tick();mark(1);
        navseed::Tick();routeplan::Frame();navfield::Service();environment::Tick();navmap::Tick();mark(2);
        profiles::Tick();context::Tick();mark(3);
        npcvehicles::Tick();passenger::Tick();mark(4);
        vehicleflight::Tick();mark(5);
        shipvehicle::Tick();realvehicles::Tick();scantarget::Tick();traffic::Tick();viewdist::Tick();mark(6);
        cost[7]=1000.0*double(stamp.QuadPart-start.QuadPart)/frequency.QuadPart;g_frameCost.Finish(cost);
    }

	class FrameTask final : public SFSETaskInterface::ITaskDelegate
	{
	public:
		void Run() override
		{
			const auto state = game::ReadPlanetState();
			// Reset only for a real world replacement (manager state 0/1: a load or a landing). State 3/4 is the routine
			// terrain re-centre every ~250 m of travel; resetting there switched hover off (SeamlessWatch csv 2026-10-17:
			// states 3 occur with the loading screen off, loads show 0/1 with it on; 19 resets matched the shifts in the log).
			if (m_ready && state.haveManager && (state.managerState == 0 || state.managerState == 1)) {
				Reset();
				m_ready = false;
			}
			if (state.haveManager && state.managerState == 2) m_ready = true;
			Frame();
		}
		void Destroy() override {}
	private:
		bool m_ready = false;
	} g_frameTask;

	void WithCore(const seamless::CoreApi& api)
	{
		if (g_started.exchange(true)) return;
		seamless::SetApi(&api);
		if (!vehicleflight::Install()) {
			logger::Write("Vehicle: physics hook unavailable; disabled");
			return;
		}
		hoverui::Install();
		mappoints::Install();
		npcvehicles::Install();
		profiles::Install();
		vehicleaim::Install();
		vehiclehits::InstallEnemyBar();
		flighttune::Load();
		api.OnFrame(&Frame);
		api.OnPlayerUpdate(&shipmodel::OnPlayerUpdate);
		api.OnLoadReset(&Reset);
		logger::Write("Vehicle: core API v%u attached", api.version);
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
		hoverui::Install();
		mappoints::Install();
		npcvehicles::Install();
		profiles::Install();
		vehicleaim::Install();
		vehiclehits::InstallEnemyBar();
		flighttune::Load();
		g_tasks->AddTaskPermanent(&g_frameTask);
		logger::Write("Vehicle: standalone ground and physics service started");
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
			if (g_standalone) Reset();
			break;
		default:
			break;
		}
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion, 63, "SeamlessVehicle", "seemless",
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
	if (!g_messaging || !g_messaging->RegisterListener(g_handle, "SFSE", SfseMessage)) return false;
	logger::Write("SeamlessVehicle 0.63.1 loaded; core optional");
	return true;
}
}

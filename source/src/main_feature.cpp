// Feature plugins (SeamlessContent, SeamlessShip, SeamlessVehicle): one source, built three times with SP_FEATURE_*.
// Each verifies the game build and the code signatures, waits for the core plugin's API (core_api.h, sent at
// PostPostLoad) and then installs only its own hooks and registers its callbacks. Without the core it stays disabled.
#include "addresses.h"
#include "core_api.h"
#include "game.h"
#include "log.h"
#include "plugininfo.h"
#include "telemetry.h"

#if defined(SP_FEATURE_CONTENT)
#include "content.h"
#elif defined(SP_FEATURE_MARKERS)
#include "globe.h"
#elif defined(SP_FEATURE_SHIP)
#include "camtune.h"
#include "flight.h"
#include "flighttune.h"
#include "shipflight.h"
#include "shipmodel.h"
#elif defined(SP_FEATURE_CHECK)
#include "shiftcheck.h"
#elif defined(SP_FEATURE_REANCHOR)
#include "reanchor.h"
#elif defined(SP_FEATURE_PACKS)
#include "packs.h"
#elif defined(SP_FEATURE_ACTORS)
#include "actors.h"
#elif defined(SP_FEATURE_DISTANT)
#include "distant.h"
#elif defined(SP_FEATURE_STREAM)
#include "stream.h"
#elif defined(SP_FEATURE_VEHICLE)
#include "flighttune.h"
#include "shipmodel.h"
#include "shipvehicle.h"
#include "vehicleflight.h"
#include "vehicle_probe.h"
#else
#error "Define one SP_FEATURE_* (CONTENT, SHIP, VEHICLE, CHECK, REANCHOR, PACKS, ACTORS, DISTANT, STREAM)"
#endif

#include "sfse/PluginAPI.h"

#include <atomic>

namespace seamless
{
	void SetApi(const CoreApi* api);
}

#if defined(SP_FEATURE_CONTENT)
#define SP_NAME "SeamlessContent"
#define SP_WNAME L"SeamlessContent"
#define SP_PREFIX L"content-"
#define SP_ABOUT "zone content (landing points, places woken/put to sleep)"
#elif defined(SP_FEATURE_MARKERS)
#define SP_NAME "SeamlessMarkers"
#define SP_WNAME L"SeamlessMarkers"
#define SP_PREFIX L"markers-"
#define SP_ABOUT "planet globe: the ship's recorded planet position, with native locations unchanged"
#elif defined(SP_FEATURE_SHIP)
#define SP_NAME "SeamlessShip"
#define SP_WNAME L"SeamlessShip"
#define SP_PREFIX L"ship-"
#define SP_ABOUT "ship summon (F6, aimed, EnableWithLanding), ship beacon, load guard, ship model / flight experiments"
#elif defined(SP_FEATURE_CHECK)
#define SP_NAME "SeamlessCheck"
#define SP_WNAME L"SeamlessCheck"
#define SP_PREFIX L"check-"
#define SP_ABOUT "test plugin: after every world shift, checks objects, player, ground ring and actors against the moment before; logs PASS/WARN/FAIL per system; changes nothing"
#elif defined(SP_FEATURE_REANCHOR)
#define SP_NAME "SeamlessReanchor"
#define SP_WNAME L"SeamlessReanchor"
#define SP_PREFIX L"reanchor-"
#define SP_ABOUT "manual landing refresh diagnostic; automatic quicksave and quickload disabled after a stalled planet load"
#elif defined(SP_FEATURE_PACKS)
#define SP_NAME "SeamlessPacks"
#define SP_WNAME L"SeamlessPacks"
#define SP_PREFIX L"packs-"
#define SP_ABOUT "building packs across shifts: measures which grouped instances stay in the old frame; later owner rebuild"
#elif defined(SP_FEATURE_ACTORS)
#define SP_NAME "SeamlessActors"
#define SP_WNAME L"SeamlessActors"
#define SP_PREFIX L"actors-"
#define SP_ABOUT "NPCs and creatures across shifts: measures actors heading back to the old frame or jumping; later path re-plan"
#elif defined(SP_FEATURE_DISTANT)
#define SP_NAME "SeamlessDistant"
#define SP_WNAME L"SeamlessDistant"
#define SP_PREFIX L"distant-"
#define SP_ABOUT "distant terrain / LOD across shifts: hidden at each shift (tdl flag) and shown again once settled; scene census"
#elif defined(SP_FEATURE_STREAM)
#define SP_NAME "SeamlessStream"
#define SP_WNAME L"SeamlessStream"
#define SP_PREFIX L"stream-"
#define SP_ABOUT "forward native grid lead and readiness reporting; speculative off-grid requests suspended"
#else
#define SP_NAME "SeamlessVehicle"
#define SP_WNAME L"SeamlessVehicle"
#define SP_PREFIX L"vehicle-"
#define SP_ABOUT "rover flight model 0.57: pushes the rover body (no overwrite), rotational momentum, bank/lean/pitch-speed couplings, path following, mouse aims yaw + pitch (camera never written), A/D yaw, W/S throttle (1-4 presets), Shift boost / double-tap holds, double-tap Space hover, up/down keys, roll, elevation assist, collision ends hover, infinite boost; opt-in ship-hull visual preview retains rover collision"
#endif

#if defined(SP_FEATURE_VEHICLE)
constexpr const char* kFeatureVersion = "0.58.6";
#elif defined(SP_FEATURE_MARKERS)
constexpr const char* kFeatureVersion = "0.2.0";
#elif defined(SP_FEATURE_SHIP)
constexpr const char* kFeatureVersion = "0.59.4";  // beacon accepts a landed ship cell with no worldspace parent
#elif defined(SP_FEATURE_CONTENT)
constexpr const char* kFeatureVersion = "0.58.4";
#elif defined(SP_FEATURE_STREAM)
constexpr const char* kFeatureVersion = "0.2.3";
#elif defined(SP_FEATURE_REANCHOR)
constexpr const char* kFeatureVersion = "0.2.4";
#elif defined(SP_FEATURE_ACTORS)
constexpr const char* kFeatureVersion = "0.1.1";
#elif defined(SP_FEATURE_DISTANT)
constexpr const char* kFeatureVersion = "0.6.1";
#else
constexpr const char* kFeatureVersion = "0.1.0";  // module plugins (2026-10-15)
#endif

namespace plugininfo
{
	const wchar_t* Name() { return SP_WNAME; }
	const wchar_t* TelemetryPrefix() { return SP_PREFIX; }
}

namespace
{
	PluginHandle g_handle = static_cast<PluginHandle>(kPluginHandle_Invalid);
	SFSEMessagingInterface* g_messaging = nullptr;
	SFSETaskInterface* g_tasks = nullptr;
	std::atomic<bool> g_started{ false };

#if defined(SP_FEATURE_CONTENT)
	void Frame()
	{
		content::Tick();  // zones as landing zones: generate / delete planet content
	}

	void Start(const seamless::CoreApi& api)
	{
		api.OnFrame(&Frame);
		api.OnZoneEnter(&content::OnZoneEnter);
		api.SetContentPostSteps(&content::LastPostSteps);
		api.OnLoadReset(&content::Reset);
	}
#elif defined(SP_FEATURE_MARKERS)
	void Start(const seamless::CoreApi& api)
	{
		if (!globe::Install()) {
			logger::Write("Globe marker hook not installed (ship map marker unavailable)");
		}
		api.OnFrame(&globe::Tick);
	}
#elif defined(SP_FEATURE_SHIP)
	void Frame()
	{
		flight::Tick();      // F6 summons the ship; load guard
		shipflight::Tick();  // planet ship flight test
	}

	void LoadReset()
	{
		shipflight::Reset();
		shipmodel::Reset();
	}

	void Start(const seamless::CoreApi& api)
	{
		flight::Install();
		camtune::Load();
		flighttune::Load();
		api.OnFrame(&Frame);
		api.OnSync(&flight::SyncTick);
		api.OnBeforeSave(&flight::BeforeSave);
		api.OnAfterSave(&flight::AfterSave);
		api.OnPlayerUpdate(&shipmodel::OnPlayerUpdate);
		api.AddBusy(&flight::Busy);
		api.OnLoadReset(&LoadReset);
	}
#elif defined(SP_FEATURE_CHECK)
	void Start(const seamless::CoreApi& api)
	{
		api.OnSync(&shiftcheck::Sync);
		api.OnLoadReset(&shiftcheck::LoadReset);
		api.OnBeforeShift(&shiftcheck::BeforeShift);
		api.OnShift(&shiftcheck::AfterShift);
	}
#elif defined(SP_FEATURE_REANCHOR)
	void Start(const seamless::CoreApi& api)
	{
		api.OnFrame(&reanchor::Tick);
		api.OnLoadReset(&reanchor::LoadReset);
	}
#elif defined(SP_FEATURE_PACKS)
	void Start(const seamless::CoreApi& api)
	{
		api.OnBeforeShift(&packs::BeforeShift);
		api.OnShift(&packs::AfterShift);
		api.OnSync(&packs::Sync);
		api.OnLoadReset(&packs::LoadReset);
	}
#elif defined(SP_FEATURE_ACTORS)
	void Start(const seamless::CoreApi& api)
	{
		api.OnBeforeShift(&actors::BeforeShift);
		api.OnShift(&actors::AfterShift);
		api.OnSync(&actors::Sync);
		api.OnLoadReset(&actors::LoadReset);
	}
#elif defined(SP_FEATURE_DISTANT)
	void Start(const seamless::CoreApi& api)
	{
		api.OnBeforeShift(&distant::BeforeShift);
		api.OnShift(&distant::AfterShift);
		api.OnSync(&distant::Sync);
		api.OnLoadReset(&distant::LoadReset);
	}
#elif defined(SP_FEATURE_STREAM)
	void Start(const seamless::CoreApi& api)
	{
		stream::Start();
		api.OnSync(&stream::Sync);
	}
#else
	void VehicleFrame()
	{
		vehicleflight::Tick();
		shipvehicle::Tick();
	}

	class StandaloneVehicleTask final : public SFSETaskInterface::ITaskDelegate
	{
	public:
		void Run() override
		{
			const auto state = game::ReadPlanetState();
			if (m_ready && state.haveManager && state.managerState != 2) {
				vehicleflight::LoadReset();
				shipvehicle::Reset();
				shipmodel::Reset();
				vehicle_probe::Reset();
				m_ready = false;
			}
			if (state.haveManager && state.managerState == 2) m_ready = true;
			VehicleFrame();
		}
		void Destroy() override {}  // permanent SFSE task
	private:
		bool m_ready = false;
	} g_standaloneTask;

	void StartStandaloneVehicle()
	{
		if (g_started.exchange(true)) return;
		if (!g_tasks || !vehicle_probe::Install()) {
			logger::Write("Vehicle: standalone update context unavailable; disabled");
			return;
		}
		vehicleflight::Install();
		flighttune::Load();
		g_tasks->AddTaskPermanent(&g_standaloneTask);
		logger::Write("Vehicle: standalone flight and ground service started");
	}

	void Start(const seamless::CoreApi& api)
	{
		vehicleflight::Install();
		flighttune::Load();
		api.OnFrame(&VehicleFrame);
		api.OnPlayerUpdate(&shipmodel::OnPlayerUpdate);
		api.OnLoadReset(&vehicleflight::LoadReset);
		api.OnLoadReset(&shipvehicle::Reset);
	}
#endif

	void OnCoreMessage(SFSEMessagingInterface::Message* msg)
	{
		if (msg->type != seamless::kMessageApi || g_started.load()) {
			return;
		}
		const auto* api = static_cast<const seamless::CoreApi*>(msg->data);
		if (!api || msg->dataLen < sizeof(std::uint32_t) || api->version != seamless::kApiVersion || msg->dataLen < sizeof(seamless::CoreApi)) {
			logger::Write("%s: core API version mismatch (got %u, need %u); disabled", SP_NAME, api ? api->version : 0u, seamless::kApiVersion);
			return;
		}
		seamless::SetApi(api);
		g_started = true;
		Start(*api);
		logger::Write("%s: core API v%u received; started", SP_NAME, api->version);
	}

	void OnSfseMessage(SFSEMessagingInterface::Message* msg)
	{
		switch (msg->type) {
		case SFSEMessagingInterface::kMessage_PostLoad:
			if (!g_messaging->RegisterListener(g_handle, seamless::kCorePluginName, OnCoreMessage)) {
				logger::Write("%s: the core plugin (%s) is not loaded; disabled", SP_NAME, seamless::kCorePluginName);
			}
			break;
		case SFSEMessagingInterface::kMessage_PostDataLoad:
		#if defined(SP_FEATURE_VEHICLE)
			if (!g_started.load()) StartStandaloneVehicle();
		#else
			if (!g_started.load()) {
				logger::Write("%s: no API from the core plugin by the time game data loaded; disabled", SP_NAME);
			}
		#endif
			break;
		#if defined(SP_FEATURE_VEHICLE)
		case SFSEMessagingInterface::kMessage_PreLoadGame:
			vehicleflight::LoadReset();
			shipvehicle::Reset();
			shipmodel::Reset();
			vehicle_probe::Reset();
			break;
		#endif
		default:
			break;
		}
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion,
	46,
	SP_NAME,
	"seemless",
	SFSEPluginVersionData::kAddressIndependence_Signatures,
	SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
	{ addr::kRuntimeVersion, 0 },
	0,
	0, 0,
};

__declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
	logger::Init();
	telemetry::Init();
	logger::Write("%s %s (%s). Runtime 0x%08X, SFSE 0x%08X", SP_NAME, kFeatureVersion, SP_ABOUT, sfse->runtimeVersion, sfse->sfseVersion);
	if (sfse->runtimeVersion != addr::kRuntimeVersion) {
		logger::Write("Wrong game version; this build supports 1.16.244 only. Disabled.");
		return false;
	}
	// No full signature check here: SFSE may load the core first, and its hooks change the bytes the table checks
	// (0.48.1: Ship and Vehicle refused to load). The core verifies the build; this plugin's own hook targets are
	// checked byte for byte when they are installed (hook::Install).
	g_handle = sfse->GetPluginHandle();
	g_messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
	g_tasks = static_cast<SFSETaskInterface*>(sfse->QueryInterface(kInterface_Task));
	if (!g_messaging || !g_messaging->RegisterListener(g_handle, "SFSE", OnSfseMessage)) {
		logger::Write("Could not get the SFSE messaging interface. Disabled.");
		return false;
	}
	telemetry::Record("session", "\"plugin\":\"%s %s\"", SP_NAME, kFeatureVersion);
	return true;
}
}

// SeamlessPlanets (core): SFSE plugin.
// Verifies it is running on the exact game build it was made for, then re-centres the world on planets
// so the player never leaves the physics world (the cause of the known breakdown past the landing wall).
// Owns every shared engine hook; the feature plugins (SeamlessContent, SeamlessShip, SeamlessVehicle) receive the
// core API (core_api.h) at PostPostLoad and register their callbacks.

#include "addresses.h"
#include "api_core.h"
#include "plugininfo.h"
#include "controlmap.h"
#include "landing.h"
#include "content.h"
#include "globe.h"
#include "flight.h"
#include "shipflight.h"
#include "shipmodel.h"
#include "camtune.h"
#include "flighttune.h"
#include "distant.h"
#include "stream.h"

#include <Windows.h>
#include "diagnostics.h"
#include "game.h"
#include "autosave.h"
#include "crash.h"
#include "watchpoint.h"
#include "trace.h"
#include "terrainguard.h"
#include "playarea.h"
#include "navgen.h"
#include "hangwatch.h"
#include "parking.h"
#include "prefetch.h"
#include "gametick.h"
#include "rebase.h"
#include "recentre.h"
#include "handoff.h"
#include "log.h"
#include "notify.h"
#include "telemetry.h"

#include "sfse/PluginAPI.h"

#include <atomic>

namespace plugininfo
{
	const wchar_t* Name() { return L"SeamlessPlanets"; }
	const wchar_t* TelemetryPrefix() { return L""; }
}

namespace
{
	PluginHandle g_handle = static_cast<PluginHandle>(kPluginHandle_Invalid);
	SFSEMessagingInterface* g_messaging = nullptr;
	SFSETaskInterface* g_tasks = nullptr;
	std::atomic_bool g_inGame = false;
	std::atomic_bool g_loadResetIssued = false;
	std::atomic_bool g_seenReadyPlanet = false;
	std::atomic_bool g_sawSaveLoad = false;

	class FrameTask : public SFSETaskInterface::ITaskDelegate
	{
	public:
		void Run() override
		{
			if (!m_logged) {
				m_logged = true;
				logger::Write("Frame task running");
			}
			telemetry::Frame();
			hangwatch::NoteFrame();
			if (g_inGame) {
				if (recentre::WatchFrame()) {  // reload/transition while switched: back to the original frame
					handoff::Reset();
				}
				// SFSE's PreLoadGame is absent for some in-game loads. Classify the native
				// planet setup by its caller: a surface landing also leaves state 2, but
				// is not a save load and must preserve the ship and feature state.
				const auto planet = game::ReadPlanetState();
				if (planet.haveManager && planet.managerState == 2) {
					g_seenReadyPlanet = true;
					if (g_sawSaveLoad.exchange(false)) g_loadResetIssued = false;
				}
				static std::uint64_t seenNativeLoad = 0;
				const auto nativeLoad = recentre::NativeSaveLoadSerial();
				if (nativeLoad != seenNativeLoad) {
					seenNativeLoad = nativeLoad;
					if (g_seenReadyPlanet) g_sawSaveLoad = true;
					if (g_seenReadyPlanet && !g_loadResetIssued.exchange(true)) {
						logger::Write("Core: native save load detected; resetting save-local feature state");
						handoff::Reset();
						rebase::ResetAfterLoad();
						prefetch::ResetProbes();
						content::Reset();
						shipflight::Reset();
						shipmodel::Reset();
						distant::LoadReset();
						parking::OnLoad(nullptr);  // save identity unknown: never borrow another save's spot
						coreapi::FireLoadReset();
					}
				}
				recentre::FixStaleCells();
				recentre::Tick();  // runs the re-centre the zone handoff requests (zone = physics region)
				recentre::FrameCheckTick();  // F10: measured re-centre test
				watchpoint::Tick();
				trace::HotkeyTick();
				recentre::CellWatchTick();   // data: grid cells after shifts / when the player's cell is not attached
				static bool keysReported = false;
				if (!keysReported) {
					keysReported = true;
					controlmap::Dump();
					for (const int vk : { VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11 }) {
						logger::Write("Keys: %s (used by this mod) is bound by the game to: %s", controlmap::KeyName(vk).c_str(), controlmap::WhoUses(vk).c_str());
					}
				}
				landing::Tick();
				content::Tick();
				flight::Tick();
				shipflight::Tick();
				globe::Tick();
				coreapi::FireFrame();  // optional standalone vehicle
				autosave::Tick();  // autosave while playing (the game's own only fires on pause)
				handoff::Tick();  // zones; keeps the wall off and the physics world enlarged
				playarea::Tick();  // content area follows the player
				rebase::Tick();    // restore ships hidden by the retired distance rule; preserve flight hiding
				readout::Tick();
				// readout::TrackPlayerMarker();  // off (user, 2026-10-15): no map marker following the player
			}
		}

		void Destroy() override {}  // permanent task, never freed

	private:
		bool m_logged = false;
	};

	FrameTask g_frameTask;

	void OnSfseMessage(SFSEMessagingInterface::Message* msg)
	{
		switch (msg->type) {
		case SFSEMessagingInterface::kMessage_PostPostLoad:
			// Every plugin has registered its listeners at PostLoad: hand the feature plugins the core API.
			g_messaging->Dispatch(g_handle, seamless::kMessageApi, const_cast<seamless::CoreApi*>(&coreapi::Table()),
				sizeof(seamless::CoreApi), nullptr);
			logger::Write("Core: API v%u sent to the feature plugins", seamless::kApiVersion);
			break;
		case SFSEMessagingInterface::kMessage_PostDataLoad:
			logger::Write("Game data loaded");
			handoff::EnlargePhysicsWorld();  // after the INI files are read, before any outdoor world exists
			diagnostics::Start();
			g_tasks->AddTaskPermanent(&g_frameTask);
			// Not every launch sends a load-game message, so enable now; Tick itself checks that the
			// player is outdoors on a planet with the planet manager idle.
			g_inGame = true;
			break;
		case SFSEMessagingInterface::kMessage_PreLoadGame:
			// The save replaces the world in its original frame: put our bookkeeping back first. Persistent
			// references are replaced by the save, so they are not moved.
			logger::Write("Loading a save");
			g_loadResetIssued = true;
			g_sawSaveLoad = true;
			recentre::ResetAllFrames("loading a save", false);
			handoff::Reset();
			rebase::ResetAfterLoad();
			prefetch::ResetProbes();
			content::Reset();
			shipflight::Reset();
			shipmodel::Reset();
			distant::LoadReset();
			parking::OnLoad(static_cast<const char*>(msg->data));
			coreapi::FireLoadReset();  // content, ship flight, ship model
			break;
		case SFSEMessagingInterface::kMessage_PreSaveGame:
			logger::Write("Saving");
			rebase::PreSave();  // saving flag only; SaveGame handles the frame
			// The SaveGame hook persists parking with the actual save name; this message is not reliable here.
			break;
		case SFSEMessagingInterface::kMessage_PostSaveGame:
			rebase::PostSave();
			break;
		case SFSEMessagingInterface::kMessage_PostLoadGame:
			logger::Write("Save loaded");
			g_inGame = true;
			break;

		default:
			break;
		}
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion,
	25,
	"SeamlessPlanets",
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
	trace::Init();
	crash::Install();
	watchpoint::Install();
	logger::Write("SeamlessPlanets 0.60.0 (consolidated traversal; module API v%u). Runtime 0x%08X, SFSE 0x%08X", seamless::kApiVersion,
		sfse->runtimeVersion, sfse->sfseVersion);

	if (sfse->runtimeVersion != addr::kRuntimeVersion) {
		logger::Write("Wrong game version; this build supports 1.16.244 only. Disabled.");
		return false;
	}
	if (!game::VerifySignatures()) {
		logger::Write("Code signatures do not match 1.16.244. Disabled.");
		return false;
	}

	g_handle = sfse->GetPluginHandle();
	g_messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
	g_tasks = static_cast<SFSETaskInterface*>(sfse->QueryInterface(kInterface_Task));
	if (!g_messaging || !g_tasks || !g_messaging->RegisterListener(g_handle, "SFSE", OnSfseMessage)) {
		logger::Write("Could not get SFSE messaging/task interfaces. Disabled.");
		return false;
	}
	handoff::EnlargePhysicsWorld();
	if (!prefetch::Install()) {
		logger::Write("Prefetch hook not installed (grid stays centred on the player)");
	}
	playarea::Install();
	navgen::Install();
	rebase::Install();
	coreapi::InstallMouseHook();
	gametick::Install();
	flight::Install();
	camtune::Load();
	flighttune::Load();
	if (!globe::Install()) logger::Write("Globe marker hook unavailable");
	stream::Start();
	coreapi::Table().OnSync(&stream::Sync);
	coreapi::Table().OnBeforeShift(&distant::BeforeShift);
	coreapi::Table().OnShift(&distant::AfterShift);
	coreapi::Table().OnSync(&distant::Sync);
	terrainguard::Install();
	recentre::InstallLoadCellFix();
	recentre::InstallLoadCellBuildFix();
	recentre::InstallLoadCellRequestTrace();
	recentre::InstallSyncPoint();
	recentre::InstallStaleWriteFix();
	recentre::InstallSaveHook();
	recentre::InstallLoadDiagnostics();
	logger::Write("Signatures verified; waiting for game data");
	telemetry::Record("session", "\"plugin\":\"core 0.60.0\",\"runtime\":\"%08X\",\"sfse\":\"%08X\"",
		sfse->runtimeVersion, sfse->sfseVersion);
	return true;
}
}

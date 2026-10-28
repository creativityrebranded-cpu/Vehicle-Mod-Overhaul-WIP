// Interactive Cover (2026-10-23): Mass Effect style run-to-cover on foot (notes/INTERACTIVE_COVER_PLAN.md). A standalone
// SFSE plugin. Test stage: only the ' test key (cover_test), which checks crouch, input writing, sprint + slide and the
// game's enter-cover action before the real mod is built.
#include "addresses.h"
#include "cover_test.h"
#include "crash.h"
#include "log.h"
#include "plugininfo.h"

#include "sfse/PluginAPI.h"

#include <atomic>

namespace plugininfo
{
	const wchar_t* Name() { return L"InteractiveCover"; }
	const wchar_t* TelemetryPrefix() { return L"cover-"; }
}

namespace
{
	PluginHandle g_handle = static_cast<PluginHandle>(kPluginHandle_Invalid);
	SFSEMessagingInterface* g_messaging = nullptr;
	SFSETaskInterface* g_tasks = nullptr;
	std::atomic_bool g_started = false;

	class FrameTask final : public SFSETaskInterface::ITaskDelegate
	{
	public:
		void Run() override { covertest::Tick();logger::Service(); }
		void Destroy() override {}
	} g_frameTask;

	void Start()
	{
		if (g_started.exchange(true)) return;
		if (!g_tasks) {
			logger::Write("InteractiveCover: the SFSE task interface is unavailable; disabled");
			return;
		}
		covertest::Install();
		g_tasks->AddTaskPermanent(&g_frameTask);
		logger::Write("InteractiveCover: started; test key ' (crouch, walk forward, sprint + slide, enter-cover action)");
	}

	void SfseMessage(SFSEMessagingInterface::Message* msg)
	{
		switch (msg->type) {
		case SFSEMessagingInterface::kMessage_PostDataLoad: Start(); break;
		case SFSEMessagingInterface::kMessage_PreLoadGame: covertest::Reset(); break;
		default: break;
		}
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion, 1, "InteractiveCover", "seemless",
	SFSEPluginVersionData::kAddressIndependence_Signatures,
	SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
	{ addr::kRuntimeVersion, 0 }, 0, 0, 0,
};

__declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
	logger::Init();
	crash::Install();
	if (sfse->runtimeVersion != addr::kRuntimeVersion) {
		logger::Write("InteractiveCover: unsupported Starfield runtime; disabled");
		return false;
	}
	g_handle = sfse->GetPluginHandle();
	g_messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
	g_tasks = static_cast<SFSETaskInterface*>(sfse->QueryInterface(kInterface_Task));
	if (!g_messaging || !g_messaging->RegisterListener(g_handle, "SFSE", SfseMessage)) return false;
	logger::Write("InteractiveCover 0.1 (test stage) loaded");
	return true;
}
}

// ---------------------------------------------------------------------------
// Liftoff - startup progress splash for Starfield (SFSE plugin)
//
// How SFSE uses this file:
//   1. It reads SFSEPlugin_Version (below) without running anything, to decide
//      whether this DLL is compatible with the installed game.
//   2. Very early in the game's startup, before the game window exists,
//      it calls SFSEPlugin_Load. That's where we set everything up.
//   3. Later it sends us messages; kMessage_PostPostDataLoad means all game
//      data-load callbacks have finished, which is our "100%".
// ---------------------------------------------------------------------------
// CommonLibSF must come before anything that includes <windows.h>, whose
// macros (ERROR etc.) break REX/LOG.h.
#include <SFSE/Impl/PCH.h>
#include <SFSE/API.h>

#include "Config.h"
#include "FileTrace.h"
#include "FileWatch.h"
#include "GameReady.h"
#include "Log.h"
#include "OsfSettings.h"
#include "Progress.h"
#include "Splash.h"
#include "Watchdog.h"
#include "sfse_api.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
	HMODULE g_self = nullptr;

	// The folder this DLL lives in, e.g. C:\...\Starfield\Data\SFSE\Plugins\  (with trailing slash)
	std::wstring PluginFolder()
	{
		wchar_t path[MAX_PATH * 2]{};
		GetModuleFileNameW(g_self, path, MAX_PATH * 2);
		std::wstring folder = path;
		folder.erase(folder.find_last_of(L"\\/") + 1);
		return folder;
	}

	// Documents\My Games\Starfield\SFSE\  (same place SFSE writes its own logs)
	std::wstring SfseUserFolder()
	{
		PWSTR documents = nullptr;
		std::wstring folder;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
			folder = std::wstring(documents) + L"\\My Games\\Starfield\\SFSE\\";
		CoTaskMemFree(documents);
		if (!folder.empty())
			SHCreateDirectoryExW(nullptr, (folder + L"Logs").c_str(), nullptr);
		return folder;
	}

	void LogMachine(const SFSEInterface* sfse)
	{
		Log::Write("Liftoff %d.%d.%d", LIFTOFF_VERSION_MAJOR, LIFTOFF_VERSION_MINOR, LIFTOFF_VERSION_PATCH);
		const std::uint32_t version = sfse->runtimeVersion;
		Log::Write("game version %u.%u.%u", (version >> 24) & 0xFF, (version >> 16) & 0xFF, (version >> 4) & 0xFFF);

		SYSTEM_INFO system{};
		GetSystemInfo(&system);
		MEMORYSTATUSEX memory{};
		memory.dwLength = sizeof(memory);
		if (GlobalMemoryStatusEx(&memory)) {
			const double gb = 1024.0 * 1024.0 * 1024.0;
			Log::Write("pc: %lu logical CPU cores, %.1f GB RAM (%.1f GB free), commit limit %.1f GB",
			           system.dwNumberOfProcessors, double(memory.ullTotalPhys) / gb,
			           double(memory.ullAvailPhys) / gb, double(memory.ullTotalPageFile) / gb);
		}

		wchar_t exe[MAX_PATH * 2]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
		Log::Write("game exe: %s", Log::Narrow(exe).c_str());
	}

	void OnSfseMessage(SFSEMessagingInterface::Message* msg)
	{
		Log::Write("SFSE message: sender=%s type=%u data=%u",
		           msg->sender ? msg->sender : "(unknown)", msg->type, msg->dataLen);

		char saveName[260]{};
		if (msg->type >= SFSEMessagingInterface::kMessage_PreSaveGame &&
		    msg->type <= SFSEMessagingInterface::kMessage_PostLoadGame && msg->data && msg->dataLen > 0) {
			const size_t length = std::min<size_t>(msg->dataLen, sizeof(saveName) - 1);
			std::memcpy(saveName, msg->data, length);
		}

		switch (msg->type) {
		case SFSEMessagingInterface::kMessage_PostLoad:
			// Don't replace the live file activity status with a generic message.
			Log::Write("SFSE says: extender plugins loaded");
			break;
		case SFSEMessagingInterface::kMessage_PostDataLoad:
			Log::Write("SFSE says: game data loaded; waiting for data-load callbacks");
			break;
		case SFSEMessagingInterface::kMessage_PostPostDataLoad:
			Log::Write("SFSE says: game data-load callbacks finished");
			Progress::Finish();      // saves timings; the splash sees "done" and fades out
			break;
		case SFSEMessagingInterface::kMessage_PreLoadGame:
			Progress::BeginSaveLoad(Log::Widen(saveName));
			Watchdog::LoadStarted(saveName);
			GameReady::Begin(true);
			break;
		case SFSEMessagingInterface::kMessage_PostLoadGame:
			GameReady::PostLoadMessageReceived();
			break;
		case SFSEMessagingInterface::kMessage_PreSaveGame:
			Log::Write("save started: %s", saveName);
			break;
		case SFSEMessagingInterface::kMessage_PostSaveGame:
			Log::Write("save finished: %s", saveName);
			break;
		default:
			break;
		}
	}
}

// Tell SFSE who we are and that we don't depend on a particular game version.
// Player world readiness uses CommonLibSF and the Address Library, with the
// Starfield 1.14.70+ structure layout.
extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion,
	// Version from CMakeLists.txt's project(), packed the way SFSE expects (major.minor.patch).
	(LIFTOFF_VERSION_MAJOR << 24) | (LIFTOFF_VERSION_MINOR << 16) | (LIFTOFF_VERSION_PATCH << 4),
	"Liftoff",                                                // name shown in sfse.txt
	"drvictorvs",
	SFSEPluginVersionData::kAddressIndependence_AddressLibraryV2,
	SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
	{ 0 },                                                    // version list not needed
	0,                                                        // any SFSE version
	0, 0,                                                     // reserved
};
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
	SFSE::Init(reinterpret_cast<const SFSE::LoadInterface*>(sfse), { .log = false, .hook = false });

	const std::wstring userFolder   = SfseUserFolder();
	const std::wstring pluginFolder = PluginFolder();

	Log::Open(userFolder + L"Logs\\Liftoff.log");
	Log::Write("Liftoff loaded");
	LogMachine(sfse);

	const std::wstring configPath = pluginFolder + L"Liftoff.ini";
	const Config cfg = LoadConfig(configPath);
	OsfSettings::Start(configPath);
	if (!cfg.enabled) {
		Log::Write("disabled in Liftoff.ini");
		return true;
	}

	auto* messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
	if (!messaging) {
		Log::Write("no messaging interface; cannot tell when loading ends, so staying off");
		return false;
	}
	if (!messaging->RegisterListener(sfse->GetPluginHandle(), "SFSE", OnSfseMessage)) {
		Log::Write("could not register for SFSE messages");
		return false;
	}

	// Order matters: Progress must be ready before hooks start reporting to it.
	Progress::Init(userFolder + L"Liftoff_profile.txt");
	if (!GameReady::Start())
		return false;
	FileWatch::Install(cfg.trackedExtensions, cfg.logAllFiles);
	FileTrace::Install(cfg);
	Watchdog::Start(cfg);
	Splash::Start(cfg);
	return true;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	// Keep DllMain minimal: Windows holds a lock while it runs. Real work goes in SFSEPlugin_Load.
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = module;
		DisableThreadLibraryCalls(module);
	}
	return TRUE;
}

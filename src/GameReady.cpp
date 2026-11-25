// CommonLibSF headers come first: <windows.h> (pulled in by Watchdog.h) defines
// macros such as ERROR that break REX/LOG.h.
#include <SFSE/Impl/PCH.h>
#include <RE/P/PlayerCharacter.h>
#include <SFSE/API.h>

#include "GameReady.h"

#include "Log.h"
#include "Progress.h"
#include "Watchdog.h"

#include <windows.h>

#include <atomic>

namespace
{
	std::atomic<bool> g_active{ false };
	std::atomic<bool> g_waitForPostLoad{ false };
	std::atomic<bool> g_postLoadReceived{ false };
	std::atomic<unsigned long long> g_startedAt{ 0 };
	std::atomic<unsigned> g_stableFrames{ 0 };
	std::atomic<bool> g_sawPlayerOutOfWorld{ false };

	bool PlayerIsInLoadedWorld()
	{
		RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->parentCell)
			return false;

		auto loadedData = player->loadedData.LockRead();
		const RE::LOADED_REF_DATA* data = *loadedData;
		return data && static_cast<bool>(data->data3D);
	}

	void Poll()
	{
		if (!g_active.load(std::memory_order_acquire))
			return;
		if (g_waitForPostLoad.load(std::memory_order_relaxed) &&
		    !g_postLoadReceived.load(std::memory_order_acquire))
			return;

		if (!PlayerIsInLoadedWorld()) {
			g_stableFrames.store(0, std::memory_order_relaxed);
			g_sawPlayerOutOfWorld.store(true, std::memory_order_relaxed);
			return;
		}

		if (g_stableFrames.fetch_add(1, std::memory_order_relaxed) + 1 < 45)
			return;

		const ULONGLONG elapsed = GetTickCount64() - g_startedAt.load(std::memory_order_relaxed);
		if (!g_sawPlayerOutOfWorld.load(std::memory_order_relaxed) && elapsed < 5000)
			return;

		if (!g_active.exchange(false, std::memory_order_acq_rel))
			return;

		Log::Write("player world is ready; ending save-load overlay after %llu ms", elapsed);
		Progress::FinishSaveLoad();
		Watchdog::LoadFinished();
		g_stableFrames.store(0, std::memory_order_relaxed);
		g_sawPlayerOutOfWorld.store(false, std::memory_order_relaxed);
	}
}

namespace GameReady
{
	bool Start()
	{
		const SFSE::TaskInterface* tasks = SFSE::GetTaskInterface();
		if (!tasks) {
			Log::Write("SFSE task interface unavailable; cannot monitor player readiness");
			return false;
		}

		tasks->AddPermanentTask([] { Poll(); });
		Log::Write("player readiness monitor installed");
		return true;
	}

	void Begin(bool waitForPostLoadMessage)
	{
		g_stableFrames.store(0, std::memory_order_relaxed);
		g_sawPlayerOutOfWorld.store(false, std::memory_order_relaxed);
		g_waitForPostLoad.store(waitForPostLoadMessage, std::memory_order_relaxed);
		g_postLoadReceived.store(false, std::memory_order_relaxed);
		g_startedAt.store(GetTickCount64(), std::memory_order_relaxed);
		g_active.store(true, std::memory_order_release);
	}

	void PostLoadMessageReceived()
	{
		g_postLoadReceived.store(true, std::memory_order_release);
	}
}

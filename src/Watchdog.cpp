#include "Watchdog.h"
#include "FileTrace.h"
#include "Log.h"
#include "Stacks.h"

#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <tlhelp32.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	Config g_cfg;

	// Written by game threads (SFSE messages), read by the watchdog thread.
	std::atomic<bool>      g_loadActive{ false };
	std::atomic<ULONGLONG> g_loadStartTick{ 0 };
	std::atomic<ULONGLONG> g_sampleUntilTick{ 0 };   // keep sampling until this time (after a load, after the hotkey)
	std::atomic<bool>      g_newEpisode{ false };    // reset the per-load dump counter
	std::atomic<bool>      g_wantModuleList{ false };

	// Only touched by the watchdog thread.
	HWND  g_gameWindow   = nullptr;
	DWORD g_windowThread = 0;
	std::unordered_map<DWORD, ULONGLONG> g_lastThreadTime;  // thread id -> CPU time used so far (100 ns units)

	const double kGB = 1024.0 * 1024.0 * 1024.0;

	ULONGLONG ToTicks(const FILETIME& ft)
	{
		return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	}

	// ------------------------------------------------------------------ game window

	struct FindArgs { HWND found; };

	BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
			return TRUE;
		RECT r{};
		GetWindowRect(hwnd, &r);
		if (r.right - r.left >= 640 && r.bottom - r.top >= 360) {
			reinterpret_cast<FindArgs*>(lp)->found = hwnd;
			return FALSE;
		}
		return TRUE;
	}

	bool FindGameWindow()
	{
		FindArgs args{ nullptr };
		EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&args));
		if (!args.found)
			return false;
		g_gameWindow   = args.found;
		g_windowThread = GetWindowThreadProcessId(g_gameWindow, nullptr);
		return true;
	}

	// ------------------------------------------------------------------ per-thread CPU

	struct ThreadCpu
	{
		DWORD  id;
		double percent;  // of one CPU core, over the last interval
	};

	// Lists the process's threads with the CPU each used since the previous call.
	void SampleThreads(double seconds, std::vector<ThreadCpu>& out)
	{
		out.clear();
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
			return;

		std::unordered_map<DWORD, ULONGLONG> current;
		const DWORD pid = GetCurrentProcessId();
		THREADENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
			if (entry.th32OwnerProcessID != pid)
				continue;
			HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
			if (!thread)
				continue;
			FILETIME created, exited, kernel, user;
			if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
				const ULONGLONG total = ToTicks(kernel) + ToTicks(user);
				current[entry.th32ThreadID] = total;
				const auto previous = g_lastThreadTime.find(entry.th32ThreadID);
				const ULONGLONG delta = (previous != g_lastThreadTime.end() && total >= previous->second)
					? total - previous->second : 0;
				out.push_back({ entry.th32ThreadID, seconds > 0 ? double(delta) / 1e7 / seconds * 100.0 : 0.0 });
			}
			CloseHandle(thread);
		}
		CloseHandle(snapshot);

		g_lastThreadTime.swap(current);
		std::sort(out.begin(), out.end(), [](const ThreadCpu& a, const ThreadCpu& b) { return a.percent > b.percent; });
	}

	// The name the game gave a thread (e.g. "MainThread"), if any.
	std::string ThreadName(DWORD threadId)
	{
		using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
		static const auto getDescription = reinterpret_cast<GetThreadDescription_t>(reinterpret_cast<void*>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));  // Windows 10 1607+
		if (!getDescription)
			return {};

		std::string name;
		if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId)) {
			PWSTR wide = nullptr;
			if (SUCCEEDED(getDescription(thread, &wide)) && wide) {
				name = Log::Narrow(wide);
				LocalFree(wide);
			}
			CloseHandle(thread);
		}
		return name;
	}

	// ------------------------------------------------------------------ whole-process numbers

	struct ProcessNumbers
	{
		double cpuCores    = 0;  // 1.0 = one CPU core fully busy
		double readMBps    = 0;
		double writeMBps   = 0;
		double workingGB   = 0;  // RAM the game holds right now
		double commitGB    = 0;  // memory the game has reserved (RAM + page file)
		int    systemRamPercent  = 0;
		double systemCommitFreeGB = 0;  // what Windows can still hand out before "out of memory"
	};

	ProcessNumbers SampleProcess(double seconds)
	{
		static ULONGLONG lastCpu = 0, lastRead = 0, lastWrite = 0;
		static bool      first   = true;

		ProcessNumbers n;
		const HANDLE self = GetCurrentProcess();

		FILETIME created, exited, kernel, user;
		ULONGLONG cpu = lastCpu;
		if (GetProcessTimes(self, &created, &exited, &kernel, &user))
			cpu = ToTicks(kernel) + ToTicks(user);

		IO_COUNTERS io{};
		GetProcessIoCounters(self, &io);

		if (!first && seconds > 0) {
			n.cpuCores  = double(cpu - lastCpu) / 1e7 / seconds;
			n.readMBps  = double(io.ReadTransferCount - lastRead) / (1024.0 * 1024.0) / seconds;
			n.writeMBps = double(io.WriteTransferCount - lastWrite) / (1024.0 * 1024.0) / seconds;
		}
		lastCpu   = cpu;
		lastRead  = io.ReadTransferCount;
		lastWrite = io.WriteTransferCount;
		first     = false;

		PROCESS_MEMORY_COUNTERS_EX memory{};
		memory.cb = sizeof(memory);
		if (GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
			n.workingGB = double(memory.WorkingSetSize) / kGB;
			n.commitGB  = double(memory.PrivateUsage) / kGB;
		}

		MEMORYSTATUSEX system{};
		system.dwLength = sizeof(system);
		if (GlobalMemoryStatusEx(&system)) {
			n.systemRamPercent   = static_cast<int>(system.dwMemoryLoad);
			n.systemCommitFreeGB = double(system.ullAvailPageFile) / kGB;
		}
		return n;
	}

	// ------------------------------------------------------------------ video memory (VRAM)

	class VideoMemory
	{
	public:
		void Init()
		{
			IDXGIFactory1* factory = nullptr;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
				Log::Write("video memory: could not ask Windows (DXGI unavailable)");
				return;
			}
			for (UINT i = 0; count_ < kMaxAdapters; ++i) {
				IDXGIAdapter1* adapter = nullptr;
				if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter)
					break;
				DXGI_ADAPTER_DESC1 desc{};
				adapter->GetDesc1(&desc);
				IDXGIAdapter3* adapter3 = nullptr;
				if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) && adapter3) {
					adapters_[count_++] = adapter3;
					Log::Write("graphics card %d: %s, %.1f GB video memory", count_,
					           Log::Narrow(desc.Description).c_str(), double(desc.DedicatedVideoMemory) / kGB);
				}
				adapter->Release();
			}
			factory->Release();
		}

		// What this game uses on the card it uses most, and how much Windows lets it have.
		bool Query(double& usedGB, double& budgetGB) const
		{
			bool   any  = false;
			UINT64 used = 0, budget = 0;
			for (int i = 0; i < count_; ++i) {
				DXGI_QUERY_VIDEO_MEMORY_INFO info{};
				if (FAILED(adapters_[i]->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
					continue;
				if (!any || info.CurrentUsage > used) {
					used   = info.CurrentUsage;
					budget = info.Budget;
				}
				any = true;
			}
			usedGB   = double(used) / kGB;
			budgetGB = double(budget) / kGB;
			return any;
		}

	private:
		static constexpr int kMaxAdapters = 4;
		IDXGIAdapter3* adapters_[kMaxAdapters]{};
		int            count_ = 0;
	};

	VideoMemory g_video;

	// ------------------------------------------------------------------ module list

	// Every DLL loaded into the game: tells us which mods and overlays are in play.
	void LogModules()
	{
		std::vector<HMODULE> modules(1024);
		DWORD bytes = 0;
		if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes))
			return;
		const size_t count = std::min<size_t>(bytes / sizeof(HMODULE), modules.size());
		Log::Write("--- %zu DLLs loaded in the game ---", count);
		for (size_t i = 0; i < count; ++i) {
			wchar_t path[MAX_PATH]{};
			if (GetModuleFileNameW(modules[i], path, MAX_PATH))
				Log::Write("  dll %s", Log::Narrow(path).c_str());
		}
		Log::Write("--- end of DLL list ---");
	}

	// ------------------------------------------------------------------ stack dumps

	bool SameStack(const Stacks::Captured& a, const Stacks::Captured& b)
	{
		return a.count == b.count && std::memcmp(a.frames, b.frames, sizeof(DWORD64) * a.count) == 0;
	}

	bool OnlySystemPlumbing(const Stacks::Captured& c)
	{
		for (int i = 0; i < c.count; ++i)
			if (!Stacks::IsSystemPlumbing(c.frames[i]))
				return false;
		return true;
	}

	int g_dumpNumber     = 0;
	int g_dumpsInEpisode = 0;

	void Dump(const char* reason, const std::vector<ThreadCpu>& threads, double totalCpuCores)
	{
		// If nothing is using the CPU, the game is waiting for something (a lock, the disk,
		// the graphics driver). Then every thread is interesting, not just busy ones.
		const bool everyone = totalCpuCores < 0.05;
		const DWORD self = GetCurrentThreadId();

		std::vector<ThreadCpu> chosen;
		for (const ThreadCpu& t : threads)
			if (t.id != self && t.id == g_windowThread)
				chosen.push_back(t);  // the window's thread always goes first
		for (const ThreadCpu& t : threads) {
			if (t.id == self || t.id == g_windowThread)
				continue;
			if (everyone ? chosen.size() < 256 : (t.percent >= 5.0 && chosen.size() < 12))
				chosen.push_back(t);
		}

		// All memory is set aside BEFORE any thread gets paused (see Stacks.cpp).
		std::vector<Stacks::Captured> captured(chosen.size());
		for (size_t i = 0; i < chosen.size(); ++i)
			Stacks::Capture(chosen[i].id, g_cfg.stackDepth, captured[i]);

		++g_dumpNumber;
		++g_dumpsInEpisode;
		Log::Write("--- stack dump #%d (%s): %s ---", g_dumpNumber, reason,
		           everyone ? "CPU is idle, so showing all threads" : "showing the window thread and busy threads");

		std::vector<bool> printed(chosen.size(), false);
		int idleSystemThreads = 0;
		for (size_t i = 0; i < chosen.size(); ++i) {
			if (printed[i])
				continue;
			const Stacks::Captured& stack = captured[i];
			const bool isWindowThread = chosen[i].id == g_windowThread;

			if (stack.count == 0) {
				if (isWindowThread)
					Log::Write("thread %lu [window thread]: could not read its stack", chosen[i].id);
				continue;
			}
			// Idle Windows worker threads all look the same and tell us nothing.
			if (everyone && !isWindowThread && OnlySystemPlumbing(stack)) {
				++idleSystemThreads;
				continue;
			}

			// Group threads that sit in exactly the same place.
			std::string who;
			int sharing = 0;
			for (size_t j = i; j < chosen.size(); ++j) {
				if (printed[j] || captured[j].count == 0 || !SameStack(stack, captured[j]))
					continue;
				printed[j] = true;
				if (++sharing > 6)
					continue;
				char one[160];
				const std::string name = ThreadName(chosen[j].id);
				snprintf(one, sizeof(one), "%s%lu%s%s%s (cpu %.0f%%)", who.empty() ? "" : ", ", chosen[j].id,
				         name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"", chosen[j].percent);
				who += one;
			}
			if (sharing > 6)
				who += " and " + std::to_string(sharing - 6) + " more";

			Log::Write("thread %s%s%s", who.c_str(), isWindowThread ? "  [window thread]" : "",
			           sharing > 1 ? "  -- all at the same place:" : "");
			for (int f = 0; f < stack.count; ++f) {
				char text[400];
				Stacks::Describe(stack.frames[f], text, sizeof(text));
				Log::Write("    %2d  %s", f, text);
			}
		}
		if (idleSystemThreads > 0)
			Log::Write("(%d idle Windows worker threads not shown)", idleSystemThreads);
		Log::Write("--- end of dump #%d ---", g_dumpNumber);
	}

	// ------------------------------------------------------------------ look-back
	// Once a second (outside of loads) we quietly note where the window thread is, plus the
	// busiest other thread if one is working hard. When a save load starts, the last few
	// seconds are written out: that covers the wait between clicking "Load" and the loading screen.

	struct Moment
	{
		ULONGLONG        tick = 0;
		double           windowPercent = 0, otherPercent = 0;
		Stacks::Captured window, other;
	};

	std::vector<Moment> g_moments;       // ring buffer, one entry per second
	size_t              g_nextMoment = 0;

	void RememberMoment(const std::vector<ThreadCpu>& threads, ULONGLONG now)
	{
		if (g_moments.empty())
			return;
		Moment& m    = g_moments[g_nextMoment];
		g_nextMoment = (g_nextMoment + 1) % g_moments.size();

		m.tick          = now;
		m.windowPercent = m.otherPercent = 0;
		m.window.count  = m.other.count = 0;

		const DWORD self = GetCurrentThreadId();
		DWORD other = 0;
		for (const ThreadCpu& t : threads) {  // sorted, busiest first
			if (t.id == g_windowThread) {
				m.windowPercent = t.percent;
			} else if (other == 0 && t.id != self && t.percent >= 50.0) {
				other          = t.id;
				m.otherPercent = t.percent;
			}
		}
		if (g_windowThread)
			Stacks::Capture(g_windowThread, g_cfg.stackDepth, m.window);
		if (other)
			Stacks::Capture(other, g_cfg.stackDepth, m.other);
	}

	void WriteFrames(const Stacks::Captured& stack)
	{
		for (int f = 0; f < stack.count; ++f) {
			char text[400];
			Stacks::Describe(stack.frames[f], text, sizeof(text));
			Log::Write("    %2d  %s", f, text);
		}
	}

	void WriteLookBack(ULONGLONG loadStart)
	{
		const size_t n = g_moments.size();
		const Stacks::Captured* lastWindow = nullptr;
		const Stacks::Captured* lastOther  = nullptr;
		bool any = false;
		for (size_t i = 0; i < n; ++i) {
			Moment& m = g_moments[(g_nextMoment + i) % n];  // oldest first
			if (m.tick == 0 || m.tick > loadStart || loadStart - m.tick > ULONGLONG(n + 2) * 1000)
				continue;
			if (!any)
				Log::Write("--- look-back: the %zu s BEFORE the load started (oldest first) ---", n);
			any = true;

			const double before = double(loadStart - m.tick) / 1000.0;
			if (m.window.count > 0) {
				const bool same = lastWindow && SameStack(*lastWindow, m.window);
				Log::Write("%.1f s before: window thread %lu (cpu %.0f%%)%s", before, m.window.threadId, m.windowPercent,
				           same ? "  -- same place as the moment before" : "");
				if (!same)
					WriteFrames(m.window);
				lastWindow = &m.window;
			}
			if (m.other.count > 0) {
				const bool same = lastOther && SameStack(*lastOther, m.other);
				const std::string name = ThreadName(m.other.threadId);
				Log::Write("%.1f s before: busiest other thread %lu%s%s%s (cpu %.0f%%)%s", before, m.other.threadId,
				           name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"", m.otherPercent,
				           same ? "  -- same place as the moment before" : "");
				if (!same)
					WriteFrames(m.other);
				lastOther = &m.other;
			}
		}
		if (any)
			Log::Write("--- end of look-back ---");
		for (Moment& m : g_moments)
			m.tick = 0;
	}

	// ------------------------------------------------------------------ the watchdog loop

	bool HotkeyDown()
	{
		// Ctrl + Shift + F9
		return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
		       (GetAsyncKeyState(VK_F9) & 0x8000);
	}

	DWORD WINAPI ThreadMain(LPVOID)
	{
		FileTrace::IgnoreThread(GetCurrentThreadId());

		const ULONGLONG stallMs  = ULONGLONG(g_cfg.stallSeconds) * 1000;
		const ULONGLONG sampleMs = ULONGLONG(g_cfg.sampleSeconds) * 1000;

		std::vector<ThreadCpu> threads;
		SampleThreads(0, threads);  // first call only records a baseline
		SampleProcess(0);

		ULONGLONG lastPulse = GetTickCount64(), lastDump = 0, unresponsiveSince = 0;
		bool stalled = false, hotkeyWasDown = false, limitAnnounced = false;
		double cpuCores = 0;

		for (;;) {
			Sleep(200);
			ULONGLONG now = GetTickCount64();

			// 1. Find the game's window (once it exists).
			if (!g_gameWindow || !IsWindow(g_gameWindow)) {
				g_gameWindow = nullptr;
				if (FindGameWindow()) {
					Log::Write("game window found; it belongs to thread %lu", g_windowThread);
					g_video.Init();
					LogModules();
				}
			}

			// 2. Does the window still answer? This is what Windows itself uses for "Not Responding".
			bool responsive = true;
			if (g_gameWindow) {
				DWORD_PTR ignored = 0;
				responsive = SendMessageTimeoutW(g_gameWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 200, &ignored) != 0;
				now = GetTickCount64();
			}
			if (!responsive) {
				if (unresponsiveSince == 0)
					unresponsiveSince = now;
			} else {
				if (stalled)
					Log::Write("=== GAME WINDOW ANSWERS AGAIN after %.1f s ===", double(now - unresponsiveSince) / 1000.0);
				stalled           = false;
				unresponsiveSince = 0;
			}

			// 3. Things other threads asked for.
			if (g_newEpisode.exchange(false)) {
				WriteLookBack(g_loadStartTick.load());
				g_dumpsInEpisode = 0;
				limitAnnounced   = false;
				lastDump         = 0;
			}
			if (g_wantModuleList.exchange(false))
				LogModules();

			const bool hotkeyDown    = HotkeyDown();
			const bool hotkeyPressed = hotkeyDown && !hotkeyWasDown;
			hotkeyWasDown = hotkeyDown;
			if (hotkeyPressed) {
				Log::Write("=== MARK: you pressed Ctrl+Shift+F9 ===");
				g_sampleUntilTick = now + 60000;
				g_dumpsInEpisode  = 0;
				limitAnnounced    = false;
			}

			// 4. Once a second: the pulse line.
			bool pulsed = false;
			if (now - lastPulse >= 1000) {
				const double seconds = double(now - lastPulse) / 1000.0;
				lastPulse = now;
				pulsed    = true;

				SampleThreads(seconds, threads);
				const ProcessNumbers n = SampleProcess(seconds);
				cpuCores = n.cpuCores;
				if (!g_loadActive && !stalled)
					RememberMoment(threads, now);

				char video[64] = "";
				double usedGB = 0, budgetGB = 0;
				if (g_video.Query(usedGB, budgetGB))
					snprintf(video, sizeof(video), " vram=%.1f/%.1fGB", usedGB, budgetGB);

				char top[200] = "";
				size_t at = 0;
				for (size_t i = 0; i < threads.size() && i < 3 && threads[i].percent >= 1.0; ++i) {
					const int written = snprintf(top + at, sizeof(top) - at, " T%lu=%.0f%%", threads[i].id, threads[i].percent);
					if (written < 0 || size_t(written) >= sizeof(top) - at)
						break;
					at += size_t(written);
				}

				Log::Write("pulse cpu=%.2f cores  disk read=%.1f write=%.1f MB/s  files=%u  ram=%.1fGB commit=%.1fGB  "
				           "pc ram=%d%% commit free=%.1fGB%s  window=%s%s%s%s",
				           n.cpuCores, n.readMBps, n.writeMBps, FileTrace::TakeOpenCount(), n.workingGB, n.commitGB,
				           n.systemRamPercent, n.systemCommitFreeGB, video,
				           !g_gameWindow ? "none" : responsive ? "ok" : "NOT ANSWERING",
				           g_loadActive ? "  [loading save]" : "", top[0] ? "  busiest:" : "", top);
			}

			// 5. Should we write down what the threads are doing?
			const char* reason = nullptr;
			if (hotkeyPressed) {
				reason = "hotkey";
			} else if (!stalled && unresponsiveSince != 0 && now - unresponsiveSince >= stallMs) {
				stalled = true;
				Log::Write("=== STALL: the game window has not answered for %.1f s ===", double(now - unresponsiveSince) / 1000.0);
				reason = "window stopped answering";
			} else if (pulsed) {
				// Sample often at first, then every 30 s so a very long freeze does not flood the log.
				const ULONGLONG interval = g_dumpsInEpisode < 12 ? sampleMs : 30000;
				if (now - lastDump >= interval) {
					if (stalled)
						reason = "window still not answering";
					else if (g_loadActive)
						reason = "save is loading";
					else if (now < g_sampleUntilTick)
						reason = "follow-up sample";
				}
			}

			if (reason) {
				if (g_dumpsInEpisode < g_cfg.maxDumps) {
					Dump(reason, threads, cpuCores);
					lastDump = GetTickCount64();
				} else if (!limitAnnounced) {
					Log::Write("stack dump limit reached (MaxDumps); pulse lines continue");
					limitAnnounced = true;
				}
			}
		}
	}
}

namespace Watchdog
{
	void Start(const Config& config)
	{
		g_cfg = config;
		g_moments.resize(static_cast<size_t>(g_cfg.lookBackSeconds));
		if (HANDLE thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr))
			CloseHandle(thread);
		else
			Log::Write("could not start the watchdog thread");
	}

	void LoadStarted(const char* saveName)
	{
		if (g_loadActive.exchange(true))
			return;
		static std::atomic<bool> first{ true };
		Log::Write("=== SAVE LOAD STARTED: %s ===", saveName && saveName[0] ? saveName : "(no name given)");
		g_loadStartTick = GetTickCount64();
		g_newEpisode    = true;
		if (first.exchange(false))
			g_wantModuleList = true;  // by now every mod's DLL is loaded
	}

	void LoadFinished()
	{
		if (!g_loadActive.exchange(false))
			return;
		const ULONGLONG now = GetTickCount64();
		Log::Write("=== SAVE LOAD FINISHED after %.1f s (the game's load function returned; "
		           "the loading screen may go on while the world streams in) ===",
		           double(now - g_loadStartTick.load()) / 1000.0);
		g_sampleUntilTick = now + 60000;  // keep watching for another minute
	}
}

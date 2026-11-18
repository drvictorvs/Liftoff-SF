#include "FileTrace.h"
#include "GameReady.h"
#include "Log.h"
#include "Progress.h"
#include "Watchdog.h"

#include <windows.h>
#include <winternl.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>

namespace
{
	using NtCreateFile_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
	                                        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
	using NtOpenFile_t   = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
	using ShutdownCheck_t = BOOLEAN(NTAPI*)();

	NtCreateFile_t  g_originalCreate = nullptr;
	NtOpenFile_t    g_originalOpen   = nullptr;
	ShutdownCheck_t g_shuttingDown   = nullptr;  // ntdll's RtlDllShutdownInProgress

	Config                 g_cfg;
	LARGE_INTEGER          g_frequency{};
	std::atomic<unsigned>  g_openCount{ 0 };
	std::atomic<int>       g_lines{ 0 };
	std::atomic<unsigned long> g_ignoredThread{ 0 };
	std::atomic<ULONGLONG> g_nextActivityReportTick{ 0 };

	// Stops infinite loops: writing the log line itself must not be traced.
	thread_local bool t_busy = false;

	constexpr ULONG kDirectoryFile = 0x00000001;  // FILE_DIRECTORY_FILE
	constexpr ACCESS_MASK kWriteAccess = GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA;

	void Record(POBJECT_ATTRIBUTES attributes, NTSTATUS status, const LARGE_INTEGER& started,
	            ACCESS_MASK access, ULONG options)
	{
		if (t_busy)
			return;
		if (status < 0 && !g_cfg.logFailedOpens)
			return;
		if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer)
			return;

		// Kernel-style path:  \??\C:\Games\Starfield\Data\Starfield.esm
		// Anything not on a drive letter (pipes, devices) is skipped.
		const wchar_t* name = attributes->ObjectName->Buffer;
		const int length    = attributes->ObjectName->Length / 2;
		if (length < 7 || name[0] != L'\\' || name[1] != L'?' || name[2] != L'?' || name[3] != L'\\' || name[5] != L':')
			return;
		if (GetCurrentThreadId() == g_ignoredThread.load(std::memory_order_relaxed))
			return;

		t_busy = true;
		const DWORD lastError = GetLastError();  // our work below must not change what the game sees

		const wchar_t* fullPath = name + 4;
		const wchar_t* pathEnd = fullPath + length - 4;
		const wchar_t* baseName = fullPath;
		const wchar_t* dot = nullptr;
		for (const wchar_t* p = fullPath; p < pathEnd; ++p) {
			if (*p == L'\\' || *p == L'/') {
				baseName = p + 1;
				dot = nullptr;
			} else if (*p == L'.') {
				dot = p;
			}
		}
		std::wstring extension;
		if (dot) {
			for (const wchar_t* p = dot; p < pathEnd; ++p)
				extension.push_back(static_cast<wchar_t>(std::towlower(*p)));
		}

		if (status >= 0 && extension == L".sfs" && !(access & kWriteAccess)) {
			const std::wstring saveName(baseName, pathEnd);
			if (Progress::BeginSaveLoadFromFile(saveName)) {
				const std::string narrowName = Log::Narrow(saveName);
				Watchdog::LoadStarted(narrowName.c_str());
				GameReady::Begin(false);
			}
		}

		const bool trackedFile = extension == L".sfs" ||
			std::find(g_cfg.trackedExtensions.begin(), g_cfg.trackedExtensions.end(), extension) != g_cfg.trackedExtensions.end();
		if (status >= 0 && trackedFile) {
			const ULONGLONG now = GetTickCount64();
			ULONGLONG nextReport = g_nextActivityReportTick.load(std::memory_order_relaxed);
			if (now >= nextReport &&
			    g_nextActivityReportTick.compare_exchange_strong(nextReport, now + 100, std::memory_order_relaxed)) {
				std::wstring activity;
				if (extension == L".esm" || extension == L".esp" || extension == L".esl")
					activity = L"Loading plugin  ";
				else if (extension == L".ba2")
					activity = L"Opening archive  ";
				else if (extension == L".sfs")
					activity = L"Reading save  ";
				else
					activity = L"Reading  ";
				activity.append(baseName, pathEnd);
				Progress::ReportFileActivity(activity);
			}
		}

		LARGE_INTEGER finished;
		QueryPerformanceCounter(&finished);
		const double ms = double(finished.QuadPart - started.QuadPart) * 1000.0 / double(g_frequency.QuadPart);

		g_openCount.fetch_add(1, std::memory_order_relaxed);

		if (ms >= g_cfg.minOpenMs) {
			const int line = g_lines.fetch_add(1, std::memory_order_relaxed);
			if (line < g_cfg.maxFileLines) {
				char path[700];
				const int n = WideCharToMultiByte(CP_UTF8, 0, name + 4, length - 4, path, sizeof(path) - 1, nullptr, nullptr);
				path[n > 0 ? n : 0] = '\0';
				Log::Write("file %8.1f ms  T%-6lu %s%s%s%s", ms, GetCurrentThreadId(),
				           (options & kDirectoryFile) ? "[folder] " : "",
				           (access & kWriteAccess) ? "[write] " : "",
				           path,
				           status < 0 ? "   (not found / failed)" : "");
			} else if (line == g_cfg.maxFileLines) {
				Log::Write("file line limit reached (MaxFileLines); no more files will be listed");
			}
		}

		SetLastError(lastError);
		t_busy = false;
	}

	inline bool ProcessIsExiting()
	{
		// While the process shuts down, other threads are killed mid-work and may
		// never release the log's lock. Stay completely out of the way then.
		return g_shuttingDown && g_shuttingDown();
	}

	NTSTATUS NTAPI Hook_NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
	                                 PIO_STATUS_BLOCK ioStatus, PLARGE_INTEGER allocationSize, ULONG fileAttributes,
	                                 ULONG shareAccess, ULONG disposition, ULONG options, PVOID eaBuffer, ULONG eaLength)
	{
		LARGE_INTEGER started;
		QueryPerformanceCounter(&started);
		const NTSTATUS status = g_originalCreate(handle, access, attributes, ioStatus, allocationSize, fileAttributes,
		                                         shareAccess, disposition, options, eaBuffer, eaLength);
		if (!ProcessIsExiting())
			Record(attributes, status, started, access, options);
		return status;
	}

	NTSTATUS NTAPI Hook_NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
	                               PIO_STATUS_BLOCK ioStatus, ULONG shareAccess, ULONG options)
	{
		LARGE_INTEGER started;
		QueryPerformanceCounter(&started);
		const NTSTATUS status = g_originalOpen(handle, access, attributes, ioStatus, shareAccess, options);
		if (!ProcessIsExiting())
			Record(attributes, status, started, access, options);
		return status;
	}
}

namespace FileTrace
{
	bool Install(const Config& config)
	{
		g_cfg = config;
		if (!g_cfg.logFiles) {
			Log::Write("file tracing is off (LogFiles=0)");
			return false;
		}

		QueryPerformanceFrequency(&g_frequency);
		if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
			g_shuttingDown = reinterpret_cast<ShutdownCheck_t>(
				reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlDllShutdownInProgress")));

		const MH_STATUS initialize = MH_Initialize();
		if (initialize != MH_OK && initialize != MH_ERROR_ALREADY_INITIALIZED) {
			Log::Write("MinHook failed to initialise; file tracing is off");
			return false;
		}

		int hooked = 0;
		if (MH_CreateHookApi(L"ntdll.dll", "NtCreateFile", reinterpret_cast<LPVOID>(&Hook_NtCreateFile),
		                     reinterpret_cast<LPVOID*>(&g_originalCreate)) == MH_OK)
			++hooked;
		else
			Log::Write("could not hook NtCreateFile");

		if (MH_CreateHookApi(L"ntdll.dll", "NtOpenFile", reinterpret_cast<LPVOID>(&Hook_NtOpenFile),
		                     reinterpret_cast<LPVOID*>(&g_originalOpen)) == MH_OK)
			++hooked;
		else
			Log::Write("could not hook NtOpenFile");

		if (hooked == 0 || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
			Log::Write("file tracing is off (no hooks active)");
			return false;
		}
		Log::Write("file tracing on (%d of 2 hooks)", hooked);
		return true;
	}

	unsigned TakeOpenCount()
	{
		return g_openCount.exchange(0, std::memory_order_relaxed);
	}

	void IgnoreThread(unsigned long threadId)
	{
		g_ignoredThread.store(threadId, std::memory_order_relaxed);
	}
}

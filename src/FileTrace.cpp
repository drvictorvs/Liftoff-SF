#include "FileTrace.h"
#include "GameReady.h"
#include "Log.h"
#include "Progress.h"
#include "Watchdog.h"

#include <windows.h>
#include <winternl.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

namespace
{
	using NtCreateFile_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
	                                        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
	using NtOpenFile_t   = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
	using NtReadFile_t   = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
	                                        PLARGE_INTEGER, PULONG);
	using NtClose_t      = NTSTATUS(NTAPI*)(HANDLE);
	using ShutdownCheck_t = BOOLEAN(NTAPI*)();

	NtCreateFile_t  g_originalCreate = nullptr;
	NtOpenFile_t    g_originalOpen   = nullptr;
	NtReadFile_t    g_originalRead   = nullptr;
	NtClose_t       g_originalClose  = nullptr;
	void*           g_readTarget     = nullptr;
	void*           g_closeTarget    = nullptr;
	ShutdownCheck_t g_shuttingDown   = nullptr;  // ntdll's RtlDllShutdownInProgress

	Config                 g_cfg;
	LARGE_INTEGER          g_frequency{};
	std::atomic<unsigned>  g_openCount{ 0 };
	std::atomic<int>       g_lines{ 0 };
	std::atomic<unsigned long> g_ignoredThread{ 0 };
	std::atomic<ULONGLONG> g_nextActivityReportTick{ 0 };

	// Stops infinite loops: writing the log line itself must not be traced.
	thread_local bool t_busy = false;
	// Set while Liftoff itself reads files (the overlay's font), so they don't count as game loading.
	thread_local bool t_quiet = false;

	constexpr ULONG kDirectoryFile = 0x00000001;  // FILE_DIRECTORY_FILE
	constexpr ACCESS_MASK kWriteAccess = GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA;
	constexpr NTSTATUS kPending = 0x00000103;  // STATUS_PENDING: an asynchronous read was started

	// --- Time spent on each file during startup, for the summary --------------------------------
	//
	// Opens are added per path. Reads are matched to a path through the handle the open returned.
	// The read and close hooks never take a lock: hooking libraries (MinHook, in Liftoff and in other
	// plugins) close handles while every other thread is suspended, and a suspended thread could be
	// holding one.
	struct FileStat
	{
		std::wstring               path;  // written once, before the file's index is published
		std::atomic<std::uint64_t> opens{ 0 }, openTicks{ 0 };
		std::atomic<std::uint64_t> reads{ 0 }, readTicks{ 0 }, bytes{ 0 }, pendingReads{ 0 };
	};

	constexpr std::size_t   kChunkBits = 12;
	constexpr std::size_t   kChunkSize = std::size_t(1) << kChunkBits;
	constexpr std::size_t   kMaxChunks = 256;                 // up to about a million files
	constexpr std::size_t   kHandleSlots = std::size_t(1) << 18;  // handle values below 1 M
	constexpr std::uint32_t kNoFile = 0xFFFFFFFF;

	std::atomic<bool>                                   g_collect{ false };
	std::array<std::atomic<FileStat*>, kMaxChunks>      g_chunks{};
	std::mutex                                          g_pathLock;  // only taken when a file is opened
	std::unordered_map<std::wstring, std::uint32_t>     g_pathIndex;
	std::uint32_t                                       g_fileCount = 0;
	// One slot per handle value: (handle << 32) | (file index + 1), or 0. Handles are multiples of 4.
	// Never freed (nor are the chunks): the read and close hooks can still run while the process
	// exits, after this DLL's globals have been destroyed.
	std::atomic<std::uint64_t>*                         g_handles = nullptr;

	FileStat& FileAt(std::uint32_t index)
	{
		return g_chunks[index >> kChunkBits].load(std::memory_order_acquire)[index & (kChunkSize - 1)];
	}

	std::atomic<std::uint64_t>* HandleSlot(HANDLE handle, std::uint64_t& value)
	{
		value = reinterpret_cast<std::uintptr_t>(handle);
		if (!value || (value >> 32) || !g_handles)
			return nullptr;
		return &g_handles[(value >> 2) & (kHandleSlots - 1)];
	}

	void MapHandle(HANDLE handle, std::uint32_t index)
	{
		std::uint64_t value = 0;
		if (auto* slot = HandleSlot(handle, value))
			slot->store((value << 32) | (std::uint64_t(index) + 1), std::memory_order_release);
	}

	void UnmapHandle(HANDLE handle)
	{
		std::uint64_t value = 0;
		auto* slot = HandleSlot(handle, value);
		if (!slot)
			return;
		std::uint64_t current = slot->load(std::memory_order_acquire);
		while ((current >> 32) == value && !slot->compare_exchange_weak(current, 0, std::memory_order_acq_rel))
			;
	}

	std::uint32_t FileOf(HANDLE handle)
	{
		std::uint64_t value = 0;
		auto* slot = HandleSlot(handle, value);
		if (!slot)
			return kNoFile;
		const std::uint64_t current = slot->load(std::memory_order_acquire);
		if ((current >> 32) != value || !(current & 0xFFFFFFFF))
			return kNoFile;
		return std::uint32_t(current & 0xFFFFFFFF) - 1;
	}

	bool IsDrivePath(POBJECT_ATTRIBUTES attributes)
	{
		// Kernel-style path:  \??\C:\Games\Starfield\Data\Starfield.esm
		if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer)
			return false;
		const wchar_t* name = attributes->ObjectName->Buffer;
		const int length    = attributes->ObjectName->Length / 2;
		return length >= 7 && name[0] == L'\\' && name[1] == L'?' && name[2] == L'?' && name[3] == L'\\' && name[5] == L':';
	}

	// A file was opened: count it under its path and remember which file the handle belongs to.
	void NoteOpen(HANDLE handle, POBJECT_ATTRIBUTES attributes, std::uint64_t ticks)
	{
		// Liftoff's own reads and anything that isn't a file on a drive only clear the handle's slot
		// (the value may have belonged to a file closed earlier).
		if (t_busy || t_quiet || GetCurrentThreadId() == g_ignoredThread.load(std::memory_order_relaxed) ||
		    !IsDrivePath(attributes)) {
			UnmapHandle(handle);
			return;
		}

		const wchar_t* name = attributes->ObjectName->Buffer;
		const int length    = attributes->ObjectName->Length / 2;
		std::wstring path(name + 4, name + length);
		std::wstring key(path);
		for (auto& c : key)
			c = static_cast<wchar_t>(std::towlower(c));

		std::uint32_t index = kNoFile;
		{
			std::lock_guard guard(g_pathLock);
			if (!g_collect.load(std::memory_order_relaxed))
				return;
			const auto found = g_pathIndex.find(key);
			if (found != g_pathIndex.end()) {
				index = found->second;
			} else if (g_fileCount < kMaxChunks * kChunkSize) {
				auto& chunk = g_chunks[g_fileCount >> kChunkBits];
				if (!chunk.load(std::memory_order_relaxed)) {
					FileStat* block = new (std::nothrow) FileStat[kChunkSize];
					if (!block)
						return;
					chunk.store(block, std::memory_order_release);
				}
				index = g_fileCount++;
				FileAt(index).path = std::move(path);
				g_pathIndex.emplace(std::move(key), index);
			}
			if (index != kNoFile) {
				FileStat& file = FileAt(index);
				file.opens.fetch_add(1, std::memory_order_relaxed);
				file.openTicks.fetch_add(ticks, std::memory_order_relaxed);
			}
		}
		if (index == kNoFile)
			UnmapHandle(handle);
		else
			MapHandle(handle, index);
	}

	void Record(POBJECT_ATTRIBUTES attributes, NTSTATUS status, const LARGE_INTEGER& started,
	            ACCESS_MASK access, ULONG options)
	{
		if (t_busy || t_quiet)
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

		if (g_cfg.logFiles && ms >= g_cfg.minOpenMs) {
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
		if (!ProcessIsExiting()) {
			if (status >= 0 && handle && g_collect.load(std::memory_order_relaxed)) {
				LARGE_INTEGER finished;
				QueryPerformanceCounter(&finished);
				NoteOpen(*handle, attributes, std::uint64_t(finished.QuadPart - started.QuadPart));
			}
			Record(attributes, status, started, access, options);
		}
		return status;
	}

	NTSTATUS NTAPI Hook_NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
	                               PIO_STATUS_BLOCK ioStatus, ULONG shareAccess, ULONG options)
	{
		LARGE_INTEGER started;
		QueryPerformanceCounter(&started);
		const NTSTATUS status = g_originalOpen(handle, access, attributes, ioStatus, shareAccess, options);
		if (!ProcessIsExiting()) {
			if (status >= 0 && handle && g_collect.load(std::memory_order_relaxed)) {
				LARGE_INTEGER finished;
				QueryPerformanceCounter(&finished);
				NoteOpen(*handle, attributes, std::uint64_t(finished.QuadPart - started.QuadPart));
			}
			Record(attributes, status, started, access, options);
		}
		return status;
	}

	// Only reads that finish inside the call are timed; asynchronous ones are just counted.
	NTSTATUS NTAPI Hook_NtReadFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID apcContext,
	                               PIO_STATUS_BLOCK ioStatus, PVOID buffer, ULONG length, PLARGE_INTEGER offset,
	                               PULONG key)
	{
		const std::uint32_t index = g_collect.load(std::memory_order_relaxed) ? FileOf(handle) : kNoFile;
		if (index == kNoFile)
			return g_originalRead(handle, event, apc, apcContext, ioStatus, buffer, length, offset, key);

		LARGE_INTEGER started;
		QueryPerformanceCounter(&started);
		const NTSTATUS status = g_originalRead(handle, event, apc, apcContext, ioStatus, buffer, length, offset, key);
		LARGE_INTEGER finished;
		QueryPerformanceCounter(&finished);

		FileStat& file = FileAt(index);
		if (status == kPending) {
			file.pendingReads.fetch_add(1, std::memory_order_relaxed);
		} else {
			file.reads.fetch_add(1, std::memory_order_relaxed);
			file.readTicks.fetch_add(std::uint64_t(finished.QuadPart - started.QuadPart), std::memory_order_relaxed);
			if (status >= 0 && ioStatus)
				file.bytes.fetch_add(ioStatus->Information, std::memory_order_relaxed);
		}
		return status;
	}

	NTSTATUS NTAPI Hook_NtClose(HANDLE handle)
	{
		if (g_collect.load(std::memory_order_relaxed))
			UnmapHandle(handle);  // before closing: once closed, another thread can get the same value
		return g_originalClose(handle);
	}
}

namespace FileTrace
{
	bool Install(const Config& config)
	{
		g_cfg = config;
		if (!g_cfg.logFiles && !g_cfg.startupSummary) {
			Log::Write("file tracing is off (LogFiles=0 and StartupSummary=0)");
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

		// Per-file read times for the startup summary; switched off again once it's written.
		bool perFile = false;
		if (hooked > 0 && g_cfg.startupSummary) {
			g_handles = new (std::nothrow) std::atomic<std::uint64_t>[kHandleSlots]();
			if (g_handles &&
			    MH_CreateHookApiEx(L"ntdll.dll", "NtReadFile", reinterpret_cast<LPVOID>(&Hook_NtReadFile),
			                       reinterpret_cast<LPVOID*>(&g_originalRead), &g_readTarget) == MH_OK) {
				if (MH_CreateHookApiEx(L"ntdll.dll", "NtClose", reinterpret_cast<LPVOID>(&Hook_NtClose),
				                       reinterpret_cast<LPVOID*>(&g_originalClose), &g_closeTarget) == MH_OK) {
					perFile = true;
				} else {
					MH_RemoveHook(g_readTarget);
					g_readTarget = nullptr;
				}
			}
			if (!perFile)
				Log::Write("could not hook NtReadFile/NtClose; the startup summary won't list file read times");
			g_collect.store(perFile, std::memory_order_release);
		}

		if (hooked == 0 || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
			g_collect.store(false);
			Log::Write("file tracing is off (no hooks active)");
			return false;
		}
		Log::Write("file tracing on (%d of 2 hooks)%s%s", hooked, g_cfg.logFiles ? "" : ", not logging each open",
		           perFile ? "; timing reads per file until the startup summary" : "");
		return true;
	}

	std::vector<FileReport> StopCollecting()
	{
		if (!g_collect.exchange(false))
			return {};

		// The hooks now pass straight through; take them out as well.
		if (g_readTarget)
			MH_QueueDisableHook(g_readTarget);
		if (g_closeTarget)
			MH_QueueDisableHook(g_closeTarget);
		MH_ApplyQueued();

		const double toMs = 1000.0 / double(g_frequency.QuadPart);
		std::vector<FileReport> files;
		std::lock_guard guard(g_pathLock);
		files.reserve(g_fileCount);
		for (std::uint32_t i = 0; i < g_fileCount; ++i) {
			FileStat& file = FileAt(i);
			FileReport report;
			report.path         = std::move(file.path);
			report.opens        = file.opens.load(std::memory_order_relaxed);
			report.reads        = file.reads.load(std::memory_order_relaxed);
			report.pendingReads = file.pendingReads.load(std::memory_order_relaxed);
			report.bytes        = file.bytes.load(std::memory_order_relaxed);
			report.openMs       = double(file.openTicks.load(std::memory_order_relaxed)) * toMs;
			report.readMs       = double(file.readTicks.load(std::memory_order_relaxed)) * toMs;
			files.push_back(std::move(report));
		}
		// The paths are the bulk of the memory; the small fixed tables stay, as a thread may still be
		// finishing a read that started before collection stopped.
		std::unordered_map<std::wstring, std::uint32_t>().swap(g_pathIndex);
		return files;
	}

	unsigned TakeOpenCount()
	{
		return g_openCount.exchange(0, std::memory_order_relaxed);
	}

	void IgnoreThread(unsigned long threadId)
	{
		g_ignoredThread.store(threadId, std::memory_order_relaxed);
	}

	QuietScope::QuietScope() { t_quiet = true; }
	QuietScope::~QuietScope() { t_quiet = false; }
}

#include "FileWatch.h"
#include "Log.h"
#include "Progress.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cwctype>

namespace
{
	// Function-pointer types matching the real Windows functions exactly.
	using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
	using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
	using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

	// MinHook fills these with "trampolines": calling them runs the ORIGINAL function.
	CreateFileW_t g_originalW = nullptr;
	CreateFileA_t g_originalA = nullptr;
	CreateFile2_t g_original2 = nullptr;

	std::vector<std::wstring> g_extensions;  // set once before hooks go live, read-only afterwards
	std::atomic<bool>         g_installed{ false };
	bool                      g_logAll    = false;

	// Stops infinite loops if anything we call below opens a file itself.
	thread_local bool t_busy = false;

	bool EndsWithTrackedExtension(const wchar_t* path, std::wstring& lowerExt)
	{
		const wchar_t* dot = wcsrchr(path, L'.');
		if (!dot)
			return false;
		lowerExt.clear();
		for (const wchar_t* p = dot; *p; ++p)
			lowerExt.push_back(static_cast<wchar_t>(std::towlower(*p)));
		for (const auto& ext : g_extensions)
			if (lowerExt == ext)
				return true;
		return false;
	}

	void Inspect(const wchar_t* path)
	{
		if (!path || t_busy)
			return;
		t_busy = true;

		if (g_logAll)
			Log::Write("open: %s", Log::Narrow(path).c_str());

		std::wstring ext;
		if (EndsWithTrackedExtension(path, ext)) {
			const wchar_t* slash1 = wcsrchr(path, L'\\');
			const wchar_t* slash2 = wcsrchr(path, L'/');
			const wchar_t* name   = path;
			if (slash1 && slash1 + 1 > name) name = slash1 + 1;
			if (slash2 && slash2 + 1 > name) name = slash2 + 1;

			std::wstring key = L"file:";
			for (const wchar_t* p = name; *p; ++p)
				key.push_back(static_cast<wchar_t>(std::towlower(*p)));

			std::wstring status;
			if (ext == L".esm" || ext == L".esp" || ext == L".esl")
				status = L"Loading plugin  ";
			else if (ext == L".ba2")
				status = L"Opening archive  ";
			else
				status = L"Reading  ";
			status += name;

			Progress::ReportFileActivity(status);
			Progress::Milestone(key, status);
		}

		t_busy = false;
	}

	// --- The hooks. Each one: call the real function, then take a look. ---
	// GetLastError() is saved and restored because the game may check it
	// right after CreateFile returns, and our code must not change it.

	HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
	                               DWORD disposition, DWORD flags, HANDLE templ)
	{
		HANDLE result = g_originalW(name, access, share, sa, disposition, flags, templ);
		const DWORD error = GetLastError();
		if (result != INVALID_HANDLE_VALUE)
			Inspect(name);
		SetLastError(error);
		return result;
	}

	HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
	                               DWORD disposition, DWORD flags, HANDLE templ)
	{
		HANDLE result = g_originalA(name, access, share, sa, disposition, flags, templ);
		const DWORD error = GetLastError();
		if (result != INVALID_HANDLE_VALUE && name && !t_busy) {
			wchar_t wide[MAX_PATH * 2];
			if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2) > 0)
				Inspect(wide);
		}
		SetLastError(error);
		return result;
	}

	HANDLE WINAPI Hook_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
	                               LPCREATEFILE2_EXTENDED_PARAMETERS params)
	{
		HANDLE result = g_original2(name, access, share, disposition, params);
		const DWORD error = GetLastError();
		if (result != INVALID_HANDLE_VALUE)
			Inspect(name);
		SetLastError(error);
		return result;
	}

	// Hook in kernelbase.dll (where the real code lives on Windows 8+),
	// falling back to kernel32.dll.
	bool HookApi(const char* function, LPVOID detour, LPVOID* original)
	{
		for (const wchar_t* module : { L"kernelbase.dll", L"kernel32.dll" }) {
			if (MH_CreateHookApi(module, function, detour, original) == MH_OK) {
				Log::Write("hooked %s in %s", function, Log::Narrow(module).c_str());
				return true;
			}
		}
		Log::Write("could not hook %s", function);
		return false;
	}
}

namespace FileWatch
{
	bool Install(const std::vector<std::wstring>& trackedExtensions, bool logAllFiles)
	{
		g_extensions = trackedExtensions;
		g_logAll     = logAllFiles;

		if (MH_Initialize() != MH_OK) {
			Log::Write("MinHook failed to initialise");
			return false;
		}

		int hooked = 0;
		hooked += HookApi("CreateFileW", reinterpret_cast<LPVOID>(&Hook_CreateFileW), reinterpret_cast<LPVOID*>(&g_originalW));
		hooked += HookApi("CreateFileA", reinterpret_cast<LPVOID>(&Hook_CreateFileA), reinterpret_cast<LPVOID*>(&g_originalA));
		hooked += HookApi("CreateFile2", reinterpret_cast<LPVOID>(&Hook_CreateFile2), reinterpret_cast<LPVOID*>(&g_original2));

		if (hooked == 0 || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
			Log::Write("no file hooks active; progress will be time-based only");
			return false;
		}
		g_installed = true;
		return true;
	}

	void Uninstall()
	{
		if (!g_installed.exchange(false))
			return;  // already off (both the timeout and "data loaded" can call this)
		// Disable, but don't MH_Uninitialize(): another thread could be inside
		// a hook right now, and freeing the trampolines under it would crash.
		MH_DisableHook(MH_ALL_HOOKS);
		Log::Write("file hooks disabled");
	}
}

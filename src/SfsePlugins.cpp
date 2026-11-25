#include "SfsePlugins.h"

#include "Log.h"
#include "Progress.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace
{
	using LoadFn         = bool (*)(const SFSEInterface*);
	using Listener       = SFSEMessagingInterface::EventCallback;
	using RegisterFn     = bool (*)(PluginHandle, const char*, Listener);
	using GetProcAddr_t  = FARPROC(WINAPI*)(HMODULE, LPCSTR);
	using LoadLibA_t     = HMODULE(WINAPI*)(LPCSTR);
	using LoadLibW_t     = HMODULE(WINAPI*)(LPCWSTR);
	using LoadLibExA_t   = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
	using LoadLibExW_t   = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

	constexpr std::size_t kKinds = SfsePlugins::kMessageKinds;
	constexpr std::size_t kOther = kKinds - 1;
	// Every plugin can have a preload and a load entry point; most register one or two listeners.
	constexpr std::size_t kLoadSlots     = 256;
	constexpr std::size_t kListenerSlots = 512;
	// SFSE also opens each plugin as a resource while scanning the folder; those aren't real loads.
	constexpr DWORD kResourceOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
	                                LOAD_LIBRARY_AS_IMAGE_RESOURCE;

	constexpr const char* kMessageNames[kKinds] = {
		"PostLoad", "PostPostLoad", "PostDataLoad", "PostPostDataLoad",
		"PreSaveGame", "PostSaveGame", "PreLoadGame", "PostLoadGame", "other messages",
	};

	struct Plugin : SfsePlugins::Report
	{
		HMODULE module = nullptr;
	};

	std::mutex          g_lock;     // guards the numbers in g_plugins (names never change once added)
	std::deque<Plugin>  g_plugins;  // a deque, so pointers to its elements stay valid as it grows
	HMODULE             g_self = nullptr;
	LARGE_INTEGER       g_frequency{};

	// The real functions, taken from SFSE's import table before it was redirected.
	GetProcAddr_t g_getProcAddress = nullptr;
	LoadLibA_t    g_loadLibraryA   = nullptr;
	LoadLibW_t    g_loadLibraryW   = nullptr;
	LoadLibExA_t  g_loadLibraryExA = nullptr;
	LoadLibExW_t  g_loadLibraryExW = nullptr;
	RegisterFn    g_register       = nullptr;

	// The plugin whose SFSEPlugin_Preload/Load is running on this thread, for listeners that it
	// registers with a handler that lives outside its own DLL.
	thread_local Plugin* t_current = nullptr;

	struct LoadSlot
	{
		LoadFn  original = nullptr;
		Plugin* plugin   = nullptr;
		bool    preload  = false;
	};
	struct ListenerSlot
	{
		Listener original = nullptr;
		Plugin*  plugin   = nullptr;
	};
	std::array<LoadSlot, kLoadSlots>         g_loadSlots{};
	std::array<ListenerSlot, kListenerSlots> g_listenerSlots{};
	std::atomic<std::size_t>                 g_usedLoadSlots{ 0 };
	std::atomic<std::size_t>                 g_usedListenerSlots{ 0 };

	LARGE_INTEGER Now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t;
	}

	double MsSince(const LARGE_INTEGER& started)
	{
		return double(Now().QuadPart - started.QuadPart) * 1000.0 / double(g_frequency.QuadPart);
	}

	std::wstring BaseName(std::wstring_view path)
	{
		const auto slash = path.find_last_of(L"\\/");
		return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
	}

	std::wstring Lower(std::wstring text)
	{
		for (auto& c : text)
			c = static_cast<wchar_t>(std::towlower(c));
		return text;
	}

	std::wstring AnsiToWide(const char* text)
	{
		const int size = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
		if (size <= 1)
			return {};
		std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
		MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), size);
		return out;
	}

	HMODULE ModuleOf(const void* address)
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        static_cast<LPCWSTR>(address), &module))
			return nullptr;
		return module;
	}

	Plugin* FindPlugin(HMODULE module)
	{
		std::lock_guard guard(g_lock);
		for (auto& plugin : g_plugins)
			if (plugin.module == module)
				return &plugin;
		return nullptr;
	}

	Plugin* PluginFor(HMODULE module)
	{
		if (Plugin* known = FindPlugin(module))
			return known;

		// Names are read outside the lock: both calls below take the Windows loader lock.
		wchar_t path[MAX_PATH * 2]{};
		GetModuleFileNameW(module, path, MAX_PATH * 2);
		std::string dll = Log::Narrow(BaseName(path));
		std::string name;
		const auto* version = reinterpret_cast<const SFSEPluginVersionData*>(
			reinterpret_cast<void*>(::GetProcAddress(module, "SFSEPlugin_Version")));
		if (version)
			name.assign(version->name, strnlen(version->name, sizeof(version->name)));

		std::lock_guard guard(g_lock);
		for (auto& plugin : g_plugins)
			if (plugin.module == module)
				return &plugin;
		Plugin& added = g_plugins.emplace_back();
		added.module = module;
		added.dll    = dll.empty() ? "(unknown DLL)" : std::move(dll);
		added.name   = std::move(name);
		return &added;
	}

	// --- Plugin entry points ---------------------------------------------------------------------

	bool RunLoad(std::size_t index, const SFSEInterface* sfse)
	{
		const LoadSlot& slot = g_loadSlots[index];
		Plugin* plugin = slot.plugin;
		const std::wstring file = Log::Widen(plugin->dll);
		Progress::Milestone((slot.preload ? L"sfse-preload:" : L"sfse-load:") + Lower(file),
		                    L"Starting SFSE plugin  " + file);

		Plugin* const outer = t_current;
		t_current = plugin;
		const LARGE_INTEGER started = Now();
		const bool ok = slot.original(sfse);
		const double ms = MsSince(started);
		t_current = outer;

		{
			std::lock_guard guard(g_lock);
			(slot.preload ? plugin->preloadMs : plugin->loadMs) = ms;
			(slot.preload ? plugin->preloadRefused : plugin->loadRefused) = !ok;
			if (!ok)
				plugin->module = nullptr;  // SFSE unloads it; another DLL may load at the same address
		}
		Log::Write("sfse: %s %s %s after %.1f ms", plugin->dll.c_str(),
		           slot.preload ? "SFSEPlugin_Preload" : "SFSEPlugin_Load",
		           ok ? "finished" : "returned false (SFSE unloads it)", ms);
		return ok;
	}

	template <std::size_t N>
	bool LoadThunk(const SFSEInterface* sfse)
	{
		return RunLoad(N, sfse);
	}

	template <std::size_t... I>
	constexpr std::array<LoadFn, sizeof...(I)> MakeLoadThunks(std::index_sequence<I...>)
	{
		return { { &LoadThunk<I>... } };
	}
	const std::array<LoadFn, kLoadSlots> kLoadThunks = MakeLoadThunks(std::make_index_sequence<kLoadSlots>{});

	FARPROC WrapLoad(HMODULE module, bool preload, FARPROC original)
	{
		const std::size_t index = g_usedLoadSlots.fetch_add(1);
		if (index >= kLoadSlots) {
			if (index == kLoadSlots)
				Log::Write("sfse: more plugins than Liftoff can time; the rest run untimed");
			return original;
		}
		g_loadSlots[index] = { reinterpret_cast<LoadFn>(reinterpret_cast<void*>(original)), PluginFor(module), preload };
		return reinterpret_cast<FARPROC>(reinterpret_cast<void*>(kLoadThunks[index]));
	}

	// --- Message handlers ------------------------------------------------------------------------

	void RunListener(std::size_t index, SFSEMessagingInterface::Message* msg)
	{
		const ListenerSlot& slot = g_listenerSlots[index];
		Plugin* plugin = slot.plugin;

		std::size_t kind = kOther;
		if (msg && msg->sender && std::strcmp(msg->sender, "SFSE") == 0 && msg->type < kOther)
			kind = msg->type;
		// Startup messages also move the overlay along; save and load messages are only timed.
		if (kind <= SFSEMessagingInterface::kMessage_PostPostDataLoad) {
			const std::wstring file = Log::Widen(plugin->dll);
			Progress::Milestone(L"sfse-" + Lower(Log::Widen(kMessageNames[kind])) + L":" + Lower(file),
			                    L"Running SFSE plugin  " + file);
		}

		const LARGE_INTEGER started = Now();
		slot.original(msg);
		const double ms = MsSince(started);

		{
			std::lock_guard guard(g_lock);
			plugin->messageMs[kind] += ms;
			++plugin->messageCalls[kind];
			plugin->slowestMessageMs[kind] = std::max(plugin->slowestMessageMs[kind], ms);
		}
		if (ms >= 1.0) {
			if (kind == kOther)
				Log::Write("sfse: %s handled message %u from %s in %.1f ms", plugin->dll.c_str(),
				           msg ? msg->type : 0u, msg && msg->sender ? msg->sender : "(unknown)", ms);
			else
				Log::Write("sfse: %s handled %s in %.1f ms", plugin->dll.c_str(), kMessageNames[kind], ms);
		}
	}

	template <std::size_t N>
	void ListenerThunk(SFSEMessagingInterface::Message* msg)
	{
		RunListener(N, msg);
	}

	template <std::size_t... I>
	constexpr std::array<Listener, sizeof...(I)> MakeListenerThunks(std::index_sequence<I...>)
	{
		return { { &ListenerThunk<I>... } };
	}
	const std::array<Listener, kListenerSlots> kListenerThunks =
		MakeListenerThunks(std::make_index_sequence<kListenerSlots>{});

	bool RegisterWrapper(PluginHandle listener, const char* sender, Listener handler)
	{
		if (!handler)
			return g_register(listener, sender, handler);

		const HMODULE module = ModuleOf(reinterpret_cast<const void*>(handler));
		if (module == g_self)
			return g_register(listener, sender, handler);  // Liftoff's own handler

		// The handler's DLL, when SFSE loaded it; otherwise the plugin that is starting up right now
		// (its handler may live in a library it uses); otherwise whichever DLL the handler is in.
		Plugin* plugin = module ? FindPlugin(module) : nullptr;
		if (!plugin)
			plugin = t_current;
		if (!plugin && module)
			plugin = PluginFor(module);
		if (!plugin)
			return g_register(listener, sender, handler);

		const std::size_t index = g_usedListenerSlots.fetch_add(1);
		if (index >= kListenerSlots) {
			if (index == kListenerSlots)
				Log::Write("sfse: more message handlers than Liftoff can time; the rest run untimed");
			return g_register(listener, sender, handler);
		}
		g_listenerSlots[index] = { handler, plugin };
		return g_register(listener, sender, kListenerThunks[index]);
	}

	// --- SFSE's imports --------------------------------------------------------------------------

	template <class Load>
	HMODULE TimedLibraryLoad(const std::wstring& path, Load&& load)
	{
		const std::wstring file = BaseName(path);
		Progress::Milestone(L"sfse-dll:" + Lower(file), L"Loading SFSE plugin  " + file);

		const LARGE_INTEGER started = Now();
		HMODULE module = load();
		const DWORD error = GetLastError();  // SFSE reports it if the load failed
		const double ms = MsSince(started);

		const std::string name = Log::Narrow(file);
		if (module) {
			// A DLL SFSE unloaded (not a plugin after all) can leave an entry at this address.
			if (Plugin* earlier = FindPlugin(module); earlier && Lower(Log::Widen(earlier->dll)) != Lower(file)) {
				std::lock_guard guard(g_lock);
				earlier->module = nullptr;
			}
			Plugin* plugin = PluginFor(module);
			{
				std::lock_guard guard(g_lock);
				if (plugin->libraryMs < 0)
					plugin->libraryMs = ms;
			}
			Log::Write("sfse: %s loaded in %.1f ms", name.c_str(), ms);
		} else {
			Log::Write("sfse: %s could not be loaded (Windows error %lu)", name.c_str(), error);
		}
		SetLastError(error);
		return module;
	}

	HMODULE WINAPI Hook_LoadLibraryA(LPCSTR path)
	{
		if (!path)
			return g_loadLibraryA(path);
		return TimedLibraryLoad(AnsiToWide(path), [&] { return g_loadLibraryA(path); });
	}

	HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR path)
	{
		if (!path)
			return g_loadLibraryW(path);
		return TimedLibraryLoad(path, [&] { return g_loadLibraryW(path); });
	}

	HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR path, HANDLE file, DWORD flags)
	{
		if (!path || (flags & kResourceOnly))
			return g_loadLibraryExA(path, file, flags);
		return TimedLibraryLoad(AnsiToWide(path), [&] { return g_loadLibraryExA(path, file, flags); });
	}

	HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags)
	{
		if (!path || (flags & kResourceOnly))
			return g_loadLibraryExW(path, file, flags);
		return TimedLibraryLoad(path, [&] { return g_loadLibraryExW(path, file, flags); });
	}

	FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name)
	{
		FARPROC proc = g_getProcAddress(module, name);
		if (!proc || !name || IS_INTRESOURCE(name) || module == g_self)
			return proc;
		const bool load    = std::strcmp(name, "SFSEPlugin_Load") == 0;
		const bool preload = !load && std::strcmp(name, "SFSEPlugin_Preload") == 0;
		if (!load && !preload)
			return proc;

		const DWORD error = GetLastError();
		FARPROC wrapped = WrapLoad(module, preload, proc);
		SetLastError(error);
		return wrapped;
	}

	struct ImportPatch
	{
		const char* name;
		void*       hook;
		void**      original;
		void*       real;  // the function's address, to recognise imports bound without names
		bool        done = false;
	};

	// Redirects the named imports of one module (SFSE's DLL) to the hooks. Returns how many were found.
	int PatchImports(HMODULE module, ImportPatch* patches, std::size_t count)
	{
		auto* base = reinterpret_cast<std::uint8_t*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
			return 0;
		const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!imports.VirtualAddress || !imports.Size)
			return 0;

		int patched = 0;
		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
		     desc->Name; ++desc) {
			auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			const auto* names = desc->OriginalFirstThunk
				? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk) : nullptr;

			for (std::size_t i = 0; slots[i].u1.Function; ++i) {
				const char* importName = nullptr;
				if (names && !IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal))
					importName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name;
				void* current = reinterpret_cast<void*>(slots[i].u1.Function);

				for (std::size_t p = 0; p < count; ++p) {
					ImportPatch& patch = patches[p];
					const bool match = importName ? std::strcmp(importName, patch.name) == 0
					                              : (patch.real && current == patch.real);
					if (!match)
						continue;
					if (current == patch.hook)
						break;
					DWORD old = 0;
					if (!VirtualProtect(&slots[i].u1.Function, sizeof(slots[i].u1.Function), PAGE_READWRITE, &old))
						break;
					if (!*patch.original)
						*patch.original = current;
					slots[i].u1.Function = reinterpret_cast<ULONGLONG>(patch.hook);
					VirtualProtect(&slots[i].u1.Function, sizeof(slots[i].u1.Function), old, &old);
					patch.done = true;
					++patched;
					break;
				}
			}
		}
		return patched;
	}
}

namespace SfsePlugins
{
	const char* MessageName(std::size_t kind)
	{
		return kind < kKinds ? kMessageNames[kind] : "?";
	}

	void Install(const SFSEInterface* sfse, void* self)
	{
		g_self = static_cast<HMODULE>(self);
		QueryPerformanceFrequency(&g_frequency);

		// 1. Message handlers: SFSE hands every plugin the same messaging interface, so replacing
		//    RegisterListener there catches each handler as it's registered.
		bool handlers = false;
		auto* messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
		if (messaging && messaging->RegisterListener) {
			DWORD old = 0;
			if (VirtualProtect(&messaging->RegisterListener, sizeof(messaging->RegisterListener), PAGE_READWRITE, &old)) {
				g_register = messaging->RegisterListener;
				messaging->RegisterListener = &RegisterWrapper;
				VirtualProtect(&messaging->RegisterListener, sizeof(messaging->RegisterListener), old, &old);
				handlers = true;
			}
		}

		// 2. DLL loads and entry points: SFSE's own calls to LoadLibrary and GetProcAddress.
		int imports = 0;
		HMODULE sfseModule = ModuleOf(reinterpret_cast<const void*>(sfse->QueryInterface));
		if (sfseModule && sfseModule != g_self) {
			HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
			auto real = [&](const char* name) {
				return kernel32 ? reinterpret_cast<void*>(::GetProcAddress(kernel32, name)) : nullptr;
			};
			ImportPatch patches[] = {
				{ "GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
				  reinterpret_cast<void**>(&g_getProcAddress), real("GetProcAddress") },
				{ "LoadLibraryA", reinterpret_cast<void*>(&Hook_LoadLibraryA),
				  reinterpret_cast<void**>(&g_loadLibraryA), real("LoadLibraryA") },
				{ "LoadLibraryW", reinterpret_cast<void*>(&Hook_LoadLibraryW),
				  reinterpret_cast<void**>(&g_loadLibraryW), real("LoadLibraryW") },
				{ "LoadLibraryExA", reinterpret_cast<void*>(&Hook_LoadLibraryExA),
				  reinterpret_cast<void**>(&g_loadLibraryExA), real("LoadLibraryExA") },
				{ "LoadLibraryExW", reinterpret_cast<void*>(&Hook_LoadLibraryExW),
				  reinterpret_cast<void**>(&g_loadLibraryExW), real("LoadLibraryExW") },
			};
			imports = PatchImports(sfseModule, patches, std::size(patches));
		}

		Log::Write("SFSE plugin timing: %s; %s",
		           imports > 0 ? "DLL loads and entry points timed" : "DLL loads and entry points not timed (SFSE's imports not found)",
		           handlers ? "message handlers timed" : "message handlers not timed");
	}

	void LogLoadTotals()
	{
		double library = 0, preload = 0, load = 0, slowestMs = 0;
		int timed = 0;
		std::string slowest;
		{
			std::lock_guard guard(g_lock);
			for (const auto& plugin : g_plugins) {
				if (plugin.libraryMs < 0 && plugin.preloadMs < 0 && plugin.loadMs < 0)
					continue;
				const double total = std::max(0.0, plugin.libraryMs) + std::max(0.0, plugin.preloadMs) +
				                     std::max(0.0, plugin.loadMs);
				++timed;
				library += std::max(0.0, plugin.libraryMs);
				preload += std::max(0.0, plugin.preloadMs);
				load += std::max(0.0, plugin.loadMs);
				if (total > slowestMs) {
					slowestMs = total;
					slowest = plugin.dll;
				}
			}
		}
		if (timed == 0)
			return;
		Log::Write("SFSE plugins: %d timed, %.2f s in all (DLL loading %.2f s, SFSEPlugin_Preload %.2f s, "
		           "SFSEPlugin_Load %.2f s); slowest %s with %.2f s",
		           timed, (library + preload + load) / 1000.0, library / 1000.0, preload / 1000.0, load / 1000.0,
		           slowest.c_str(), slowestMs / 1000.0);
	}

	std::vector<Report> Snapshot()
	{
		std::lock_guard guard(g_lock);
		return std::vector<Report>(g_plugins.begin(), g_plugins.end());
	}
}

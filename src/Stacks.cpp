#include "Stacks.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

#if !defined(_M_X64) && !defined(__x86_64__)
#error LoadTrace only supports 64-bit builds (Starfield is 64-bit). Pick the amd64 kit.
#endif

namespace
{
	// Walks up the stack one function at a time. Windows keeps "unwind tables"
	// for every 64-bit function, so this works without debug symbols.
	//
	// IMPORTANT: this runs while the target thread is paused. If that thread was
	// paused in the middle of allocating memory or writing the log, it still holds
	// those locks. So this function must not allocate, log, or lock anything.
	void Unwind(CONTEXT* ctx, DWORD64* frames, int* count, int maxFrames)
	{
		while (*count < maxFrames && ctx->Rip != 0) {
			frames[(*count)++] = ctx->Rip;

			const DWORD64 previousRsp = ctx->Rsp;
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
			if (function) {
				PVOID   handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, function, ctx,
				                 &handlerData, &establisher, nullptr);
			} else {
				// A "leaf" function without a table: its return address is on top of the stack.
				MEMORY_BASIC_INFORMATION info{};
				if (!VirtualQuery(reinterpret_cast<LPCVOID>(ctx->Rsp), &info, sizeof(info)) ||
				    info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
					break;
				ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
				ctx->Rsp += 8;
			}
			if (ctx->Rsp <= previousRsp)  // the stack must shrink as we walk up; otherwise we are lost
				break;
		}
	}

	// If the walk hits unreadable memory (rare), give up on that thread instead of crashing the game.
	void UnwindGuarded(CONTEXT* ctx, DWORD64* frames, int* count, int maxFrames)
	{
#ifdef _MSC_VER
		__try {
			Unwind(ctx, frames, count, maxFrames);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
#else
		Unwind(ctx, frames, count, maxFrames);
#endif
	}

	// Finds the named (exported) function that starts closest before the address.
	// DLLs list their public functions in an "export table"; no debug symbols needed.
	const char* NearestExport(HMODULE module, DWORD64 address, DWORD& displacement)
	{
		const BYTE* base = reinterpret_cast<const BYTE*>(module);
		const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
			return nullptr;
		const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
			return nullptr;

		const auto* exports   = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
		const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
		const auto* names     = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
		const auto* ordinals  = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);

		const DWORD target = static_cast<DWORD>(address - reinterpret_cast<DWORD64>(base));
		DWORD       best   = 0;
		const char* name   = nullptr;
		for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
			if (ordinals[i] >= exports->NumberOfFunctions)
				continue;
			const DWORD start = functions[ordinals[i]];
			const bool forwarded = start >= dir.VirtualAddress && start < dir.VirtualAddress + dir.Size;
			if (!forwarded && start <= target && start > best) {
				best = start;
				name = reinterpret_cast<const char*>(base + names[i]);
			}
		}
		displacement = target - best;
		return name;
	}

	// Fills `name` with the file name of the module (e.g. "ntdll.dll"). Returns the module or null.
	// The caller must FreeLibrary() the result.
	HMODULE ModuleOf(DWORD64 address, char* name, size_t nameSize)
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &module) || !module)
			return nullptr;

		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(module, path, MAX_PATH);
		const wchar_t* file = wcsrchr(path, L'\\');
		file = file ? file + 1 : path;
		const int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, name, static_cast<int>(nameSize), nullptr, nullptr);
		if (n <= 0)
			snprintf(name, nameSize, "?");
		return module;
	}
}

namespace Stacks
{
	bool Capture(DWORD threadId, int maxFrames, Captured& out)
	{
		out.threadId = threadId;
		out.count    = 0;
		if (maxFrames > kMaxFrames)
			maxFrames = kMaxFrames;
		if (threadId == GetCurrentThreadId())
			return false;

		HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
		if (!thread)
			return false;

		if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
			// ---- target thread is paused: no allocation, no logging, no locks until ResumeThread ----
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			if (GetThreadContext(thread, &ctx))
				UnwindGuarded(&ctx, out.frames, &out.count, maxFrames);
			ResumeThread(thread);
			// ---- running again ----
		}
		CloseHandle(thread);
		return out.count > 0;
	}

	void Describe(DWORD64 address, char* out, size_t outSize)
	{
		char moduleName[160];
		HMODULE module = ModuleOf(address, moduleName, sizeof(moduleName));
		if (!module) {
			snprintf(out, outSize, "0x%016llX  (not inside any DLL: generated code?)", static_cast<unsigned long long>(address));
			return;
		}

		const unsigned long long offset = address - reinterpret_cast<DWORD64>(module);
		DWORD displacement = 0;
		const char* symbol = nullptr;
		if (module != GetModuleHandleW(nullptr))  // the game .exe has no useful export names
			symbol = NearestExport(module, address, displacement);

		if (symbol && displacement < 0x200)
			snprintf(out, outSize, "%s+0x%llX  (near %s+0x%lX)", moduleName, offset, symbol, static_cast<unsigned long>(displacement));
		else
			snprintf(out, outSize, "%s+0x%llX", moduleName, offset);
		FreeLibrary(module);
	}

	bool IsSystemPlumbing(DWORD64 address)
	{
		char name[160];
		HMODULE module = ModuleOf(address, name, sizeof(name));
		if (!module)
			return false;
		FreeLibrary(module);
		return _stricmp(name, "ntdll.dll") == 0 || _stricmp(name, "kernel32.dll") == 0 ||
		       _stricmp(name, "kernelbase.dll") == 0;
	}
}

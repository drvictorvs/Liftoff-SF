#include "Log.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
	HANDLE     g_file = INVALID_HANDLE_VALUE;
	std::mutex g_lock;
	const auto g_start = std::chrono::steady_clock::now();
}

namespace Log
{
	void Open(const std::wstring& path)
	{
		g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	void Write(const char* fmt, ...)
	{
		if (g_file == INVALID_HANDLE_VALUE)
			return;

		char message[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(message, sizeof(message), fmt, args);
		va_end(args);

		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - g_start).count();

		char line[1100];
		const int len = snprintf(line, sizeof(line), "[%7lld ms] %s\r\n",
		                         static_cast<long long>(ms), message);
		if (len <= 0)
			return;
		const DWORD bytes = static_cast<DWORD>(len < (int)sizeof(line) ? len : (int)sizeof(line) - 1);

		// WriteFile hands the text to Windows straight away, so the log
		// survives even if the game crashes a moment later.
		std::lock_guard guard(g_lock);
		DWORD written = 0;
		WriteFile(g_file, line, bytes, &written, nullptr);
	}

	std::string Narrow(std::wstring_view text)
	{
		if (text.empty())
			return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
		std::string out(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size, nullptr, nullptr);
		return out;
	}

	std::wstring Widen(std::string_view text)
	{
		if (text.empty())
			return {};
		const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
		std::wstring out(size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size);
		return out;
	}
}

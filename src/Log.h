#pragma once
// A tiny log file writer. Your best debugging tool: when something goes wrong,
// open Documents\My Games\Starfield\SFSE\Logs\Liftoff.log and read it.
#include <string>
#include <string_view>

namespace Log
{
	void Open(const std::wstring& path);

	// printf-style: Log::Write("loaded %d files", count);
	void Write(const char* fmt, ...);

	// Conversions between Windows' UTF-16 strings and UTF-8.
	std::string  Narrow(std::wstring_view text);
	std::wstring Widen(std::string_view text);
}

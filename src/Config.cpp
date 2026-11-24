#include "Config.h"

#include <algorithm>
#include <cwctype>

namespace
{
	std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
	{
		wchar_t buffer[1024]{};
		GetPrivateProfileStringW(section, key, fallback, buffer, 1024, ini.c_str());
		return buffer;
	}

	int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback)
	{
		return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
	}

	int ReadBoundedInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
	                   int fallback, int minimum, int maximum)
	{
		return std::clamp(ReadInt(ini, section, key, fallback), minimum, maximum);
	}

	// "#4FC3F7" -> RGB(0x4F, 0xC3, 0xF7)
	COLORREF ParseColor(const std::wstring& text, COLORREF fallback)
	{
		std::wstring hex = text;
		if (!hex.empty() && hex[0] == L'#')
			hex.erase(0, 1);
		if (hex.size() != 6)
			return fallback;
		try {
			const unsigned long value = std::stoul(hex, nullptr, 16);
			return RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
		} catch (...) {
			return fallback;
		}
	}

	// ".esm, .ESP ,ba2" -> { L".esm", L".esp", L".ba2" }
	std::vector<std::wstring> ParseExtensions(const std::wstring& text)
	{
		std::vector<std::wstring> result;
		std::wstring current;
		auto flush = [&] {
			if (current.empty())
				return;
			if (current[0] != L'.')
				current.insert(current.begin(), L'.');
			result.push_back(current);
			current.clear();
		};
		for (wchar_t c : text) {
			if (c == L',' || c == L';' || std::iswspace(c))
				flush();
			else
				current.push_back(static_cast<wchar_t>(std::towlower(c)));
		}
		flush();
		return result;
	}
}

Config LoadConfig(const std::wstring& ini)
{
	Config c;

	c.enabled = ReadInt(ini, L"General", L"Enabled", 1) != 0;

	c.barColor    = ParseColor(ReadString(ini, L"Look", L"BarColor", L""), c.barColor);
	c.showStatus  = ReadInt(ini, L"Look", L"ShowStatus", 1) != 0;
	c.showPercent = ReadInt(ini, L"Look", L"ShowPercent", 1) != 0;
	c.useStarfieldFont = ReadInt(ini, L"Look", L"UseStarfieldFont", 1) != 0;
	c.panelColor  = ParseColor(ReadString(ini, L"Look", L"PanelColor", L""), c.panelColor);
	c.panelOpacity = ReadBoundedInt(ini, L"Look", L"PanelOpacity", c.panelOpacity, 0, 100);
	c.textColor   = ParseColor(ReadString(ini, L"Look", L"TextColor", L""), c.textColor);
	c.fontSize    = ReadBoundedInt(ini, L"Look", L"FontSize", c.fontSize, 10, 48);
	c.uppercase   = ReadInt(ini, L"Look", L"Uppercase", 1) != 0;
	c.transparentBackground = ReadInt(ini, L"Look", L"TransparentBackground", 0) != 0;

	c.stayOnTop         = ReadInt(ini, L"Behaviour", L"StayOnTop", 1) != 0;
	c.showOnSaveLoad    = ReadBoundedInt(ini, L"Behaviour", L"ShowOnSaveLoad", c.showOnSaveLoad, 0, 2);
	c.fadeMs            = ReadInt(ini, L"Behaviour", L"FadeMs", c.fadeMs);
	c.timeoutSeconds    = ReadInt(ini, L"Behaviour", L"TimeoutSeconds", c.timeoutSeconds);

	const std::wstring ext = ReadString(ini, L"Tracking", L"Extensions", L"");
	if (!ext.empty())
		c.trackedExtensions = ParseExtensions(ext);
	c.logAllFiles = ReadInt(ini, L"Tracking", L"LogAllFiles", 0) != 0;

	c.logFiles = ReadInt(ini, L"Files", L"LogFiles", 1) != 0;
	c.logFailedOpens = ReadInt(ini, L"Files", L"LogFailedOpens", 0) != 0;
	c.minOpenMs = ReadBoundedInt(ini, L"Files", L"MinOpenMs", c.minOpenMs, 0, 60000);
	c.maxFileLines = ReadBoundedInt(ini, L"Files", L"MaxFileLines", c.maxFileLines, 0, 5000000);

	c.stallSeconds = ReadBoundedInt(ini, L"Watchdog", L"StallSeconds", c.stallSeconds, 1, 600);
	c.sampleSeconds = ReadBoundedInt(ini, L"Watchdog", L"SampleSeconds", c.sampleSeconds, 1, 600);
	c.maxDumps = ReadBoundedInt(ini, L"Watchdog", L"MaxDumps", c.maxDumps, 1, 1000);
	c.stackDepth = ReadBoundedInt(ini, L"Watchdog", L"StackDepth", c.stackDepth, 4, 64);
	c.lookBackSeconds = ReadBoundedInt(ini, L"Watchdog", L"LookBackSeconds", c.lookBackSeconds, 0, 60);

	return c;
}

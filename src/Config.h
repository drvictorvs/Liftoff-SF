#pragma once
// Settings read from Data\SFSE\Plugins\Liftoff.ini
#include <windows.h>

#include <string>
#include <vector>

struct Config
{
	bool enabled = true;

	// Look
	COLORREF     barColor    = RGB(79, 195, 247);
	bool         showStatus  = true; // "Loading plugin Starfield.esm"
	bool         showPercent = true;
	bool         useStarfieldFont = false;
	bool         transparentBackground = false;

	// Behaviour
	bool stayOnTop         = true;
	int  fadeMs            = 500;
	int  timeoutSeconds    = 240;    // safety net: close even if the "done" signal never arrives

	// Which file types count as loading milestones (lower case, with the dot)
	std::vector<std::wstring> trackedExtensions{ L".esm", L".esp", L".esl", L".ba2" };

	// Debug aid: write EVERY file the game opens to Liftoff.log (big log, slower start).
	bool logAllFiles = false;

	// File-open diagnostics and save-load sampling, merged from LoadTrace.
	bool logFiles = true;
	bool logFailedOpens = false;
	int  minOpenMs = 0;
	int  maxFileLines = 100000;
	int  stallSeconds = 3;
	int  sampleSeconds = 5;
	int  maxDumps = 60;
	int  stackDepth = 32;
	int  lookBackSeconds = 10;
};

Config LoadConfig(const std::wstring& iniPath);

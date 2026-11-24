#pragma once
// Settings read from Data\SFSE\Plugins\Liftoff.ini
#include <windows.h>

#include <string>
#include <vector>

struct Config
{
	bool enabled = true;

	// Look. The defaults copy Starfield's main menu: white uppercase text on a flat #5D5D5D panel,
	// with a white bar like the menu's highlighted row.
	COLORREF     barColor    = RGB(255, 255, 255);
	COLORREF     panelColor  = RGB(0x5D, 0x5D, 0x5D);
	int          panelOpacity = 100;  // percent
	COLORREF     textColor   = RGB(255, 255, 255);
	int          fontSize    = 20;    // pixels; the panel's height follows it
	bool         uppercase   = true;
	bool         showStatus  = true; // "Loading plugin Starfield.esm"
	bool         showPercent = true;
	bool         useStarfieldFont = true;  // NB Architekt if installed, else Bahnschrift; off = Segoe UI
	bool         transparentBackground = false;

	// Behaviour
	bool stayOnTop         = true;
	// When a save loads: 0 = never show the overlay (startup only), 1 = only for the first save load
	// of the session (the one from the main menu), 2 = every save load.
	int  showOnSaveLoad    = 1;
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

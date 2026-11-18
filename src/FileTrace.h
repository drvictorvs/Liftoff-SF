#pragma once
// Logs every file the game opens, with how long the open took.
//
// It hooks NtCreateFile and NtOpenFile in ntdll.dll. Every way of opening a
// file on Windows ends up in one of those two, so nothing slips past.
#include "Config.h"

namespace FileTrace
{
	bool Install(const Config& config);

	// How many files were opened since the last call (for the once-a-second pulse line).
	unsigned TakeOpenCount();

	// File opens made by this thread are not logged (used for the watchdog thread).
	void IgnoreThread(unsigned long threadId);
}

#pragma once
// Watches tracked files opened during startup and save loads, by "hooking" the
// Windows functions that open files (CreateFileW and friends).
//
// Why this approach: it needs NO knowledge of Starfield's internals, so the
// plugin keeps working after game updates without being recompiled.
#include <string>
#include <vector>

namespace FileWatch
{
	bool Install(const std::vector<std::wstring>& trackedExtensions, bool logAllFiles);

	// Available for explicitly disabling file watching; normal operation keeps
	// the hooks active so save loads can update the overlay.
	void Uninstall();
}

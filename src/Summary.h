#pragma once
// The startup summary written to Liftoff.log after the game has loaded its data:
//   - the longest stretches between loading steps (what the game was busy with),
//   - the SFSE plugins, slowest first (DLL load, SFSEPlugin_Preload/Load, startup message handlers),
//   - the files the game spent the most time opening and reading, and the totals by file type.
#include "Config.h"

namespace Summary
{
	// Remembers the settings. Call at startup.
	void Start(const Config& config);

	// Startup has finished: write the summary two seconds from now, so handlers that run after
	// Liftoff's own and the first main-menu reads are included.
	void Schedule();

	// Write it now if it hasn't been written yet (a save started loading first, for example).
	void WriteNow();
}

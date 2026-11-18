#pragma once
// A background thread that watches the game from the inside:
//   - once a second it writes a "pulse" line: CPU, disk, memory, video memory
//   - while a save is loading, or when the game window stops answering,
//     it writes down what the busy threads are doing (stack dumps)
#include "Config.h"

namespace Watchdog
{
	void Start(const Config& config);

	// Called from SFSE's messages (on a game thread).
	void LoadStarted(const char* saveName);
	void LoadFinished();
}

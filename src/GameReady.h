#pragma once

namespace GameReady
{
	// Starts the main-thread player-world readiness check.
	bool Start();

	// Begin waiting after an actual SFSE load event or an inferred save-file read.
	void Begin(bool waitForPostLoadMessage);
	void PostLoadMessageReceived();
}

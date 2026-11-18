#pragma once
// The splash window: an image with a progress bar and status line.
//
// It runs on its OWN thread with its own message loop. That is the key trick:
// the game's main thread is busy loading and can't draw anything, but our
// thread is free, so the window keeps animating the whole time.
#include "Config.h"

namespace Splash
{
	void Start(const Config& config);
}

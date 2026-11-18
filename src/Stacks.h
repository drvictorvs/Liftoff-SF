#pragma once
// Takes a "stack trace" of another thread: the chain of functions it is in
// right now, innermost first. This is how we see what a frozen game is doing.
#include <windows.h>

#include <cstddef>

namespace Stacks
{
	constexpr int kMaxFrames = 64;

	struct Captured
	{
		DWORD   threadId = 0;
		int     count    = 0;                // how many entries of frames[] are valid
		DWORD64 frames[kMaxFrames]{};        // code addresses, innermost first
	};

	// Briefly pauses the thread, reads its stack, lets it run again.
	// Must not be called for the calling thread itself.
	bool Capture(DWORD threadId, int maxFrames, Captured& out);

	// "nvwgf2umx.dll+0x1A2B3C  (near SomeFunction+0x24)"
	void Describe(DWORD64 address, char* out, size_t outSize);

	// True if the address belongs to core Windows plumbing (ntdll, kernel32, kernelbase).
	bool IsSystemPlumbing(DWORD64 address);
}

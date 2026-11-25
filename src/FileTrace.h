#pragma once
// Logs every file the game opens, with how long the open took.
//
// It hooks NtCreateFile and NtOpenFile in ntdll.dll. Every way of opening a
// file on Windows ends up in one of those two, so nothing slips past.
#include "Config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace FileTrace
{
	bool Install(const Config& config);

	// Until the startup summary, the time spent opening and reading each file is added up per path
	// (reads are timed through NtReadFile; asynchronous ones are only counted).
	struct FileReport
	{
		std::wstring  path;          // e.g. C:\Games\Starfield\Data\Starfield.esm
		std::uint64_t opens = 0;
		std::uint64_t reads = 0;     // reads that finished inside the call
		std::uint64_t pendingReads = 0;  // asynchronous reads: counted, not timed
		std::uint64_t bytes = 0;     // read by the timed reads
		double        openMs = 0;
		double        readMs = 0;
	};

	// Stops adding up and returns the totals (once; later calls return nothing).
	std::vector<FileReport> StopCollecting();

	// How many files were opened since the last call (for the once-a-second pulse line).
	unsigned TakeOpenCount();

	// File opens made by this thread are not logged (used for the watchdog thread).
	void IgnoreThread(unsigned long threadId);

	// While one exists, file opens made by the current thread are not logged or counted (for
	// Liftoff's own reads, such as the overlay font).
	struct QuietScope
	{
		QuietScope();
		~QuietScope();
		QuietScope(const QuietScope&) = delete;
		QuietScope& operator=(const QuietScope&) = delete;
	};
}

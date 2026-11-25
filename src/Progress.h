#pragma once
// Turns "things that happened during startup" into a progress percentage.
//
// The idea: every launch, we write down WHEN each milestone happened
// (e.g. "Starfield.esm was opened 8.2 s after start, data finished at 41 s").
// On the next launch we compare: if Starfield.esm took 20% of the total time
// last time, then seeing it now means we're about 20% done.
// The first launch has nothing to compare against, so the bar just animates.
#include <string>
#include <string_view>
#include <vector>

namespace Progress
{
	// Start the clock and read the previous run's timings (if any).
	void Init(const std::wstring& profilePath);

	// Something happened. Safe to call from any thread.
	//   key    - stable identifier, e.g. L"file:starfield.esm"
	//   status - text for the user, e.g. L"Loading plugin Starfield.esm"
	void Milestone(std::wstring_view key, std::wstring_view status);

	// Startup is complete: save this run's timings for next time.
	void Finish();

	// Save-load activity is separate from startup calibration.
	void BeginSaveLoad(std::wstring_view saveName);
	bool BeginSaveLoadFromFile(std::wstring_view saveName);
	void ReportFileActivity(std::wstring_view status);
	void FinishSaveLoad();

	struct Snapshot
	{
		bool         calibrated  = false; // false on the very first launch
		double       fraction    = 0.0;   // 0..1, only meaningful if calibrated
		bool         done        = false;
		bool         saveLoading = false;
		int          saveLoadsFinished = 0;  // save loads completed this session
		std::wstring status;
	};

	Snapshot Get();

	// What happened during startup, in order, for the summary written after it.
	struct Event
	{
		std::wstring key;
		std::wstring status;  // the overlay text at that moment, e.g. "Loading plugin  Starfield.esm"
		double       ms = 0;  // since Init()
	};
	struct Timeline
	{
		std::vector<Event> events;
		double             finishedMs = -1;  // when Finish() ran; < 0 = not yet
	};
	Timeline GetTimeline();
}

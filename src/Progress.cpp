#include "Progress.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	std::mutex        g_lock;
	Clock::time_point g_start;
	std::wstring      g_profilePath;

	// --- last run (loaded from disk) ---
	std::unordered_map<std::wstring, double> g_previousFraction;  // key -> 0..1
	std::vector<double>                      g_previousSorted;    // all fractions, ascending
	double                                   g_previousTotalMs = 0;

	// --- this run ---
	std::vector<std::pair<std::wstring, double>> g_events;  // key, ms since start
	std::unordered_set<std::wstring>             g_seen;
	double       g_reachedFraction = 0;  // best fraction confirmed by a milestone
	double       g_shownFraction   = 0;  // what the bar displays (never goes backwards)
	bool         g_done            = false;
	bool         g_saveLoading     = false;
	std::wstring g_status          = L"Starting";

	double ElapsedMs()
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
	}

	void LoadProfile()
	{
		std::ifstream in{ std::filesystem::path(g_profilePath) };
		if (!in)
			return;

		std::vector<std::pair<std::wstring, double>> rows;
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#')
				continue;
			const auto tab = line.find('\t');
			if (tab == std::string::npos)
				continue;
			const double ms = std::atof(line.substr(0, tab).c_str());
			std::string key = line.substr(tab + 1);
			if (!key.empty() && key.back() == '\r')
				key.pop_back();
			if (key == "total")
				g_previousTotalMs = ms;
			else
				rows.emplace_back(Log::Widen(key), ms);
		}

		if (g_previousTotalMs < 1000)  // nonsense profile, ignore it
			return;

		for (auto& [key, ms] : rows) {
			const double f = std::clamp(ms / g_previousTotalMs, 0.0, 1.0);
			g_previousFraction[key] = f;
			g_previousSorted.push_back(f);
		}
		std::sort(g_previousSorted.begin(), g_previousSorted.end());
	}

	void SaveProfile(double totalMs)
	{
		std::ofstream out(std::filesystem::path(g_profilePath), std::ios::trunc);
		if (!out) {
			Log::Write("could not write profile");
			return;
		}
		out << "# Liftoff startup profile: milliseconds<TAB>milestone. Delete this file to recalibrate.\n";
		out << static_cast<long long>(totalMs) << "\ttotal\n";
		for (auto& [key, ms] : g_events)
			out << static_cast<long long>(ms) << '\t' << Log::Narrow(key) << '\n';
	}
}

namespace Progress
{
	void Init(const std::wstring& profilePath)
	{
		std::lock_guard guard(g_lock);
		g_start       = Clock::now();
		g_profilePath = profilePath;
		LoadProfile();
		Log::Write("profile: %zu milestones from last run, total %.1f s",
		           g_previousFraction.size(), g_previousTotalMs / 1000.0);
	}

	void Milestone(std::wstring_view key, std::wstring_view status)
	{
		std::lock_guard guard(g_lock);
		if (g_done)
			return;

		g_status.assign(status);

		std::wstring k(key);
		if (!g_seen.insert(k).second)
			return;  // already counted (games often open the same file several times)

		const double ms = ElapsedMs();
		g_events.emplace_back(k, ms);

		if (auto it = g_previousFraction.find(k); it != g_previousFraction.end())
			g_reachedFraction = std::max(g_reachedFraction, it->second);
	}

	void Finish()
	{
		std::lock_guard guard(g_lock);
		if (g_done)
			return;
		g_done   = true;
		if (!g_saveLoading)
			g_status = L"Ready";

		const double total = ElapsedMs();
		Log::Write("startup finished after %.1f s (%zu milestones)", total / 1000.0, g_events.size());
		SaveProfile(total);
	}

	void BeginSaveLoad(std::wstring_view saveName)
	{
		std::lock_guard guard(g_lock);
		g_saveLoading = true;
		g_status = L"Loading save";
		if (!saveName.empty()) {
			g_status += L"  ";
			g_status.append(saveName);
		}
		Log::Write("save load started: %s", Log::Narrow(saveName).c_str());
	}

	bool BeginSaveLoadFromFile(std::wstring_view saveName)
	{
		std::lock_guard guard(g_lock);
		if (!g_done || g_saveLoading)
			return false;
		g_saveLoading = true;
		g_status = L"Loading save";
		if (!saveName.empty()) {
			g_status += L"  ";
			g_status.append(saveName);
		}
		Log::Write("save load inferred from save-file read: %s", Log::Narrow(saveName).c_str());
		return true;
	}

	void ReportFileActivity(std::wstring_view status)
	{
		std::lock_guard guard(g_lock);
		if (g_saveLoading)
			g_status.assign(status);
	}

	void FinishSaveLoad()
	{
		std::lock_guard guard(g_lock);
		if (!g_saveLoading)
			return;
		g_saveLoading = false;
		g_status = L"Save loaded";
		Log::Write("save load finished");
	}

	Snapshot Get()
	{
		std::lock_guard guard(g_lock);

		Snapshot s;
		s.status     = g_status;
		s.done       = g_done;
		s.saveLoading = g_saveLoading;
		s.calibrated = g_previousTotalMs > 0;

		if (g_saveLoading)
			return s;

		if (g_done) {
			s.fraction = 1.0;
			return s;
		}
		if (!s.calibrated)
			return s;

		// Time says we should be here...
		const double byTime = ElapsedMs() / g_previousTotalMs;

		// ...but don't run past the next milestone we're still waiting for.
		auto next = std::upper_bound(g_previousSorted.begin(), g_previousSorted.end(), g_reachedFraction);
		const double ceiling = (next != g_previousSorted.end()) ? *next : 0.99;

		double estimate = std::max(g_reachedFraction, std::min(byTime, ceiling));
		estimate        = std::min(estimate, 0.99);  // 100% only when really done

		g_shownFraction = std::max(g_shownFraction, estimate);
		s.fraction      = g_shownFraction;
		return s;
	}
}

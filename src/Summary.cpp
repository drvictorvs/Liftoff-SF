#include "Summary.h"

#include "FileTrace.h"
#include "Log.h"
#include "Progress.h"
#include "SfsePlugins.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr DWORD kDelayMs = 2000;

	Config            g_cfg;
	bool              g_enabled = false;
	std::atomic<bool> g_scheduled{ false };
	std::atomic<bool> g_written{ false };

	std::size_t Rows(std::size_t available)
	{
		return g_cfg.summaryLines > 0 ? std::min<std::size_t>(available, std::size_t(g_cfg.summaryLines)) : available;
	}

	std::wstring Lower(std::wstring text)
	{
		for (auto& c : text)
			c = static_cast<wchar_t>(std::towlower(c));
		return text;
	}

	// "Loading plugin  Starfield.esm" -> "Loading plugin Starfield.esm"
	std::string Step(std::wstring_view status)
	{
		std::wstring text(status);
		for (std::size_t gap; (gap = text.find(L"  ")) != std::wstring::npos;)
			text.erase(gap, 1);
		return Log::Narrow(text);
	}

	// Milliseconds in a fixed-width column, or "-" for "not seen".
	std::string Ms(double ms)
	{
		char text[32];
		if (ms < 0)
			snprintf(text, sizeof(text), "%10s", "-");
		else
			snprintf(text, sizeof(text), "%10.1f", ms);
		return text;
	}

	// The game's folder, to show paths under it shortly (Data\Starfield.esm).
	std::wstring GameFolder()
	{
		wchar_t exe[MAX_PATH * 2]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
		std::wstring folder = exe;
		folder.erase(folder.find_last_of(L"\\/") + 1);
		return Lower(folder);
	}

	void WriteTimeline(const Progress::Timeline& timeline)
	{
		const auto& events = timeline.events;
		if (events.empty()) {
			Log::Write("no loading steps were seen");
			return;
		}
		const double end = timeline.finishedMs >= 0 ? timeline.finishedMs : events.back().ms;
		Log::Write("startup took %.1f s, from Liftoff starting to SFSE's \"data loaded\" message; %zu loading steps seen",
		           end / 1000.0, events.size());

		struct Gap
		{
			double      at;
			double      length;
			std::size_t step;  // index into events, or events.size() for "before the first step"
		};
		std::vector<Gap> gaps;
		gaps.reserve(events.size() + 1);
		gaps.push_back({ 0.0, events.front().ms, events.size() });
		for (std::size_t i = 0; i < events.size(); ++i) {
			const double next = i + 1 < events.size() ? events[i + 1].ms : end;
			gaps.push_back({ events[i].ms, std::max(0.0, next - events[i].ms), i });
		}
		std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) { return a.length > b.length; });

		Log::Write("longest stretches between loading steps (the step is the last one seen before the stretch):");
		Log::Write("     gap s      at s  step");
		for (std::size_t i = 0; i < Rows(gaps.size()); ++i) {
			const Gap& gap = gaps[i];
			const std::string step = gap.step < events.size() ? Step(events[gap.step].status) : "(before the first step)";
			Log::Write("  %8.2f  %8.2f  %s", gap.length / 1000.0, gap.at / 1000.0, step.c_str());
		}
	}

	double StartupMessagesMs(const SfsePlugins::Report& plugin)
	{
		double ms = 0;
		for (std::size_t kind = 0; kind <= SFSEMessagingInterface::kMessage_PostPostDataLoad; ++kind)
			ms += plugin.messageMs[kind];
		return ms;
	}

	double TotalMs(const SfsePlugins::Report& plugin)
	{
		return std::max(0.0, plugin.libraryMs) + std::max(0.0, plugin.preloadMs) + std::max(0.0, plugin.loadMs) +
		       StartupMessagesMs(plugin);
	}

	void WritePlugins(std::vector<SfsePlugins::Report> plugins)
	{
		std::erase_if(plugins, [](const SfsePlugins::Report& plugin) {
			return plugin.libraryMs < 0 && plugin.preloadMs < 0 && plugin.loadMs < 0 &&
			       std::none_of(plugin.messageCalls.begin(), plugin.messageCalls.end(), [](unsigned n) { return n > 0; });
		});
		if (plugins.empty()) {
			Log::Write("SFSE plugins: none were timed (SfsePlugins=0 in Liftoff.ini, or SFSE didn't preload Liftoff)");
			return;
		}
		std::sort(plugins.begin(), plugins.end(),
		          [](const auto& a, const auto& b) { return TotalMs(a) > TotalMs(b); });
		double all = 0;
		for (const auto& plugin : plugins)
			all += TotalMs(plugin);

		Log::Write("SFSE plugins: %zu timed, %.2f s in all. library = loading the DLL (with its own startup code), "
		           "preload/load = SFSEPlugin_Preload/Load, messages = its PostLoad to PostPostDataLoad handlers",
		           plugins.size(), all / 1000.0);
		Log::Write("    total ms  library ms  preload ms     load ms messages ms  plugin");
		for (std::size_t i = 0; i < Rows(plugins.size()); ++i) {
			const auto& plugin = plugins[i];
			std::string label = plugin.dll;
			const std::string stem = plugin.dll.substr(0, plugin.dll.rfind('.'));
			if (!plugin.name.empty() && plugin.name != stem)
				label += " (" + plugin.name + ")";
			if (plugin.preloadRefused || plugin.loadRefused)
				label += " [declined to load]";
			const double messages = StartupMessagesMs(plugin);
			Log::Write("  %s  %s  %s  %s  %s  %s", Ms(TotalMs(plugin)).c_str(), Ms(plugin.libraryMs).c_str(),
			           Ms(plugin.preloadMs).c_str(), Ms(plugin.loadMs).c_str(),
			           Ms(messages > 0 ? messages : -1).c_str(), label.c_str());
		}

		Log::Write("startup message handlers, by message:");
		for (std::size_t kind = 0; kind <= SFSEMessagingInterface::kMessage_PostPostDataLoad; ++kind) {
			double total = 0, slowest = 0;
			int handlers = 0;
			const SfsePlugins::Report* slowestPlugin = nullptr;
			for (const auto& plugin : plugins) {
				if (!plugin.messageCalls[kind])
					continue;
				++handlers;
				total += plugin.messageMs[kind];
				if (plugin.messageMs[kind] > slowest) {
					slowest = plugin.messageMs[kind];
					slowestPlugin = &plugin;
				}
			}
			if (handlers == 0)
				continue;
			Log::Write("  %-17s %8.1f ms in %d handler%s; slowest %s with %.1f ms", SfsePlugins::MessageName(kind), total,
			           handlers, handlers == 1 ? "" : "s", slowestPlugin ? slowestPlugin->dll.c_str() : "-", slowest);
		}
	}

	void WriteFiles(std::vector<FileTrace::FileReport> files)
	{
		if (files.empty()) {
			Log::Write("files: per-file times weren't collected");
			return;
		}

		double openMs = 0, readMs = 0;
		std::uint64_t bytes = 0, pending = 0;
		struct Type
		{
			std::size_t   files = 0;
			double        ms    = 0;
			std::uint64_t bytes = 0;
		};
		std::unordered_map<std::wstring, Type> types;
		for (const auto& file : files) {
			openMs += file.openMs;
			readMs += file.readMs;
			bytes += file.bytes;
			pending += file.pendingReads;

			const auto slash = file.path.find_last_of(L"\\/");
			const auto dot = file.path.rfind(L'.');
			std::wstring extension = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)
				? Lower(file.path.substr(dot)) : L"(none)";
			Type& type = types[extension];
			++type.files;
			type.ms += file.openMs + file.readMs;
			type.bytes += file.bytes;
		}

		constexpr double kMb = 1024.0 * 1024.0;
		Log::Write("files: %zu different files opened; %.2f s opening and %.2f s reading them, %.1f MB read (times are added up "
		           "over all threads, so they can exceed the startup time; %llu asynchronous reads were counted "
		           "but not timed)",
		           files.size(), openMs / 1000.0, readMs / 1000.0, double(bytes) / kMb,
		           static_cast<unsigned long long>(pending));

		std::vector<std::pair<std::wstring, Type>> byType(types.begin(), types.end());
		std::sort(byType.begin(), byType.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
		Log::Write("by file type:");
		for (std::size_t i = 0; i < std::min<std::size_t>(Rows(byType.size()), 12); ++i) {
			const auto& [extension, type] = byType[i];
			Log::Write("  %-10s %9.2f s  %7zu files  %10.1f MB", Log::Narrow(extension).c_str(), type.ms / 1000.0,
			           type.files, double(type.bytes) / kMb);
		}

		std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
			return a.openMs + a.readMs > b.openMs + b.readMs;
		});
		const std::wstring game = GameFolder();
		Log::Write("files that took the longest to open and read:");
		Log::Write("    total ms     open ms     read ms     reads  opens          MB  file");
		for (std::size_t i = 0; i < Rows(files.size()); ++i) {
			const auto& file = files[i];
			std::wstring shown = file.path;
			if (!game.empty() && Lower(shown.substr(0, game.size())) == game)
				shown.erase(0, game.size());
			Log::Write("  %s  %s  %s  %8llu  %5llu  %10.1f  %s", Ms(file.openMs + file.readMs).c_str(),
			           Ms(file.openMs).c_str(), Ms(file.readMs).c_str(), static_cast<unsigned long long>(file.reads),
			           static_cast<unsigned long long>(file.opens), double(file.bytes) / kMb,
			           Log::Narrow(shown).c_str());
		}
	}

	void Write()
	{
		if (!g_enabled || g_written.exchange(true))
			return;
		// Stop the per-file counting first, so the summary itself doesn't keep it running.
		auto files    = FileTrace::StopCollecting();
		auto plugins  = SfsePlugins::Snapshot();
		auto timeline = Progress::GetTimeline();

		Log::Write("========== startup summary ==========");
		WriteTimeline(timeline);
		WritePlugins(std::move(plugins));
		WriteFiles(std::move(files));
		Log::Write("========== end of startup summary ==========");
	}
}

namespace Summary
{
	void Start(const Config& config)
	{
		g_cfg = config;
		g_enabled = config.startupSummary;
	}

	void Schedule()
	{
		if (!g_enabled || g_scheduled.exchange(true))
			return;
		std::thread([] {
			Sleep(kDelayMs);
			Write();
		}).detach();
	}

	void WriteNow()
	{
		Write();
	}
}

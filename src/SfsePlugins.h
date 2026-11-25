#pragma once
// Times the other SFSE plugins while SFSE loads them: how long each DLL took to load (including its
// own startup code), how long its SFSEPlugin_Preload / SFSEPlugin_Load ran, and how long its
// handlers for SFSE's messages (PostLoad, PostDataLoad, save and load events...) took.
//
// How: SFSE loads plugins with LoadLibrary and finds their entry points with GetProcAddress. Liftoff
// redirects those two calls in SFSE's own import table only (nothing else in the game is affected),
// and hands SFSE a small timing wrapper in place of each plugin's entry point. Message handlers are
// wrapped the same way, by replacing RegisterListener in SFSE's messaging interface.
//
// Only what happens after Liftoff's own SFSEPlugin_Preload is seen: every plugin's SFSEPlugin_Load,
// and the preloads of plugins whose files sort after Liftoff.dll.
#include "sfse_api.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace SfsePlugins
{
	// Message kinds: SFSE's eight messages in their own order, then everything else (messages one
	// plugin sends another).
	constexpr std::size_t kMessageKinds = 9;
	const char* MessageName(std::size_t kind);

	// self: Liftoff's own module, which is never wrapped.
	void Install(const SFSEInterface* sfse, void* self);

	// One line in the log with how many plugins loaded and how long that took (at PostLoad).
	void LogLoadTotals();

	struct Report
	{
		std::string dll;           // file name, e.g. "MoreInformativeConsole.dll" (UTF-8)
		std::string name;          // name from the plugin's version data, if it has one
		double      libraryMs = -1;  // LoadLibrary, including the DLL's own startup code; < 0 = not seen
		double      preloadMs = -1;  // SFSEPlugin_Preload; < 0 = not seen
		double      loadMs    = -1;  // SFSEPlugin_Load; < 0 = not seen
		bool        preloadRefused = false;  // returned false
		bool        loadRefused    = false;
		std::array<double, kMessageKinds>   messageMs{};
		std::array<unsigned, kMessageKinds> messageCalls{};
		std::array<double, kMessageKinds>   slowestMessageMs{};
	};

	std::vector<Report> Snapshot();
}

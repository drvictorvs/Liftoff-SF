#include "OsfSettings.h"

#include "Log.h"

#include <SFSE/Impl/PCH.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/B/BSFixedString.h>
#include <RE/G/GameVM.h>
#include <RE/I/IVirtualMachine.h>
#include <RE/I/IStackCallbackSaveInterface.h>
#include <RE/V/Variable.h>
#include <SFSE/API.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <charconv>
#include <functional>
#include <string_view>
#include <utility>

namespace
{
	constexpr ULONGLONG kPollIntervalMs = 2000;
	constexpr std::size_t kSettingCount = 30;
	// An older Liftoff_OSF.pex returns fewer values (the first 20 were there from the start); the
	// ones it sends are still saved.
	constexpr std::size_t kMinimumSettingCount = 20;

	struct Setting
	{
		const wchar_t* section;
		const wchar_t* key;
	};

	constexpr std::array<Setting, kSettingCount> kSettings{ {
		{ L"General", L"Enabled" },
		{ L"Look", L"BarColor" },
		{ L"Look", L"ShowStatus" },
		{ L"Look", L"ShowPercent" },
		{ L"Look", L"UseStarfieldFont" },
		{ L"Look", L"TransparentBackground" },
		{ L"Behaviour", L"StayOnTop" },
		{ L"Behaviour", L"FadeMs" },
		{ L"Behaviour", L"TimeoutSeconds" },
		{ L"Tracking", L"Extensions" },
		{ L"Tracking", L"LogAllFiles" },
		{ L"Files", L"LogFiles" },
		{ L"Files", L"LogFailedOpens" },
		{ L"Files", L"MinOpenMs" },
		{ L"Files", L"MaxFileLines" },
		{ L"Watchdog", L"StallSeconds" },
		{ L"Watchdog", L"SampleSeconds" },
		{ L"Watchdog", L"MaxDumps" },
		{ L"Watchdog", L"StackDepth" },
		{ L"Watchdog", L"LookBackSeconds" },
		// Added after the first 20, in the order Liftoff_OSF.psc returns them.
		{ L"Look", L"PanelColor" },
		{ L"Look", L"PanelOpacity" },
		{ L"Look", L"TextColor" },
		{ L"Look", L"FontSize" },
		{ L"Look", L"Uppercase" },
		{ L"Behaviour", L"ShowOnSaveLoad" },
		{ L"Look", L"GameFont" },
		{ L"Tracking", L"SfsePlugins" },
		{ L"Files", L"StartupSummary" },
		{ L"Files", L"SummaryLines" },
	} };

	std::wstring g_iniPath;
	std::string g_lastSnapshot;
	std::atomic<bool> g_inFlight{ false };
	std::atomic<bool> g_started{ false };
	std::atomic<bool> g_warnedUnavailable{ false };
	ULONGLONG g_nextPoll = 0;

	// Splits the '|'-joined values; returns how many there were, or 0 if the text isn't usable.
	std::size_t SplitSnapshot(std::string_view text, std::array<std::string_view, kSettingCount>& fields)
	{
		std::size_t count = 0;
		for (;;) {
			if (count == fields.size())
				return 0;  // more values than Liftoff knows
			const std::size_t separator = text.find('|');
			fields[count++] = text.substr(0, separator);
			if (separator == std::string_view::npos)
				break;
			text.remove_prefix(separator + 1);
		}
		if (count < kMinimumSettingCount)
			return 0;
		for (std::size_t i = 0; i < count; ++i) {
			if (fields[i].find_first_of("\r\n") != std::string_view::npos)
				return 0;
		}
		return count;
	}

	bool IsInteger(std::string_view value)
	{
		int parsed = 0;
		const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
		return error == std::errc{} && end == value.data() + value.size();
	}

	bool IsBoolean(std::string_view value)
	{
		return value == "0" || value == "1";
	}

	bool PersistSnapshot(const std::string& snapshot)
	{
		if (snapshot.empty() || snapshot == g_lastSnapshot)
			return false;

		std::array<std::string_view, kSettingCount> fields{};
		const std::size_t count = SplitSnapshot(snapshot, fields);
		if (count == 0) {
			Log::Write("OSF settings returned an invalid value count or delimiter");
			return false;
		}
		static bool warnedOld = false;
		if (count < kSettingCount && !std::exchange(warnedOld, true))
			Log::Write("OSF settings: Liftoff_OSF.pex is older than this Liftoff (%zu of %zu values); "
			           "the newer settings stay as they are in Liftoff.ini", count, kSettingCount);

		constexpr std::array<std::size_t, 11> booleanIndices{ 0, 3, 4, 5, 6, 10, 11, 12, 24, 27, 28 };
		for (const std::size_t i : booleanIndices) {
			if (i < count && !IsBoolean(fields[i])) {
				Log::Write("OSF settings returned an invalid boolean for %s", Log::Narrow(kSettings[i].key).c_str());
				return false;
			}
		}

		// ShowStatus (2) was a boolean before it became 0-2; an older Liftoff_OSF.pex still sends 0/1.
		constexpr std::array<std::size_t, 14> integerIndices{ 2, 7, 8, 13, 14, 15, 16, 17, 18, 19, 21, 23, 25, 29 };
		for (const std::size_t i : integerIndices) {
			if (i < count && !IsInteger(fields[i])) {
				Log::Write("OSF settings returned an invalid integer for %s", Log::Narrow(kSettings[i].key).c_str());
				return false;
			}
		}

		for (std::size_t i = 0; i < count; ++i) {
			const std::wstring value = Log::Widen(fields[i]);
			if (!WritePrivateProfileStringW(kSettings[i].section, kSettings[i].key, value.c_str(), g_iniPath.c_str())) {
				Log::Write("could not save OSF setting %s to Liftoff.ini (error %lu)",
				           Log::Narrow(kSettings[i].key).c_str(), GetLastError());
				return false;
			}
		}
		if (!WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath.c_str())) {
			Log::Write("could not flush OSF settings to Liftoff.ini (error %lu)", GetLastError());
			return false;
		}

		g_lastSnapshot = snapshot;
		Log::Write("OSF settings saved to Liftoff.ini; they will apply on the next game launch");
		return true;
	}

	class StringCallback final : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		explicit StringCallback(std::function<void(std::string)> callback) :
			_callback(std::move(callback))
		{}

		void CallQueued() override {}
		void CallCanceled() override { g_inFlight.store(false, std::memory_order_release); }
		void StartMultiDispatch() override {}
		void EndMultiDispatch() override {}

		void operator()(RE::BSScript::Variable result) override
		{
			std::string value;
			if (result.is<RE::BSFixedString>()) {
				const auto papyrusString = RE::BSScript::get<RE::BSFixedString>(result);
				if (papyrusString.c_str())
					value = papyrusString.c_str();
			}
			if (_callback)
				_callback(std::move(value));
			g_inFlight.store(false, std::memory_order_release);
		}

	private:
		std::function<void(std::string)> _callback;
	};

	void Poll()
	{
		const ULONGLONG now = GetTickCount64();
		if (now < g_nextPoll || g_inFlight.exchange(true, std::memory_order_acq_rel))
			return;
		g_nextPoll = now + kPollIntervalMs;

		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm) {
			g_inFlight.store(false, std::memory_order_release);
			return;
		}
		if (!vm->TypeIsValid(RE::BSFixedString("Liftoff_OSF")) ||
		    !vm->TypeIsValid(RE::BSFixedString("OSFSettings"))) {
			g_inFlight.store(false, std::memory_order_release);
			if (!g_warnedUnavailable.exchange(true))
				Log::Write("Liftoff_OSF.pex or OSF Settings is not loaded yet; retrying with Liftoff.ini as fallback");
			return;
		}

		auto callback = RE::make_smart<StringCallback>([](std::string value) {
			if (!value.empty())
				PersistSnapshot(value);
		});
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callbackBase = callback;
		const bool queued = vm->DispatchStaticCall(
			RE::BSFixedString("Liftoff_OSF"),
			RE::BSFixedString("ReadSettings"),
			[](RE::BSScrapArray<RE::BSScript::Variable>& arguments) {
				arguments.clear();
				return true;
			},
			callbackBase,
			0);

		if (!queued) {
			g_inFlight.store(false, std::memory_order_release);
			if (!g_warnedUnavailable.exchange(true))
				Log::Write("could not queue OSF Papyrus settings read; keeping Liftoff.ini settings");
		}
	}
}

namespace OsfSettings
{
	void Start(const std::wstring& iniPath)
	{
		if (g_started.exchange(true))
			return;
		g_iniPath = iniPath;

		const SFSE::TaskInterface* tasks = SFSE::GetTaskInterface();
		if (!tasks) {
			g_started.store(false);
			Log::Write("SFSE task interface unavailable; OSF settings will not be synchronized");
			return;
		}

		tasks->AddPermanentTask([] { Poll(); });
		Log::Write("OSF settings bridge started; checking for changes every %llu ms", kPollIntervalMs);
	}
}

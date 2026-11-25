#include "Splash.h"
#include "FileTrace.h"
#include "FileWatch.h"
#include "Log.h"
#include "Progress.h"
#include "SwfFont.h"
#include "Watchdog.h"

#include <windows.h>

// GDI+ (Windows' built-in 2D drawing library) expects min/max to exist.
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

using namespace Gdiplus;

namespace
{
	// Shortens a_text to fit a_maxWidth by cutting from the middle and keeping the extension:
	// "Starfield - Meshes01.ba2" -> "Starfield - Mes....ba2". Text without a short extension is
	// cut at the end instead. a_width measures a string in the same units as a_maxWidth.
	template <class Measure>
	std::wstring FitKeepingExtension(std::wstring a_text, float a_maxWidth, Measure&& a_width)
	{
		if (a_text.empty() || a_width(a_text) <= a_maxWidth)
			return a_text;

		std::wstring tail;
		if (const auto dot = a_text.rfind(L'.');
		    dot != std::wstring::npos && dot > 0 && a_text.size() - dot <= 6 && a_text.find(L' ', dot) == std::wstring::npos) {
			tail = a_text.substr(dot);
			a_text.erase(dot);
		}
		tail.insert(0, L"...");
		while (!a_text.empty() && a_width(a_text + tail) > a_maxWidth)
			a_text.pop_back();
		return a_text + tail;
	}

	enum class Phase { Showing, Holding, Fading, Overlay, SaveLoading, Idle };

	struct State
	{
		Config                  cfg;
		HWND                    hwnd = nullptr;
		int                     width = 0, height = 0;
		std::unique_ptr<Bitmap> backbuffer;   // we draw here, then copy to screen (no flicker)
		Phase                   phase = Phase::Showing;
		ULONGLONG               startTick = 0, phaseTick = 0, lastTopmostTick = 0;
		HWND                    gameWindow = nullptr;
		bool                    gameWindowSeen = false;
		BYTE                    alpha = 255;
		bool                    inSaveLoad = false;  // a save load is running, shown or not

		// Text size in pixels: FontSize is for a 1080p screen and scales with the monitor height.
		float                   em = 20.f;
		// The game's own menu font, read from Data\Interface\fonts_<language>.swf (see SwfFont.h);
		// each glyph as a GDI+ path in font units.
		SwfFont::Font           gameFont;
		bool                    hasGameFont = false;
		std::vector<std::unique_ptr<GraphicsPath>> glyphPaths;
	};

	// Only ever touched by the splash thread (after Start copies the config).
	// Created with new and never deleted on purpose: if the player quits mid-startup,
	// destroying GDI+ objects during process exit could crash the game on the way out.
	State& g = *new State();

	constexpr UINT_PTR kTimerId  = 1;
	constexpr UINT     kFrameMs  = 33;  // ~30 frames per second
	constexpr double   kPi       = 3.14159265358979323846;
	constexpr int      kOverlayWidth = 440;
	constexpr int      kOverlayMargin = 24;
	constexpr ULONGLONG kOverlayHoldMs = 5000;

	void SetOpacity(BYTE alpha)
	{
		const DWORD flags = LWA_ALPHA | (g.cfg.transparentBackground ? LWA_COLORKEY : 0);
		SetLayeredWindowAttributes(g.hwnd, RGB(0, 0, 0), alpha, flags);
	}

	// ------------------------------------------------------------------ the game's font

	void LoadGameFont()
	{
		if (!g.cfg.useStarfieldFont)
			return;
		wchar_t exe[MAX_PATH * 2]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
		const std::filesystem::path gameDir = std::filesystem::path(exe).parent_path();
		wchar_t language[32]{};
		GetPrivateProfileStringW(L"General", L"sLanguage", L"en", language, 32, (gameDir / L"Starfield.ini").c_str());

		const ULONGLONG   started = GetTickCount64();
		std::string       error;
		FileTrace::QuietScope quiet;  // our own reads aren't game loading
		if (!SwfFont::Load(gameDir / L"Data", Log::Narrow(language), Log::Narrow(g.cfg.gameFont), g.gameFont, error)) {
			Log::Write("overlay font: game font not used (%s); using an installed font", error.c_str());
			return;
		}

		// SWF glyphs are quadratic curves; GDI+ paths take cubic Beziers with the same shape.
		g.glyphPaths.reserve(g.gameFont.glyphs.size());
		for (const auto& glyph : g.gameFont.glyphs) {
			auto  path = std::make_unique<GraphicsPath>(FillModeAlternate);
			float x = 0, y = 0;
			bool  open = false;
			for (const auto& c : glyph.commands) {
				switch (c.type) {
				case SwfFont::Command::kMove:
					if (open)
						path->CloseFigure();
					path->StartFigure();
					open = true;
					break;
				case SwfFont::Command::kLine:
					path->AddLine(x, y, c.x, c.y);
					break;
				case SwfFont::Command::kQuad:
					path->AddBezier(x, y, x + (c.cx - x) * 2.f / 3.f, y + (c.cy - y) * 2.f / 3.f,
						c.x + (c.cx - c.x) * 2.f / 3.f, c.y + (c.cy - c.y) * 2.f / 3.f, c.x, c.y);
					break;
				}
				x = c.x;
				y = c.y;
			}
			if (open)
				path->CloseFigure();
			g.glyphPaths.push_back(std::move(path));
		}
		g.hasGameFont = true;
		Log::Write("overlay font: \"%s\" from %s (%zu glyphs, %llu ms)", g.gameFont.name.c_str(),
			g.gameFont.source.c_str(), g.gameFont.glyphs.size(), GetTickCount64() - started);
	}

	// ------------------------------------------------------------------ one frame

	void DrawFrame(HDC hdc)
	{
		const auto snap = Progress::Get();
		const float w = float(g.width), h = float(g.height);

		Graphics gfx(g.backbuffer.get());
		gfx.SetSmoothingMode(SmoothingModeAntiAlias);
		gfx.SetPixelOffsetMode(PixelOffsetModeHighQuality);
		gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

		const Config& c = g.cfg;
		const float   em = g.em;
		if (c.transparentBackground) {
			gfx.Clear(Color(255, 0, 0, 0));
		} else {
			// A flat panel, no border, like a Starfield menu row.
			gfx.Clear(Color(0, 0, 0, 0));
			const BYTE alpha = BYTE(std::clamp(c.panelOpacity, 0, 100) * 255 / 100);
			SolidBrush panel(Color(alpha, GetRValue(c.panelColor), GetGValue(c.panelColor), GetBValue(c.panelColor)));
			gfx.FillRectangle(&panel, 0.f, 0.f, w, h);
		}

		// Installed fonts, when the game's font couldn't be read: NB Architekt if installed, else
		// Bahnschrift (in Windows 10 and 11, a similar square sans). Segoe UI when the Starfield font is off.
		FontFamily architekt(L"NB Architekt");
		FontFamily bahnschrift(L"Bahnschrift");
		FontFamily segoe(L"Segoe UI");
		const FontFamily* family = FontFamily::GenericSansSerif();
		if (c.useStarfieldFont && architekt.IsAvailable())
			family = &architekt;
		else if (c.useStarfieldFont && bahnschrift.IsAvailable())
			family = &bahnschrift;
		else if (segoe.IsAvailable())
			family = &segoe;
		Font font(family, em, FontStyleRegular, UnitPixel);
		SolidBrush textBrush(Color(255, GetRValue(c.textColor), GetGValue(c.textColor), GetBValue(c.textColor)));
		StringFormat left;
		left.SetFormatFlags(StringFormatFlagsNoWrap);
		left.SetTrimming(StringTrimmingEllipsisCharacter);
		left.SetLineAlignment(StringAlignmentCenter);
		StringFormat right;
		right.SetAlignment(StringAlignmentFar);
		right.SetFormatFlags(StringFormatFlagsNoWrap);
		right.SetLineAlignment(StringAlignmentCenter);

		auto shown = [&](std::wstring text) {
			if (c.uppercase)
				CharUpperBuffW(text.data(), DWORD(text.size()));
			return text;
		};

		// Text in the game's font: glyph paths scaled to the text size, laid out with the font's own
		// advances, vertically centred on the capitals, trimmed with "..." when too long.
		const float scale = em / SwfFont::Font::kEm;
		auto glyphOf = [&](wchar_t a_char) -> std::size_t {
			const auto it = g.gameFont.index.find(static_cast<char32_t>(a_char));
			return it == g.gameFont.index.end() ? SIZE_MAX : it->second;
		};
		auto measure = [&](const std::wstring& a_text) {
			float width = 0;
			for (wchar_t ch : a_text) {
				const auto i = glyphOf(ch);
				width += i == SIZE_MAX ? em * 0.5f : g.gameFont.glyphs[i].advance * scale;
			}
			return width;
		};
		auto drawGameText = [&](std::wstring a_text, const RectF& a_rect, bool a_right) {
			if (measure(a_text) > a_rect.Width) {
				while (!a_text.empty() && measure(a_text + L"...") > a_rect.Width)
					a_text.pop_back();
				a_text += L"...";
			}
			float       x = a_right ? a_rect.X + a_rect.Width - measure(a_text) : a_rect.X;
			const float baseline = a_rect.Y + a_rect.Height / 2 + g.gameFont.capHeight * scale / 2;
			for (wchar_t ch : a_text) {
				const auto i = glyphOf(ch);
				if (i == SIZE_MAX) {
					x += em * 0.5f;
					continue;
				}
				Matrix place(scale, 0, 0, scale, x, baseline);
				gfx.SetTransform(&place);
				gfx.FillPath(&textBrush, g.glyphPaths[i].get());
				x += g.gameFont.glyphs[i].advance * scale;
			}
			gfx.ResetTransform();
		};

		// Geometry scales with the font: at 20 px, text starts 26 px in, like the menu's rows.
		const float padX = std::round(em * 1.3f);
		const RectF textRow(padX, std::round(em * 0.55f), w - 2 * padX, std::round(em * 1.4f));
		const RectF statusRect(textRow.X, textRow.Y, textRow.Width * 0.76f, textRow.Height);
		std::wstring label;
		if (c.showPercent) {
			wchar_t buffer[32];
			if (snap.saveLoading)
				swprintf(buffer, 32, L"Loading");
			else if (snap.calibrated || snap.done)
				swprintf(buffer, 32, L"%d%%", int(snap.fraction * 100 + 0.5));
			else
				swprintf(buffer, 32, L"Measuring");
			label = shown(buffer);
		}
		if (c.showStatus != 0) {
			// Status texts are "<activity>  <file name>"; mode 1 keeps only the name.
			std::wstring status = snap.status;
			if (c.showStatus == 1) {
				if (const auto gap = status.find(L"  "); gap != std::wstring::npos)
					status.erase(0, gap + 2);
			}
			status = shown(std::move(status));

			RectF room = statusRect;
			if (g.hasGameFont)  // everything left of the label, with a gap the width of the text size
				room.Width = textRow.Width - (label.empty() ? 0.f : measure(label) + em);
			auto width = [&](const std::wstring& a_text) {
				if (g.hasGameFont)
					return measure(a_text);
				RectF bounds;
				gfx.MeasureString(a_text.c_str(), -1, &font, PointF(0, 0), &left, &bounds);
				return bounds.Width;
			};
			status = FitKeepingExtension(std::move(status), room.Width, width);

			if (g.hasGameFont)
				drawGameText(status, room, false);
			else
				gfx.DrawString(status.c_str(), -1, &font, statusRect, &left, &textBrush);
		}
		if (!label.empty()) {
			if (g.hasGameFont)
				drawGameText(label, textRow, true);
			else
				gfx.DrawString(label.c_str(), -1, &font, textRow, &right, &textBrush);
		}

		const float barX = padX, barW = w - 2 * padX;
		const float barH = std::max(4.f, std::round(em * 0.3f));
		const float barY = textRow.Y + textRow.Height + std::round(em * 0.45f);
		SolidBrush track(Color(110, 0, 0, 0));  // a dark groove, like the gaps between menu rows
		SolidBrush fill(Color(255, GetRValue(c.barColor), GetGValue(c.barColor), GetBValue(c.barColor)));
		gfx.FillRectangle(&track, barX, barY, barW, barH);
		if (!snap.saveLoading && (snap.calibrated || snap.done)) {
			gfx.FillRectangle(&fill, barX, barY, float(barW * snap.fraction), barH);
		} else {
			const double t = double((GetTickCount64() - g.startTick) % 1800) / 1800.0;
			const float blockW = barW * 0.22f;
			const float x = barX + float((std::sin(2 * kPi * t) + 1) / 2) * (barW - blockW);
			gfx.FillRectangle(&fill, x, barY, blockW, barH);
		}

		Graphics screen(hdc);
		screen.DrawImage(g.backbuffer.get(), 0, 0, g.width, g.height);
	}

	// ------------------------------------------------------------------ game window detection

	struct FindArgs { HWND self; HWND found; int width = 0; int height = 0; };

	BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp)
	{
		auto* args = reinterpret_cast<FindArgs*>(lp);
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId() || hwnd == args->self || !IsWindowVisible(hwnd))
			return TRUE;
		RECT r{};
		GetWindowRect(hwnd, &r);
		const int width = r.right - r.left;
		const int height = r.bottom - r.top;
		if (width >= 320 && height >= 240) {
			args->found = hwnd;
			args->width = width;
			args->height = height;
			return FALSE;  // stop searching
		}
		return TRUE;
	}

	HWND FindGameWindow()
	{
		FindArgs args{ g.hwnd };
		EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&args));
		if (args.found && !g.gameWindowSeen)
			Log::Write("game window candidate %p (%dx%d)", args.found, args.width, args.height);
		return args.found;
	}

	void PositionOverlay()
	{
		if (g.phase == Phase::Idle)
			return;
		if (!g.gameWindow || !IsWindow(g.gameWindow) || IsIconic(g.gameWindow))
			return;

		RECT client{};
		POINT origin{ 0, 0 };
		if (!GetClientRect(g.gameWindow, &client) || !ClientToScreen(g.gameWindow, &origin))
			return;

		const int clientW = client.right - client.left;
		const int clientH = client.bottom - client.top;
		const int x = origin.x + std::max(0, std::min(kOverlayMargin, clientW - g.width - kOverlayMargin));
		const int y = origin.y + std::max(0, clientH - g.height - kOverlayMargin);
		SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, g.width, g.height,
		             SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}

	// ------------------------------------------------------------------ timer: the "brain"

	void SetPhase(Phase p)
	{
		g.phase     = p;
		g.phaseTick = GetTickCount64();
	}

	void OnTimer()
	{
		const ULONGLONG now = GetTickCount64();
		const auto progress = Progress::Get();

		if (progress.saveLoading && !g.inSaveLoad) {
			g.inSaveLoad = true;
			// ShowOnSaveLoad: 0 never, 1 only the session's first save load, 2 every save load.
			const bool show = g.cfg.showOnSaveLoad == 2 ||
			                  (g.cfg.showOnSaveLoad == 1 && progress.saveLoadsFinished == 0);
			if (show) {
				Log::Write("save-load status received by splash; showing overlay");
				g.alpha = 255;
				SetOpacity(g.alpha);
				ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
				SetPhase(Phase::SaveLoading);
			} else {
				Log::Write("save-load status received by splash; overlay not shown (ShowOnSaveLoad=%d)", g.cfg.showOnSaveLoad);
			}
		}
		if (!progress.saveLoading && g.inSaveLoad) {
			g.inSaveLoad = false;
			Log::Write("save-load status cleared by splash");
			Watchdog::LoadFinished();
			if (g.phase == Phase::SaveLoading)
				SetPhase(Phase::Holding);
		}

		HWND foundWindow = FindGameWindow();
		if (foundWindow)
			g.gameWindow = foundWindow;

		if (!g.gameWindowSeen && g.gameWindow) {
			g.gameWindowSeen = true;
			Log::Write("game window appeared; attaching bottom-left overlay");
			if (g.phase == Phase::Showing)
				SetPhase(Phase::Overlay);
		}
		PositionOverlay();

		// The game may put its own window on top; politely move ours back above it.
		if (g.phase != Phase::Idle && !g.gameWindowSeen && g.cfg.stayOnTop && now - g.lastTopmostTick > 1000) {
			SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			g.lastTopmostTick = now;
		}

		switch (g.phase) {
		case Phase::Idle:
			break;
		case Phase::Showing:
			if (progress.done)
				SetPhase(Phase::Holding);
			else if (now - g.startTick > ULONGLONG(g.cfg.timeoutSeconds) * 1000) {
				Log::Write("timeout reached, closing splash");
				SetPhase(Phase::Fading);
			}
			break;
		case Phase::Holding:
			if (now - g.phaseTick >= kOverlayHoldMs)
				SetPhase(Phase::Fading);
			break;
		case Phase::Fading: {
			const double t = g.cfg.fadeMs > 0 ? double(now - g.phaseTick) / g.cfg.fadeMs : 1.0;
			if (t >= 1.0) {
				g.alpha = 255;
				ShowWindow(g.hwnd, SW_HIDE);
				SetPhase(Phase::Idle);
				return;
			}
			g.alpha = BYTE(255 * (1.0 - t));
			SetOpacity(g.alpha);
			break;
		}
		case Phase::Overlay:
			if (progress.done) {
				Log::Write("startup finished; overlay will close in 5 seconds");
				SetPhase(Phase::Holding);
			}
			break;
		case Phase::SaveLoading:
			break;
		}

		InvalidateRect(g.hwnd, nullptr, FALSE);
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg) {
		case WM_TIMER:
			if (wp == kTimerId)
				OnTimer();
			return 0;
		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			DrawFrame(hdc);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_ERASEBKGND:
			return 1;  // we paint everything ourselves
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;  // clicking must not steal focus from the game
		case WM_LBUTTONDOWN:
			return 0;  // keep mouse input on the game; clicks never dismiss the overlay
		case WM_DESTROY:
			KillTimer(hwnd, kTimerId);
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// The HMODULE of Liftoff.dll itself (not the game's .exe).
	HINSTANCE ThisModule()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   reinterpret_cast<LPCWSTR>(&ThisModule), &module);
		return module;
	}

	DWORD WINAPI ThreadMain(LPVOID)
	{
		GdiplusStartupInput gdipInput;
		ULONG_PTR gdipToken = 0;
		if (GdiplusStartup(&gdipToken, &gdipInput, nullptr) != Ok) {
			Log::Write("GDI+ failed to start");
			return 1;
		}

		g.startTick = GetTickCount64();
		LoadGameFont();

		// Text size: FontSize is for a 1080p screen, scaled to this monitor's height. The height
		// follows it: text row, bar and margins.
		g.em = float(g.cfg.fontSize) * std::max(0.5f, float(GetSystemMetrics(SM_CYSCREEN)) / 1080.f);
		{
			const float em = g.em;
			const float bottom = std::round(em * 0.55f) + std::round(em * 1.4f) + std::round(em * 0.45f) +
			                     std::max(4.f, std::round(em * 0.3f)) + std::round(em * 0.6f);
			g.width = std::max(kOverlayWidth, int(std::round(em * 25.f)));
			g.height = int(bottom);
		}
		g.backbuffer = std::make_unique<Bitmap>(g.width, g.height, PixelFormat32bppARGB);

		WNDCLASSEXW wc{};
		wc.cbSize        = sizeof(wc);
		wc.lpfnWndProc   = WndProc;
		wc.hInstance     = ThisModule();
		wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
		wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
		wc.hIconSm       = wc.hIcon;
		wc.lpszClassName = L"LiftoffSplash";
		RegisterClassExW(&wc);

		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const int x = work.left + kOverlayMargin;
		const int y = work.bottom - g.height - kOverlayMargin;

		DWORD exStyle = WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_NOACTIVATE;
		if (g.cfg.stayOnTop)
			exStyle |= WS_EX_TOPMOST;

		g.hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"Starfield Startup", WS_POPUP,
		                         x, y, g.width, g.height, nullptr, nullptr, wc.hInstance, nullptr);
		if (!g.hwnd) {
			Log::Write("CreateWindowEx failed (%lu)", GetLastError());
			g.backbuffer.reset();
			GdiplusShutdown(gdipToken);
			return 1;
		}

		if (g.cfg.useStarfieldFont) {
			FontFamily starfield(L"Futura PT");
			if (!starfield.IsAvailable())
				Log::Write("Futura PT is not installed; using Segoe UI instead");
		}
		SetOpacity(255);
		ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
		SetTimer(g.hwnd, kTimerId, kFrameMs, nullptr);
		Log::Write("splash shown (%dx%d)", g.width, g.height);

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		Log::Write("splash closed");
		g.backbuffer.reset();
		GdiplusShutdown(gdipToken);
		return 0;
	}
}

namespace Splash
{
	void Start(const Config& config)
	{
		g.cfg = config;
		HANDLE thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
		else
			Log::Write("could not start splash thread");
	}
}

#include "Splash.h"
#include "FileWatch.h"
#include "Log.h"
#include "Progress.h"
#include "Watchdog.h"

#include <windows.h>

// GDI+ (Windows' built-in 2D drawing library) expects min/max to exist.
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <cmath>
#include <memory>

using namespace Gdiplus;

namespace
{
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
	};

	// Only ever touched by the splash thread (after Start copies the config).
	// Created with new and never deleted on purpose: if the player quits mid-startup,
	// destroying GDI+ objects during process exit could crash the game on the way out.
	State& g = *new State();

	constexpr UINT_PTR kTimerId  = 1;
	constexpr UINT     kFrameMs  = 33;  // ~30 frames per second
	constexpr double   kPi       = 3.14159265358979323846;
	constexpr int      kOverlayWidth = 440;
	constexpr int      kOverlayHeight = 88;
	constexpr int      kOverlayMargin = 24;
	constexpr ULONGLONG kOverlayHoldMs = 5000;

	void SetOpacity(BYTE alpha)
	{
		const DWORD flags = LWA_ALPHA | (g.cfg.transparentBackground ? LWA_COLORKEY : 0);
		SetLayeredWindowAttributes(g.hwnd, RGB(0, 0, 0), alpha, flags);
	}

	// ------------------------------------------------------------------ one frame

	void DrawFrame(HDC hdc)
	{
		const auto snap = Progress::Get();
		const float w = float(g.width), h = float(g.height);

		Graphics gfx(g.backbuffer.get());
		gfx.SetSmoothingMode(SmoothingModeAntiAlias);
		gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

		if (g.cfg.transparentBackground) {
			gfx.Clear(Color(255, 0, 0, 0));
		} else {
			gfx.Clear(Color(0, 0, 0, 0));
			SolidBrush panel(Color(242, 8, 12, 22));
			Pen border(Color(110, 150, 180, 210), 1.0f);
			gfx.FillRectangle(&panel, 0.f, 0.f, w, h);
			gfx.DrawRectangle(&border, 0.5f, 0.5f, w - 1, h - 1);
		}

		FontFamily starfield(L"Futura PT");
		FontFamily segoe(L"Segoe UI");
		const FontFamily* family = g.cfg.useStarfieldFont && starfield.IsAvailable() ? &starfield :
		                           segoe.IsAvailable() ? &segoe : FontFamily::GenericSansSerif();
		Font font(family, 16.f, FontStyleRegular, UnitPixel);
		SolidBrush textBrush(Color(245, 235, 242, 250));
		StringFormat left;
		left.SetFormatFlags(StringFormatFlagsNoWrap);
		left.SetTrimming(StringTrimmingEllipsisCharacter);
		StringFormat right;
		right.SetAlignment(StringAlignmentFar);
		right.SetFormatFlags(StringFormatFlagsNoWrap);

		const RectF textRow(16.f, 12.f, w - 32.f, 24.f);
		const RectF statusRect(textRow.X, textRow.Y, textRow.Width * 0.76f, textRow.Height);
		if (g.cfg.showStatus)
			gfx.DrawString(snap.status.c_str(), -1, &font, statusRect, &left, &textBrush);

		if (g.cfg.showPercent) {
			wchar_t label[32];
			if (snap.saveLoading)
				swprintf(label, 32, L"Loading");
			else if (snap.calibrated || snap.done)
				swprintf(label, 32, L"%d%%", int(snap.fraction * 100 + 0.5));
			else
				swprintf(label, 32, L"Measuring");
			gfx.DrawString(label, -1, &font, textRow, &right, &textBrush);
		}

		const float barX = 16.f, barY = 54.f, barW = w - 32.f, barH = 8.f;
		SolidBrush track(Color(85, 255, 255, 255));
		SolidBrush fill(Color(255, GetRValue(g.cfg.barColor), GetGValue(g.cfg.barColor), GetBValue(g.cfg.barColor)));
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

		if (progress.saveLoading && g.phase != Phase::SaveLoading) {
			Log::Write("save-load status received by splash; showing overlay");
			g.alpha = 255;
			SetOpacity(g.alpha);
			ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
			SetPhase(Phase::SaveLoading);
		}
		if (!progress.saveLoading && g.phase == Phase::SaveLoading) {
			Log::Write("save-load status cleared by splash");
			Watchdog::LoadFinished();
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
		g.width = kOverlayWidth;
		g.height = kOverlayHeight;
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

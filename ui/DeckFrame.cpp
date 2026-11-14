#include "ui/DeckFrame.h"

#include "ui/DeckIcon.h"
#include "ui/Theme.h"
#include "ui/DeckTitleBar.h"
#include "ui/NativeSubclass.h"
#include "ui/WindowFrame.h"
#include "ui/WindowShadow.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <windows.h>
#include <windowsx.h>

namespace ahkdeck
{
namespace ui
{

namespace
{
const wchar_t *kTitleBarProp = L"AHKDeck.TitleBar";
const wchar_t *kFixedSizeProp = L"AHKDeck.FrameFixedSize";
const wchar_t *kMinClientHProp = L"AHKDeck.MinClientH";
constexpr int kMinClientW = 600;
constexpr int kMinClientH = 400;

LRESULT WINAPI DeckFrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_ACTIVATE:
	{
		LRESULT ret = nativeSubclass::forward(hwnd, DeckFrameProc, msg, wp, lp);
		// m185: activation re-orders the layered glow layers. Re-seat every
		// one behind its own owner so a click inside a dialog cannot bury
		// them (see WindowShadow.cpp for the owner-side half of the fix).
		if (LOWORD(wp) != WA_INACTIVE)
			windowShadow::syncAll();
		return ret;
	}

	case WM_SIZE:
	{
		LRESULT ret = nativeSubclass::forward(hwnd, DeckFrameProc, msg, wp, lp);
		if (auto *tb = static_cast<DeckTitleBar *>(GetPropW(hwnd, kTitleBarProp)))
		{
			tb->setMaximized(IsZoomed(hwnd) != 0);
			tb->RefreshHoverFromCursor();
		}
		if (Fl_Window *win = fl_find(hwnd))
		{
			windowChrome::refreshRoundedCorners(win);
			windowShadow::sync(win);
		}
		return ret;
	}

	case WM_WINDOWPOSCHANGED:
	case WM_SHOWWINDOW:
	{
		LRESULT ret = nativeSubclass::forward(hwnd, DeckFrameProc, msg, wp, lp);
		if (msg == WM_SHOWWINDOW)
		{
			if (auto *tb = static_cast<DeckTitleBar *>(GetPropW(hwnd, kTitleBarProp)))
				tb->RefreshHoverFromCursor();
		}
		windowShadow::sync(fl_find(hwnd));
		return ret;
	}

	case WM_DESTROY:
	case WM_NCDESTROY:
		windowShadow::detachHwnd(reinterpret_cast<void *>(hwnd));
		nativeSubclass::detachAll(hwnd);
		RemovePropW(hwnd, kTitleBarProp);
		break;

	case WM_GETMINMAXINFO:
	{
		auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = {sizeof(mi)};
		if (GetMonitorInfoW(mon, &mi))
		{
			const RECT work = mi.rcWork;
			mmi->ptMaxPosition.x = work.left - WindowFrame::NC_PAD;
			mmi->ptMaxPosition.y = work.top;
			mmi->ptMaxSize.x = (work.right - work.left) + 2 * WindowFrame::NC_PAD;
			mmi->ptMaxSize.y = (work.bottom - work.top) + WindowFrame::NC_PAD;
		}
		mmi->ptMinTrackSize.x = kMinClientW + 2 * WindowFrame::NC_PAD;
		// m183: the 400px floor belongs to the main window. A short dialog
		// may opt into a smaller one; Windows was stretching the 300px
		// subscription manager back to 400 and leaving a wall of empty card.
		int minClientH = kMinClientH;
		HANDLE stored = GetPropW(hwnd, kMinClientHProp);
		if (stored)
		{
			const INT_PTR value = reinterpret_cast<INT_PTR>(stored);
			if (value > 0 && value < 2000)
				minClientH = (int)value;
		}
		mmi->ptMinTrackSize.y = minClientH + WindowFrame::NC_PAD;
		return 0;
	}

	case WM_NCCALCSIZE:
		if (wp == TRUE)
		{
			auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(lp);
			p->rgrc[0].left += WindowFrame::NC_PAD;
			p->rgrc[0].right -= WindowFrame::NC_PAD;
			p->rgrc[0].bottom -= WindowFrame::NC_PAD;
			// Top unchanged: the title bar is client row 0 (deck layout).
			return 0;
		}
		break;

	case WM_NCPAINT:
	{
		// Do NOT forward this one. DefWindowProc paints the default NC frame
		// from the system metrics (a ~5px sizing frame) even though our
		// WM_NCCALCSIZE only reserved 1px; forwarding left that band drawn
		// INSIDE the client - the grey ring the user reported as "shadow
		// inside the window". The WndProc here paints and returns 0 too.
		if (Fl_Window *win = fl_find(hwnd))
			windowChrome::paintNonClientFrame(win, windowChrome::windowBorderColour(win));
		return 0;
	}

	case WM_PAINT:
	{
		// Repaint the border AFTER FLTK blits its client buffer (the top run and
		// the inner half of the arcs live in the client area).
		LRESULT ret = nativeSubclass::forward(hwnd, DeckFrameProc, msg, wp, lp);
		if (Fl_Window *win = fl_find(hwnd))
			windowChrome::paintNonClientFrame(win, windowChrome::windowBorderColour(win));
		return ret;
	}

	case WM_NCACTIVATE:
		return 1;

	case WM_NCLBUTTONDOWN:
		// Our WM_NCHITTEST returns HTLEFT/HTTOP/... for a grip that is mostly
		// INSIDE the client area. The sizing loop is entered by DefWindowProc
		// on this message; do not let a lower proc swallow it (the cursor
		// would change while the drag did nothing).
		return DefWindowProcW(hwnd, msg, wp, lp);

	case WM_NCHITTEST:
	{
		POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		RECT rc;
		GetWindowRect(hwnd, &rc);
		if (pt.x < rc.left || pt.x >= rc.right || pt.y < rc.top || pt.y >= rc.bottom)
			return nativeSubclass::forward(hwnd, DeckFrameProc, msg, wp, lp);
		if (IsZoomed(hwnd))
			return HTCLIENT;
		if (GetPropW(hwnd, kFixedSizeProp))
			return HTCLIENT; // a dialog is not resizable: no grips at all

		const int grip = windowChrome::pointOverScrollbar(
							 fl_find(hwnd), pt.x - rc.left, pt.y - rc.top)
						 ? WindowFrame::RESIZE_ZONE_OVER_SCROLLBAR
						 : WindowFrame::RESIZE_ZONE;
		const bool onTop = pt.y < rc.top + grip;
		const bool onLeft = pt.x < rc.left + grip;
		const bool onRight = pt.x >= rc.right - grip;
		const bool onBottom = pt.y >= rc.bottom - grip;

		if (onTop && onLeft)
			return HTTOPLEFT;
		if (onTop && onRight)
			return HTTOPRIGHT;
		if (onLeft && onBottom)
			return HTBOTTOMLEFT;
		if (onRight && onBottom)
			return HTBOTTOMRIGHT;
		if (onTop)
			return HTTOP;
		if (onLeft)
			return HTLEFT;
		if (onRight)
			return HTRIGHT;
		if (onBottom)
			return HTBOTTOM;
		return HTCLIENT;
	}

	default:
		break;
	}

	return nativeSubclass::forward(hwnd, DeckFrameProc, msg, wp, lp);
}

// The one frame body. `fixedSize` is the only difference between the main window
// and a dialog: a dialog gets no sizing grips, no min/max boxes and no taskbar
// button, and everything else - region, ring, glow, icon, repaint - runs the
// same code, so the two can never drift apart.
void InstallFrameCommon(Fl_Window *win, DeckTitleBar *titleBar, unsigned int borderColor,
	bool rounded, bool shadow, bool fixedSize, bool onTaskbar, int aMinClientH)
{
	HWND hwnd = win ? fl_xid(win) : nullptr;
	if (!hwnd)
		return;

	// border(0) makes FLTK create a WS_POPUP | WS_EX_TOOLWINDOW window. The main
	// window puts itself back on the taskbar; a modal dialog stays off it -
	// including a resizable one, where a taskbar button would put a second
	// entry for a window that is not the application.
	LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (onTaskbar)
	{
		exStyle &= ~WS_EX_TOOLWINDOW;
		exStyle |= WS_EX_APPWINDOW;
	}
	else
	{
		exStyle |= WS_EX_TOOLWINDOW;
		exStyle &= ~WS_EX_APPWINDOW;
	}
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

	nativeSubclass::install(hwnd, DeckFrameProc);
	if (titleBar)
		SetPropW(hwnd, kTitleBarProp, titleBar);
	if (fixedSize)
		SetPropW(hwnd, kFixedSizeProp, reinterpret_cast<HANDLE>(1));
	else
		RemovePropW(hwnd, kFixedSizeProp);
	if (aMinClientH > 0)
		SetPropW(hwnd, kMinClientHProp,
			reinterpret_cast<HANDLE>(static_cast<INT_PTR>(aMinClientH)));
	else
		RemovePropW(hwnd, kMinClientHProp);

	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	style |= WS_SYSMENU;
	if (!fixedSize)
	{
		style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
		// No taskbar button means a minimized window cannot be brought back,
		// so the minimize box only exists where onTaskbar does.
		if (onTaskbar)
			style |= WS_MINIMIZEBOX;
		else
			style &= ~WS_MINIMIZEBOX;
	}
	SetWindowLongPtrW(hwnd, GWL_STYLE, style);

	// Grow the outer rect so the CLIENT area keeps the size the caller asked
	// for; WM_NCCALCSIZE then gives the 1px border somewhere to live.
	RECT rc;
	GetWindowRect(hwnd, &rc);
	SetWindowPos(hwnd, nullptr,
		rc.left - WindowFrame::NC_PAD, rc.top,
		(rc.right - rc.left) + 2 * WindowFrame::NC_PAD,
		(rc.bottom - rc.top) + WindowFrame::NC_PAD,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

	windowChrome::setRoundedCornersEnabled(rounded);
	windowChrome::applyRoundedCorners(win, rounded, borderColor);
	windowChrome::takeOverFrame(win);
	windowShadow::setEnabled(shadow);
	windowShadow::attach(win);

	// border(0) strips the caption and its system icon, so re-apply both sizes
	// from the exe resource (m030). The tray already extracts icon group 0.
	HINSTANCE instance = GetModuleHandleW(nullptr);
	HICON iconSmall = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_AHKDECK),
		IMAGE_ICON, 16, 16, LR_SHARED);
	HICON iconBig = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_AHKDECK),
		IMAGE_ICON, 32, 32, LR_SHARED);
	if (iconSmall)
		SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSmall);
	if (iconBig)
		SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)iconBig);

	SetForegroundWindow(hwnd);
	RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
}
} // namespace

void InstallDeckFrame(Fl_Window *win, DeckTitleBar *titleBar, unsigned int borderColor,
	bool rounded, bool shadow)
{
	InstallFrameCommon(win, titleBar, borderColor, rounded, shadow, false, true, 0);
}

void InstallDialogFrame(Fl_Window *win, DeckTitleBar *titleBar, unsigned int borderColor,
	bool rounded, bool shadow, bool aResizable, int aMinClientH)
{
	// A resizable dialog keeps the dialog's ex-style (no taskbar button) but
	// gains the sizing frame and the maximize box (m179).
	InstallFrameCommon(win, titleBar, borderColor, rounded, shadow,
		!aResizable, false, aMinClientH);
}

void ToggleDeckMaximize(Fl_Window *win)
{
	HWND hwnd = win ? fl_xid(win) : nullptr;
	if (!hwnd)
		return;
	ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
	windowChrome::refreshRoundedCorners(win);
	windowShadow::sync(win);
}

void SyncDeckFrame(Fl_Window *win)
{
	windowChrome::refreshRoundedCorners(win);
	windowShadow::sync(win);
}

int DeckClientWidth(Fl_Window *win)
{
	HWND hwnd = win ? fl_xid(win) : nullptr;
	if (hwnd)
	{
		RECT rc;
		if (GetClientRect(hwnd, &rc))
			return rc.right - rc.left;
	}
	return win ? win->w() : 0;
}

int DeckClientHeight(Fl_Window *win)
{
	HWND hwnd = win ? fl_xid(win) : nullptr;
	if (hwnd)
	{
		RECT rc;
		if (GetClientRect(hwnd, &rc))
			return rc.bottom - rc.top;
	}
	return win ? win->h() : 0;
}

void PaintDeckClientBorder(Fl_Window *win)
{
	// The split layout: WM_NCPAINT draws the left/right/bottom runs and the
	// outer half of the arcs; this draws the top run and the inner half.
	const Fl_Color pen = ThemeBorderLine();
	fl_color(pen);
	if (windowChrome::roundedCornersActive(win))
	{
		const int rad = windowChrome::roundedCornerRadiusPx(win);
		const int x0 = rad - WindowFrame::NC_PAD;
		const int len = DeckClientWidth(win) - 2 * (rad - WindowFrame::NC_PAD);
		if (len > 0)
			fl_rectf(x0, 0, len, 1);

		static windowChrome::FramePixel pix[1024];
		const int n = windowChrome::buildCornerStroke(win, (unsigned int)pen,
			WindowFrame::NC_PAD, 0, pix, (int)(sizeof(pix) / sizeof(pix[0])));
		for (int i = 0; i < n; ++i)
		{
			fl_color((Fl_Color)pix[i].colour);
			fl_rectf(pix[i].x, pix[i].y, 1, 1);
		}
	}
	else
	{
		fl_rectf(0, 0, DeckClientWidth(win), 1);
	}
}

} // namespace ui
} // namespace ahkdeck

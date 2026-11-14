// DeckFrame.h - installs the borderless frame on the deck window.
//
// The visual stack lives in ui/WindowFrame + ui/WindowShadow: the window is
// border(0), a rounded region clips it, a 1px border is hand-painted, and a
// layered companion window carries the anti-aliased ring and the outer glow.
// This file only owns the Win32 message plumbing that a borderless top-level
// window needs: NC metrics, resize hit-test, NC painting, maximize bounds.
//
// Everything here is best-effort: without a native handle it is a no-op, so the
// GUI-less probes and ctest stay unaffected.
#pragma once

class Fl_Window;

namespace ahkdeck
{
namespace ui
{

class DeckTitleBar;

// Call after show(): a borderless window has no HWND before that. `titleBar`
// receives maximize/restore state from the native zoom state; it may be null.
// `rounded` / `shadow` are the persisted View-menu switches (m027).
void InstallDeckFrame(Fl_Window *win, DeckTitleBar *titleBar, unsigned int borderColor,
	bool rounded, bool shadow);

// The same frame for a dialog: no taskbar button. One plumbing body serves
// both, so a dialog can never grow a different border, ring or glow from the
// main window. m179: `aResizable` turns on the sizing frame and the maximize
// box for a dialog whose content genuinely gets bigger (获取脚本); the taskbar
// button stays off, and so does minimize - a window with no taskbar button
// cannot be restored once minimized.
void InstallDialogFrame(Fl_Window *win, DeckTitleBar *titleBar, unsigned int borderColor,
	bool rounded, bool shadow, bool aResizable = false, int aMinClientH = 0);

// Maximize / restore through the native zoom state (the title bar button and
// the system menu both land here).
void ToggleDeckMaximize(Fl_Window *win);

// Re-sync the companion shadow after a move / resize / show / hide.
void SyncDeckFrame(Fl_Window *win);

// The client size Win32 reports. Installing the frame grows the OUTER rect by
// the 1px strip, so w()/h() are not the client size and any layout that uses
// them drifts by one pixel per edge.
int DeckClientWidth(Fl_Window *win);
int DeckClientHeight(Fl_Window *win);

// Repaint the 1px top run and the inner half of the corner arcs INSIDE the
// client area. The border has no non-client home on the top edge (the caption
// bar is client row 0), so every window that installs the deck frame - the main
// window and each dialog - must do this after its own client blit. One function,
// because it is one element.
void PaintDeckClientBorder(Fl_Window *win);

} // namespace ui
} // namespace ahkdeck

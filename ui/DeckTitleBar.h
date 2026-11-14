// DeckTitleBar.h - the custom caption bar that replaces the native one.
//
// The frame (WindowFrame/WindowShadow) is built for a border(0) window:
// the window has no OS caption, so dragging, maximizing and closing are the
// app's job. This is the smallest title bar that keeps the deck usable:
// title text plus minimize / maximize-restore / close, themed with the same
// palette as the rest of the window.
#pragma once

#include <FL/Fl_Widget.H>
#include <functional>
#include <string>

#include "ui/BandColourSource.h"

namespace ahkdeck
{
namespace ui
{

class DeckTitleBar : public Fl_Widget, public windowChrome::BandColourSource
{
public:
	enum { kHeight = 32 };

	// Which caption buttons the bar owns. The main window is kAll; a plain
	// dialog has nothing to minimize or maximize, so it shows the close box
	// only. m179: a resizable dialog (获取脚本) gets kMaxClose - maximize is
	// useful there, but a tool-window dialog has no taskbar button to restore
	// from, so it must never offer minimize. This is the ONE place that count
	// is decided - draw() and HitTest() both read it, so a dialog can never
	// paint three buttons and answer to one.
	enum Caption
	{
		kCaptionAll = 3,
		kCaptionMaxClose = 2,
		kCaptionCloseOnly = 1
	};

	explicit DeckTitleBar(int x, int y, int w, int h);

	void setCaption(Caption caption)
	{
		mCaption = caption;
		redraw();
	}
	Caption caption() const { return mCaption; }

	void setMaximized(bool on);
	bool maximized() const { return mMaximized; }

	void setOnMinimize(std::function<void()> fn) { mOnMinimize = std::move(fn); }
	void setOnToggleMaximize(std::function<void()> fn) { mOnToggleMaximize = std::move(fn); }
	void setOnClose(std::function<void()> fn) { mOnClose = std::move(fn); }

	void draw() override;
	int handle(int event) override;
	bool bandColourAt(int x, int y, unsigned int *colour) const override;

	// m112: FLTK only sends FL_ENTER/FL_LEAVE when the pointer MOVES. A window
	// that gets minimized or hidden never receives the matching FL_LEAVE, so a
	// hovered caption button could stay tinted after the window came back with
	// the pointer somewhere else. Recompute the hover from the real cursor;
	// the frame proc calls this on WM_SIZE / WM_SHOWWINDOW.
	void RefreshHoverFromCursor();

private:
	enum class Hit { None, Minimize, Maximize, Close };

	Hit HitTest(int mx, int my) const;
	// m114: the hover state is recomputed from the real pointer every time the
	// bar paints, so a repaint triggered by show()/restore can never put back a
	// tint that the pointer no longer justifies.
	Hit CursorHit() const;
	void DrawButton(int x, int w, Hit hit) const;
	Fl_Color Background() const;

	std::function<void()> mOnMinimize;
	std::function<void()> mOnToggleMaximize;
	std::function<void()> mOnClose;

	bool mMaximized = false;
	Caption mCaption = kCaptionAll;
	Hit mHover = Hit::None;
	Hit mPressed = Hit::None;
	bool mDragging = false;
	int mDragStartX = 0;
	int mDragStartY = 0;
	int mWindowX = 0;
	int mWindowY = 0;
};

} // namespace ui
} // namespace ahkdeck

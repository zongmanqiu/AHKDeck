// DeckTitleBar.h - the custom caption bar that replaces the native one.
//
// Pecia's frame (WindowFrame/WindowShadow) is built for a border(0) window:
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

	// Which caption buttons the bar owns. The main window is kAll; a dialog has
	// nothing to minimize or maximize, so it shows the close box only. This is
	// the ONE place that count is decided - both draw() and HitTest() read it,
	// so a dialog can never paint three buttons and answer to one.
	enum Caption { kCaptionAll = 3, kCaptionCloseOnly = 1 };

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

private:
	enum class Hit { None, Minimize, Maximize, Close };

	Hit HitTest(int mx, int my) const;
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

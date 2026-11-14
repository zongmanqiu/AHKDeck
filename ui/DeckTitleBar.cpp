#include "ui/DeckTitleBar.h"

#include "ui/DeckIcon.h"
#include "ui/Theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <windows.h>

#include <algorithm>

namespace ahkdeck
{
namespace ui
{

namespace
{
constexpr int kButtonW = 32;
constexpr int kIconMargin = 6;
constexpr int kIconSz = 18;
const Fl_Color kCloseHover = CloseHoverRed();
} // namespace

DeckTitleBar::DeckTitleBar(int x, int y, int w, int h)
	: Fl_Widget(x, y, w, h, nullptr)
{
	box(FL_FLAT_BOX);
}

void DeckTitleBar::setMaximized(bool on)
{
	if (mMaximized != on)
	{
		mMaximized = on;
		redraw();
	}
}

Fl_Color DeckTitleBar::Background() const
{
	return DeckColors().background2;
}

DeckTitleBar::Hit DeckTitleBar::HitTest(int mx, int my) const
{
	if (my < 0 || my >= h())
		return Hit::None;
	const int right = w();
	const int closeX = right - kButtonW;
	if (mx >= closeX)
		return Hit::Close;
	if (mCaption == kCaptionCloseOnly)
		return Hit::None;
	const int maxX = closeX - kButtonW;
	if (mx >= maxX)
		return Hit::Maximize;
	if (mCaption == kCaptionMaxClose)
		return Hit::None;
	const int minX = maxX - kButtonW;
	if (mx >= minX)
		return Hit::Minimize;
	return Hit::None;
}

void DeckTitleBar::DrawButton(int bx, int bw, Hit hit) const
{
	const bool hovered = (mHover == hit);
	const bool pressed = (mPressed == hit);
	Fl_Color fill = hovered ? DeckColors().highlight1 : Background();
	if (pressed)
		fill = DeckColors().highlight2;
	if (hovered && hit == Hit::Close)
		fill = kCloseHover;

	fl_color(fill);
	fl_rectf(bx, y(), bw, h());

	Fl_Color ink = DeckColors().text1;
	if (hovered && hit == Hit::Close)
		ink = FL_WHITE;
	fl_color(ink);

	const int cx = bx + bw / 2;
	const int cy = y() + h() / 2;
	if (hit == Hit::Minimize)
	{
		fl_line_style(FL_SOLID, 1);
		fl_line(cx - 5, cy + 4, cx + 5, cy + 4);
	}
	else if (hit == Hit::Maximize)
	{
		fl_line_style(FL_SOLID, 1);
		if (mMaximized)
		{
			fl_rect(cx - 5, cy - 5, 9, 9);
			fl_rect(cx - 2, cy - 2, 9, 9);
		}
		else
		{
			fl_rect(cx - 5, cy - 5, 10, 10);
		}
	}
	else if (hit == Hit::Close)
	{
		fl_line_style(FL_SOLID, 1);
		fl_line(cx - 4, cy - 4, cx + 4, cy + 4);
		fl_line(cx + 4, cy - 4, cx - 4, cy + 4);
	}
	fl_line_style(0);
}

void DeckTitleBar::draw()
{
	// The pointer decides the tint, not a remembered flag: windows that were
	// hidden, minimised or moved out from under the mouse never got their
	// FL_LEAVE, and every repaint after returning would otherwise re-tint the
	// button the user last clicked.
	mHover = CursorHit();

	fl_color(Background());
	fl_rectf(x(), y(), w(), h());

	// App icon (m030): the AutoHotkey "H" from the pinned source. LoadImage
	// keeps several sizes; cache the one for this DPI and let DrawIconEx do
	// the per-pixel scaling FLTK cannot do through fl_draw.
	static HICON sIcon = nullptr;
	static int sIconPx = 0;
	const float scale = window() ? Fl::screen_scale(window()->screen_num()) : 1.0f;
	const int dev = int(kIconSz * scale + 0.5f);
	if (!sIcon || sIconPx != dev)
	{
		if (sIcon)
			DestroyIcon(sIcon);
		sIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
			MAKEINTRESOURCEW(IDI_AHKDECK), IMAGE_ICON, dev, dev, 0);
		sIconPx = dev;
	}
	if (sIcon)
	{
		const int iconX = x() + kIconMargin;
		const int iconY = y() + (h() - kIconSz) / 2;
		DrawIconEx(fl_gc, int(iconX * scale), int(iconY * scale), sIcon,
			dev, dev, 0, nullptr, DI_NORMAL);
	}

	const int buttons = (mCaption == kCaptionCloseOnly) ? 1
		: (mCaption == kCaptionMaxClose ? 2 : 3);
	const char *title = window() ? window()->label() : "";
	if (title && *title)
	{
		fl_color(DeckColors().text1);
		fl_font(FL_HELVETICA, 16);
		const int textX = x() + kIconMargin + kIconSz + 8;
		fl_draw(title, textX, y(), x() + w() - textX - buttons * kButtonW - 8, h(),
			FL_ALIGN_LEFT | FL_ALIGN_CLIP);
	}

	const int right = x() + w();
	if (buttons == 3)
	{
		DrawButton(right - 3 * kButtonW, kButtonW, Hit::Minimize);
		DrawButton(right - 2 * kButtonW, kButtonW, Hit::Maximize);
	}
	else if (buttons == 2)
	{
		DrawButton(right - 2 * kButtonW, kButtonW, Hit::Maximize);
	}
	DrawButton(right - kButtonW, kButtonW, Hit::Close);
}

int DeckTitleBar::handle(int event)
{
	const int mx = Fl::event_x();
	const int my = Fl::event_y();
	switch (event)
	{
	case FL_ENTER:
	case FL_MOVE:
	{
		const Hit hit = HitTest(mx, my);
		if (hit != mHover)
		{
			mHover = hit;
			redraw();
		}
		return 1;
	}
	case FL_LEAVE:
		if (mHover != Hit::None)
		{
			mHover = Hit::None;
			redraw();
		}
		return 1;
	case FL_PUSH:
	{
		const Hit hit = HitTest(mx, my);
		if (hit != Hit::None)
		{
			mPressed = hit;
			redraw();
			return 1;
		}
		if (Fl::event_button() != FL_LEFT_MOUSE)
			return 1;
		if (Fl::event_clicks() > 0)
		{
			Fl::event_clicks(0);
			if (mOnToggleMaximize)
				mOnToggleMaximize();
			return 1;
		}
		if (!mMaximized && window())
		{
			mDragging = true;
			mDragStartX = Fl::event_x_root();
			mDragStartY = Fl::event_y_root();
			mWindowX = window()->x();
			mWindowY = window()->y();
		}
		return 1;
	}
	case FL_DRAG:
		if (mDragging && window())
		{
			const int dx = Fl::event_x_root() - mDragStartX;
			const int dy = Fl::event_y_root() - mDragStartY;
			window()->position(mWindowX + dx, mWindowY + dy);
			return 1;
		}
		return 1;
	case FL_RELEASE:
	{
		const Hit hit = HitTest(mx, my);
		const Hit pressed = mPressed;
		mPressed = Hit::None;
		mDragging = false;
		redraw();
		if (pressed != Hit::None && hit == pressed)
		{
			if (pressed == Hit::Minimize && mOnMinimize)
				mOnMinimize();
			else if (pressed == Hit::Maximize && mOnToggleMaximize)
				mOnToggleMaximize();
			else if (pressed == Hit::Close && mOnClose)
				mOnClose();
		}
		return 1;
	}
	default:
		return Fl_Widget::handle(event);
	}
}

DeckTitleBar::Hit DeckTitleBar::CursorHit() const
{
	Fl_Window *win = window();
	if (!win || !win->visible())
		return Hit::None;
	HWND hwnd = fl_xid(win);
	if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd))
		return Hit::None;
	POINT pt = {0, 0};
	if (!GetCursorPos(&pt))
		return Hit::None;
	// Only our own surface counts: with a popup or another app on top the
	// pointer is over that window, not over this button.
	if (WindowFromPoint(pt) != hwnd)
		return Hit::None;
	ScreenToClient(hwnd, &pt);
	return HitTest(pt.x - x(), pt.y - y());
}

void DeckTitleBar::RefreshHoverFromCursor()
{
	const Hit hit = CursorHit();
	if (hit != mHover)
	{
		mHover = hit;
		redraw();
	}
}

bool DeckTitleBar::bandColourAt(int px, int py, unsigned int *colour) const
{
	if (!colour)
		return false;
	Hit hit = HitTest(px, py);
	Fl_Color fill = Background();
	if (hit == mHover && hit != Hit::None)
		fill = (hit == Hit::Close) ? kCloseHover : DeckColors().highlight1;
	if (hit == mPressed && hit != Hit::None)
		fill = DeckColors().highlight2;
	*colour = (unsigned int)fill;
	return true;
}

} // namespace ui
} // namespace ahkdeck

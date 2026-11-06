#pragma once

#include "ui/Theme.h"

#include <FL/fl_draw.H>
#include <initializer_list>

namespace ahkdeck
{
namespace ui
{

// Flat button with the Pecia hover/pressed fill (no 3D bevel).
class HoverButton : public Fl_Button
{
public:
	HoverButton(int x, int y, int w, int h, const char *label = nullptr)
		: Fl_Button(x, y, w, h, label)
	{
		box(FL_BORDER_BOX);
	}

	void draw() override
	{
		Fl_Color fill = color();
		// A deactivated button is a permanent-but-unavailable action: no hover
		// or press fill until it is activated again.
		if (active() && (Fl::belowmouse() == this || Fl::pushed() == this))
		{
			fill = DeckColors().highlight1;
			if (Fl::pushed() == this)
				fill = DeckColors().highlight2;
		}
		draw_box(box(), fill);
		draw_label();
	}

	int handle(int event) override
	{
		if (event == FL_ENTER || event == FL_LEAVE)
			redraw();
		return Fl_Button::handle(event);
	}

	// Pecia's one width rule for auto-sized dialog buttons: label + 12px each
	// side, never narrower than 48px.
	int fitWidth() const
	{
		int textW = 0;
		int textH = 0;
		measure_label(textW, textH);
		const int w = textW + 2 * kPadX;
		return w < kMinW ? kMinW : w;
	}

	void fit() { resize(x(), y(), fitWidth(), h()); }

	static constexpr int kPadX = 12;
	static constexpr int kMinW = 48;
};

// Right-aligned row of fitted buttons, vertically centred on `aBarCentreY`
// with a 12px right margin and 8px between buttons. The list order is the
// visual left-to-right order, so the negative button goes last (rightmost).
inline void FitButtonRow(std::initializer_list<Fl_Button *> aButtons,
	int aRightEdge, int aBarCentreY, int aMargin, int aGap)
{
	for (Fl_Button *button : aButtons)
		if (button)
			static_cast<HoverButton *>(button)->fit();
	int x = aRightEdge - aMargin;
	Fl_Button *const *begin = aButtons.begin();
	for (size_t i = aButtons.size(); i-- > 0;)
	{
		Fl_Button *button = begin[i];
		if (!button)
			continue;
		button->position(x - button->w(), aBarCentreY - button->h() / 2);
		x -= button->w() + aGap;
	}
}

// An Fl_Scroll whose bars are the ones above. Fl_Scroll builds its bars
// internally, so any dialog that used a plain Fl_Scroll showed the stock bevel
// the list and the editor had already replaced (m071 A2). Fl_Browser and
// Fl_Text_Display are handled the same way: let the base draw, then repaint the
// bar area.
class PeciaScroll : public Fl_Scroll
{
public:
	PeciaScroll(int x, int y, int w, int h, const char *label = nullptr)
		: Fl_Scroll(x, y, w, h, label)
	{
	}

	void draw() override
	{
		Fl_Scroll::draw();
		fl_push_clip(x(), y(), w(), h());
		DrawPeciaScrollbarCorner(&scrollbar, &hscrollbar);
		DrawPeciaScrollbar(&scrollbar, false);
		DrawPeciaScrollbar(&hscrollbar, true);
		fl_pop_clip();
	}
};

} // namespace ui
} // namespace ahkdeck

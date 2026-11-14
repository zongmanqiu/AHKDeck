#pragma once

#include "ui/Theme.h"
#include "ui/UiMetrics.h"

#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Rect.H>
#include <FL/fl_draw.H>
#include <initializer_list>

// The popup menus' check-mark colour (Theme.cpp). The menu bar and every
// popup use it, so a check box that wants to look like the menu's check mark
// has to use it too.
extern Fl_Color g_popupCheckColor;

// The one flat 1px line colour every box in the deck is drawn with
// (Theme.cpp). The check box needs it for its own border.
extern Fl_Color g_borderColour;

namespace ahkdeck
{
namespace ui
{

// m086: a check box drawn EXACTLY like the menu bar's check mark.
//
// FLTK's own Fl_Check_Button draws a filled box with a tick in a contrasting
// colour - and with the "gtk+" scheme the tick ends up black. That is why the
// find bar's switches looked foreign next to our menus. This widget copies
// the menu's drawing (Fl_Menu_Item::draw in the patched FLTK):
//
//   unchecked -> an empty square with one 1px border
//   checked   -> the same square, filled with the menu's check colour
//   the label sits W + 6px to the right of the box
//
// m087: the menu draws the UNCHECKED box as FL_BORDER_FRAME, and the theme
// replaces that with a 1px outer line PLUS a 1px inner black line - two
// lines, so the empty box looked wider than the filled one and the switch
// changed width when it was ticked. Both states are now drawn by hand with
// the SAME single 1px border (g_borderColour), so the box keeps its width
// and only the fill changes.
class DeckCheckButton : public Fl_Button
{
public:
	DeckCheckButton(int x, int y, int w, int h, const char *label = nullptr)
		: Fl_Button(x, y, w, h, label)
	{
		type(FL_TOGGLE_BUTTON);
		align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		box(FL_NO_BOX);
	}

	// The menu's own geometry: a 14px box centred in the row.
	static int BoxSize(int aHeight)
	{
		const int d = (aHeight - FL_NORMAL_SIZE + 1) / 2;
		return aHeight - 2 * d;
	}

	// The gap between the box and the label (the menu uses 3 + 3).
	static constexpr int kLabelGap = 6;

	void draw() override
	{
		const int d = (h() - FL_NORMAL_SIZE + 1) / 2;
		const int size = h() - 2 * d;
		const int bx = x() + 2;
		const int by = y() + d;
		const Fl_Color tick = g_popupCheckColor ? g_popupCheckColor
			: DeckColors().highlight2;
		if (value())
		{
			fl_color(tick);
			fl_rectf(bx, by, size, size);
		}
		// One border, one colour, both states: the box never changes width.
		fl_color(g_borderColour);
		fl_rect(bx, by, size, size);
		draw_label(x() + size + kLabelGap, y(), w() - size - kLabelGap, h());
	}

	int handle(int event) override
	{
		if (event == FL_ENTER || event == FL_LEAVE)
			redraw();
		return Fl_Button::handle(event);
	}
};

// Flat button with the deck hover/pressed fill (no 3D bevel).
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
		// A deactivated button is a permanent-but-unavailable action: no hover
		// or press fill until it is activated again. The fill rule itself lives
		// in UiMetrics so hand-painted cells and widgets cannot drift apart.
		const bool hovered = active() && Fl::belowmouse() == this;
		const bool pressed = active() && Fl::pushed() == this;
		draw_box(box(), UiButtonFill(color(), hovered || pressed, pressed));
		draw_label();
	}

	int handle(int event) override
	{
		if (event == FL_ENTER || event == FL_LEAVE)
			redraw();
		return Fl_Button::handle(event);
	}

	// One width rule for auto-sized dialog buttons: label + 12px each
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

	static constexpr int kPadX = kUiButtonPadX;
	static constexpr int kMinW = kUiButtonMinW;
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
class DeckScroll : public Fl_Scroll
{
public:
	DeckScroll(int x, int y, int w, int h, const char *label = nullptr)
		: Fl_Scroll(x, y, w, h, label)
	{
	}

	void draw() override
	{
		Fl_Scroll::draw();
		fl_push_clip(x(), y(), w(), h());
		DeckDrawScrollbarCorner(&scrollbar, &hscrollbar);
		DeckDrawScrollbar(&scrollbar, false);
		DeckDrawScrollbar(&hscrollbar, true);
		fl_pop_clip();
	}
};

// m148: pecia's dropdown (see .ex/pecia/ui/ThemeWidgets.h:280). A stock
// Fl_Choice hardcodes its own box type, so under an FLTK scheme it paints a
// raised grey button next to our flat inputs. This one draws a rectangular
// FL_BORDER_BOX, a 1px divider and a themed choice arrow itself; the selected
// item is drawn with Fl_Menu_Item::draw so shortcuts/highlighting still work.
// The source of the arrow colour is labelcolor(), the divider uses the same
// g_borderColour as every other flat frame in the deck.
class SettingsChoice : public Fl_Choice
{
public:
	SettingsChoice(int aX, int aY, int aW, int aH, const char *aLabel = nullptr)
		: Fl_Choice(aX, aY, aW, aH, aLabel)
	{
	}

	void draw() override
	{
		const int dx = Fl::box_dx(box());
		const int dy = Fl::box_dy(box());
		draw_box(box(), color());

		const int arrowW = 20;
		const int arrowH = h() - 2 * dy;
		const int arrowX = x() + w() - arrowW - dx;
		const int arrowY = y() + dy;
		const bool active = active_r();
		const Fl_Color arrowColor = active ? labelcolor() : fl_inactive(labelcolor());

		fl_color(g_borderColour);
		fl_yxline(arrowX, y() + dy, y() + h() - dy);

		Fl_Rect arrowRect(arrowX, arrowY, arrowW, arrowH);
		arrowRect.inset(2, 0, 2, 0);
		fl_draw_arrow(arrowRect, FL_ARROW_CHOICE, FL_ORIENT_NONE, arrowColor);

		const int takenW = arrowW + 2 * dx;
		if (mvalue())
		{
			Fl_Menu_Item item = *mvalue();
			if (active)
				item.activate();
			else
				item.deactivate();
			const int xx = x() + dx, yy = y() + dy + 1;
			const int ww = w() - takenW, hh = arrowH - 2;
			fl_push_clip(xx, yy, ww, hh);
			fl_draw_shortcut = 2;
			// pecia passes Fl::focus() == this as `selected`. The deck runs an
			// FLTK scheme, where stock Fl_Choice draws a plain label plus a
			// focus frame instead; passing selected=1 here would fill the whole
			// field with the bright selection colour on a freshly opened
			// dialog. Keep pecia's flat frame/arrow, keep the deck's calm focus.
			item.draw(xx, yy, ww, hh, this, false);
			fl_draw_shortcut = 0;
			if (Fl::focus() == this)
				draw_focus(box(), xx, yy, ww, hh);
			fl_pop_clip();
		}
		draw_label();
	}
};

} // namespace ui
} // namespace ahkdeck

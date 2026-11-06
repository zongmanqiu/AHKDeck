#include "ui/Theme.h"

#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <cstring>

// 0 means "fall back to selection_color()" in Fl_Menu_Item::draw().
Fl_Color g_popupCheckColor = 0;

// One flat 1px divider colour, shared by every popup layer.
Fl_Color g_borderColour = fl_rgb_color(140, 140, 140);

namespace ahkdeck
{
namespace ui
{
namespace
{

// Pecia light.ini values (8 semantic keys).
const ThemeColors kLight = {
	fl_rgb_color(0, 0, 0),       // text1
	fl_rgb_color(140, 140, 140), // text2
	fl_rgb_color(255, 255, 255), // background1 (work area)
	fl_rgb_color(225, 225, 225), // background2 (frame / scrollbar thumb)
	fl_rgb_color(240, 240, 240), // background3 (menu / toolbar / status)
	fl_rgb_color(230, 255, 230), // highlight1 (hover / selection)
	fl_rgb_color(150, 255, 200), // highlight2 (pressed / text selection)
	fl_rgb_color(255, 100, 100)  // highlight3
};

// FLTK's stock FL_BORDER_BOX hardcodes FL_BLACK; replace it with the themed
// line colour. Same trick as Pecia's theme_registerBorderBox().
void ThemedBorderBox(int x, int y, int w, int h, Fl_Color background)
{
	Fl::set_box_color(background);
	fl_rectf(x, y, w, h);
	Fl::set_box_color(g_borderColour);
	fl_rect(x, y, w, h);
}

void ThemedBorderFrame(int x, int y, int w, int h, Fl_Color)
{
	Fl::set_box_color(g_borderColour);
	fl_rect(x, y, w, h);
	Fl::set_box_color(FL_BLACK);
	fl_rect(x + 1, y + 1, w - 2, h - 2);
}

void StyleScrollbar(Fl_Scrollbar *aBar, const ThemeColors &aColors)
{
	if (!aBar)
		return;
	aBar->box(FL_FLAT_BOX);
	aBar->color(aColors.background1);
	aBar->selection_color(aColors.background2);
}

} // namespace

const ThemeColors &DeckColors()
{
	return kLight;
}

Fl_Color ThemeBorderLine()
{
	return fl_color_average(kLight.text1, kLight.background1, 0.55f);
}

Fl_Color MenuBarBackground()
{
	return fl_color_average(kLight.background2, kLight.background3, 0.5f);
}

Fl_Color FrameBorderColour()
{
	return kLight.text1;
}

void ApplyDeckTheme()
{
	// Pecia's stock-widget scheme. Our own widgets paint themselves; this is
	// for the ones we do not replace (Fl_Choice popups, file chooser, stock
	// dialogs), so they share the flat gtk+ look instead of FLTK's 3D default.
	Fl::scheme("gtk+");

	g_borderColour = ThemeBorderLine();
	g_popupCheckColor = kLight.highlight2;

	Fl::set_font(FL_HELVETICA, "Microsoft YaHei UI");
	Fl::set_font(FL_COURIER, "Consolas");
	Fl::scrollbar_size(10);

	Fl::set_color(FL_BACKGROUND_COLOR, kLight.background3);
	Fl::set_color(FL_BACKGROUND2_COLOR, kLight.background1);
	Fl::set_color(FL_FOREGROUND_COLOR, kLight.text1);
	Fl::set_color(FL_SELECTION_COLOR, kLight.highlight2);

	// Replace the stock 1px border boxes (same insets as FLTK's definition).
	Fl::set_boxtype(FL_BORDER_BOX, ThemedBorderBox, 1, 1, 2, 2);
	Fl::set_boxtype(FL_BORDER_FRAME, ThemedBorderFrame, 1, 1, 2, 2);

	// Pecia themes Fl_Tooltip globally (it is static state, not a widget
	// property). Without this every tooltip kept FLTK's pale-yellow default.
	Fl_Tooltip::color(kLight.background3);
	Fl_Tooltip::textcolor(kLight.text1);
	Fl_Tooltip::font(FL_HELVETICA);
	Fl_Tooltip::size(16);
}

void StyleButton(Fl_Button *aButton)
{
	if (!aButton)
		return;
	const ThemeColors &colors = DeckColors();
	aButton->box(FL_BORDER_BOX);
	aButton->color(colors.background3);
	aButton->selection_color(colors.highlight1);
	aButton->labelcolor(colors.text1);
	aButton->labelsize(16);
}

void StyleInput(Fl_Input *aInput)
{
	if (!aInput)
		return;
	const ThemeColors &colors = DeckColors();
	aInput->box(FL_BORDER_BOX);
	aInput->color(colors.background1);
	aInput->textcolor(colors.text1);
	aInput->cursor_color(colors.text1);
	aInput->selection_color(colors.highlight2);
	aInput->textsize(16);
}

void StyleChoice(Fl_Choice *aChoice)
{
	if (!aChoice)
		return;
	const ThemeColors &colors = DeckColors();
	aChoice->box(FL_BORDER_BOX);
	aChoice->color(colors.background1);
	aChoice->textcolor(colors.text1);
	aChoice->selection_color(colors.highlight2);
	aChoice->labelsize(16);
	aChoice->textsize(16);
}

void StyleTextDisplay(Fl_Text_Display *aDisplay)
{
	if (!aDisplay)
		return;
	const ThemeColors &colors = DeckColors();
	// The pane frame draws the only border; a second box here would sit 1px
	// inside it and read as a doubled line.
	aDisplay->box(FL_FLAT_BOX);
	aDisplay->scrollbar_size(10);
	aDisplay->color(colors.background1);
	aDisplay->textcolor(colors.text1);
	aDisplay->cursor_color(colors.text1);
	aDisplay->selection_color(colors.highlight2);
}

namespace
{
// Copied from Pecia ui/ThemeWidgets.h. FLTK's own thumb geometry is the
// authority for the hit test; drawing anything else (the old 20px floor) makes
// the visible thumb larger than the grab area.
void ThumbGeometry(const Fl_Scrollbar *aBar, int aTrackOrigin, int aTrackLen,
	int aCrossSize, int *aThumbLen, int *aThumbPos)
{
	const double minv = aBar->minimum();
	const double maxv = aBar->maximum();
	const double value = aBar->value();
	const float slider = aBar->slider_size();

	int len = (int)(slider * aTrackLen + 0.5f);
	const int minimum = aCrossSize / 2 + 1;
	if (len < minimum)
		len = minimum;
	if (len > aTrackLen)
		len = aTrackLen;

	double ratio;
	if (minv == maxv)
		ratio = 0.5;
	else
	{
		ratio = (value - minv) / (maxv - minv);
		if (ratio > 1.0)
			ratio = 1.0;
		else if (ratio < 0.0)
			ratio = 0.0;
	}

	*aThumbLen = len;
	*aThumbPos = aTrackOrigin + (int)(ratio * (aTrackLen - len) + 0.5);
}
} // namespace

void DrawPeciaScrollbar(Fl_Scrollbar *aBar, bool aHorizontal)
{
	if (!aBar || !aBar->visible_r())
		return;
	const int sx = aBar->x();
	const int sy = aBar->y();
	const int sw = aBar->w();
	const int sh = aBar->h();
	if (sw <= 0 || sh <= 0)
		return;

	const ThemeColors &colors = DeckColors();
	fl_color(colors.background1);
	fl_rectf(sx, sy, sw, sh);

	int thumbLen = 0;
	int thumbPos = 0;
	if (aHorizontal)
		ThumbGeometry(aBar, sx, sw, sh, &thumbLen, &thumbPos);
	else
		ThumbGeometry(aBar, sy, sh, sw, &thumbLen, &thumbPos);

	fl_color(colors.background2);
	if (aHorizontal)
		fl_rectf(thumbPos, sy, thumbLen, sh);
	else
		fl_rectf(sx, thumbPos, sw, thumbLen);
}

void DrawPeciaScrollbarCorner(Fl_Scrollbar *aVertical, Fl_Scrollbar *aHorizontal)
{
	if (!aVertical || !aHorizontal || !aVertical->visible_r() || !aHorizontal->visible_r())
		return;
	fl_color(DeckColors().background1);
	fl_rectf(aVertical->x(), aHorizontal->y(), aVertical->w(), aHorizontal->h());
}

} // namespace ui
} // namespace ahkdeck

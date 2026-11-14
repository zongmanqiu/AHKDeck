// ui/UiMetrics.h - the ONE layout vocabulary for every deck window (m182).
//
// Why this file exists: until m182 each dialog declared its own kMargin /
// kGap / kRowH / kControlH. They drifted (16/10/26/26, 16/8/40/30, ...) so
// two windows that "looked similar" had different row spacing, different
// top/bottom breathing room, and different hover behaviour on painted
// buttons. From now on a window does not invent a number: it aliases these.
//
// The values are not new - they are the facts already living in Theme.h
// (kTextSize / kRowStep = 28), DeckUI.cpp (kRowHeight = 26) and §10.2.3 of
// the script guide (EDGE 16, H_FIELD 26, H_ACTION 28, INLINE_GAP 8,
// H_STATUS 30). This header just gives them one address.
#pragma once

#include "ui/Theme.h"

#include <FL/fl_draw.H>

#include <string>

namespace ahkdeck
{
namespace ui
{

// EDGE: content starts 16px from the client edge on ALL FOUR sides. The same
// number for left/right and top/bottom is what makes the first row and the
// footer symmetrical (m182: they used to be 21px and 16px apart).
constexpr int kUiEdge = 16;

// A control (button, input, check box) is 26px - the main window's list row
// and console buttons, and §10's H_FIELD. A ROW is 28 = Theme.h's kRowStep
// (26 + 2px of air), so a control centred in a row has 1px above and below.
// The status strip is its own 30px fact (§10 H_STATUS).
constexpr int kUiControl = 26;
constexpr int kUiRow = 28;
constexpr int kUiStatus = 30;

// INLINE_GAP: between controls on one row AND between semantic blocks.
// One gap value everywhere is the whole point of this header.
constexpr int kUiGap = 8;

// A list card is a CONTAINER: its first row may not touch the 1px frame. m183
// found the button in row 0 only 1px below the top line, because a 26px
// control centred in a 28px row starts at +1 and the card painted rows from
// y(). Both lists now inset their content by one gap at top AND bottom, so
// the card reads as a box with air inside instead of a grid glued to it.
constexpr int kUiListPad = 8;

// m184: ONE width rule for every button in the deck. A label plus this much
// air on each side, never narrower than kUiButtonMinW. HoverButton::fitWidth
// (ThemeWidgets.h) and hand-painted list cells both read these numbers, so
// two two-character labels cannot end up 72px and 88px in the same row.
constexpr int kUiButtonPadX = 12;
constexpr int kUiButtonMinW = 48;

// Measure with the font the button actually draws with, then restore FLTK's
// global font state: Layout() calls this while other widgets still draw.
inline int UiLabelWidth(const char *aLabel, int aTextSize)
{
	const Fl_Font face = fl_font();
	const int size = fl_size();
	fl_font(FL_HELVETICA, aTextSize);
	const double measured = fl_width(aLabel);
	fl_font(face, size);
	const int width = (int)(measured + 0.5) + 2 * kUiButtonPadX;
	return width < kUiButtonMinW ? kUiButtonMinW : width;
}

// Painted cells use SubTextSize(); widget buttons use HoverButton::fit().
inline int UiCellButtonWidth(const char *aLabel)
{
	return UiLabelWidth(aLabel, SubTextSize());
}

// m185b: FL_ALIGN_CLIP cuts a long label off with no hint that it was cut.
// Every list column now goes through this, so an over-long value ends in a
// visible ellipsis instead of a word chopped in half. Measured with the same
// font/size the caller draws with, and the global FLTK font is restored.
inline std::string UiFitText(const std::string &aText, int aMaxWidth,
	int aTextSize)
{
	if (aText.empty() || aMaxWidth <= 0)
		return std::string();
	const Fl_Font face = fl_font();
	const int size = fl_size();
	fl_font(FL_HELVETICA, aTextSize);
	const double ellipsisW = fl_width("…");
	std::string out;
	double width = 0.0;
	size_t i = 0;
	while (i < aText.size())
	{
		const unsigned char lead = (unsigned char)aText[i];
		size_t len = 1;
		if ((lead & 0xE0) == 0xC0) len = 2;
		else if ((lead & 0xF0) == 0xE0) len = 3;
		else if ((lead & 0xF8) == 0xF0) len = 4;
		if (i + len > aText.size()) len = 1;
		const std::string ch = aText.substr(i, len);
		const double chW = fl_width(ch.c_str());
		if (width + chW + ellipsisW > (double)aMaxWidth)
		{
			out += "…";
			fl_font(face, size);
			return out;
		}
		out += ch;
		width += chW;
		i += len;
	}
	fl_font(face, size);
	return out;
}


// The label column of the search / address rows (already the same in both
// dialogs; named here so a third dialog cannot pick 40 or 56 by accident).
constexpr int kUiLabelW = 48;

// Centre a kUiControl-tall control inside a kUiRow-tall row. Both the first
// row and the footer MUST call this: that is what makes the top and bottom
// margins equal instead of "one row is centred, the other is not".
constexpr int UiControlY(int aRowY)
{
	return aRowY + (kUiRow - kUiControl) / 2;
}

// The single hover/press rule for EVERY button in the deck, whether it is an
// FLTK widget (HoverButton) or a cell painted by hand inside a list. Before
// m182 only the widget family hovered - the 感叹号 cell in the script window
// stayed flat, which is why it felt like a different app.
inline Fl_Color UiButtonFill(Fl_Color aBase, bool aHovered, bool aPressed)
{
	if (aPressed)
		return DeckColors().highlight2;
	if (aHovered)
		return DeckColors().highlight1;
	return aBase;
}

// A cell button sits INSIDE a row that already has a hover tint, so it needs
// one stronger step than a standalone HoverButton to remain visible:
//   neutral cell (感叹号 / 移除) -> highlight2 on hover;
//   tinted cell  (blue 下载 / amber 升级) -> lighten its own hue, so the
//   action keeps its meaning instead of turning green.
inline Fl_Color UiCellButtonFill(Fl_Color aBase, bool aHovered, bool aTinted)
{
	if (!aHovered)
		return aBase;
	if (aTinted)
		return fl_color_average(aBase, DeckColors().background1, 0.45f);
	return DeckColors().highlight2;
}

// The same button, drawn by hand inside a self-painted list cell. Before m182
// each list invented this locally and none of them passed a hover state, so
// the 感叹号 / 移除 cells stayed flat while HoverButton changed colour.
inline void UiDrawCellButton(int aX, int aY, int aW, int aH,
	const char *aText, Fl_Color aBase, Fl_Color aTextColour,
	bool aHovered, bool aTinted = false)
{
	fl_color(UiCellButtonFill(aBase, aHovered, aTinted));
	fl_rectf(aX, aY, aW, aH);
	fl_color(ThemeBorderLine());
	fl_rect(aX, aY, aW, aH);
	fl_font(FL_HELVETICA, SubTextSize());
	fl_color(aTextColour);
	fl_draw(aText, aX, aY, aW, aH, FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
}

} // namespace ui
} // namespace ahkdeck

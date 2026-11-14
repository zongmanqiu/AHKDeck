// ui/ScriptInfoDialog.cpp - see ui/ScriptInfoDialog.h.
//
// The layout is computed once (Layout) and then painted from the same numbers,
// so the window height and the widget positions cannot disagree. Fields are
// measured with fl_width, which resolves CJK glyphs correctly, instead of
// guessing a characters-per-line count.
#include "ui/ScriptInfoDialog.h"

#include "ui/DeckDialog.h"
#include "ui/DeckTitleBar.h"
#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"
#include "ui/UiMetrics.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <string>
#include <vector>

namespace ahkdeck
{
namespace ui
{
namespace
{

// m182: shared facts from UiMetrics; only this dialog's label column and
// prose padding are local.
constexpr int kWidth = 480;
constexpr int kMargin = kUiEdge;
constexpr int kLabelW = 72;  // longer field names than the 48px search label
constexpr int kGap = kUiGap;
constexpr int kLineH = kUiControl;
constexpr int kBtnH = kUiControl;
constexpr int kGroupGap = kUiGap;
// Wrapped prose stops this far short of the value column's right edge, so the
// longest line never ends on the frame and short lines keep a visible gap.
constexpr int kTextPad = 8;

int ValueWidth()
{
	return kWidth - 2 * kMargin - kLabelW - kGap;
}

std::string Trimmed(const std::string &aText)
{
	size_t first = 0;
	while (first < aText.size() && (aText[first] == ' ' || aText[first] == '\t'
		|| aText[first] == '\r' || aText[first] == '\n'))
		++first;
	size_t last = aText.size();
	while (last > first && (aText[last - 1] == ' ' || aText[last - 1] == '\t'
		|| aText[last - 1] == '\r' || aText[last - 1] == '\n'))
		--last;
	return aText.substr(first, last - first);
}

// Empty values and then-empty groups are removed before anything is measured.
std::vector<InfoGroup> Pruned(const std::vector<InfoGroup> &aGroups)
{
	std::vector<InfoGroup> out;
	out.reserve(aGroups.size());
	for (const InfoGroup &group : aGroups)
	{
		InfoGroup kept;
		kept.reserve(group.size());
		for (const InfoField &field : group)
		{
			std::string value = Trimmed(field.value);
			if (value.empty())
				continue;
			InfoField copy = field;
			copy.value = value;
			kept.push_back(copy);
		}
		if (!kept.empty())
			out.push_back(kept);
	}
	return out;
}

// FLTK's own label wrapper breaks only at spaces: fl_draw.cxx checks the width
// when it meets ' ' or '\n', so a run of Chinese with no spaces is drawn past
// the box and clipped by the window frame. That is why the longest line of a
// description sat flush against the right edge however narrow the box was.
// The text is broken here instead, one character at a time, and FLTK is handed
// a value that already carries the newlines.
constexpr int kMaxValueLines = 6;

bool IsClosingMark(const std::string &aChar)
{
	static const char *const kMarks[] = {
		"，", "。", "、", "；", "：", "！", "？", "）", "〕", "〉",
		"》", "」", "』", "】", "”", "’", "…",
	};
	for (const char *mark : kMarks)
	{
		if (aChar == mark)
			return true;
	}
	return false;
}

// One element per character, so a break can fall between any two of them.
std::vector<std::string> BytesToChars(const std::string &aText)
{
	std::vector<std::string> chars;
	chars.reserve(aText.size());
	size_t i = 0;
	while (i < aText.size())
	{
		const unsigned char lead = (unsigned char)aText[i];
		size_t len = 1;
		if ((lead & 0xE0) == 0xC0)
			len = 2;
		else if ((lead & 0xF0) == 0xE0)
			len = 3;
		else if ((lead & 0xF8) == 0xF0)
			len = 4;
		if (i + len > aText.size())
			len = 1;
		chars.push_back(aText.substr(i, len));
		i += len;
	}
	return chars;
}

std::string WrappedByWidth(const std::string &aText, int aWrapWidth)
{
	const int avail = aWrapWidth;
	if (avail <= 0 || aText.empty() || aText.find('\n') != std::string::npos)
		return aText;

	fl_font(FL_HELVETICA, SubTextSize());
	const std::vector<std::string> chars = BytesToChars(aText);

	std::string out;
	size_t lineBegin = 0;
	double lineWidth = 0.0;
	int lines = 1;
	for (size_t i = 0; i < chars.size(); ++i)
	{
		const double charWidth = fl_width(chars[i].c_str());
		if (lineWidth + charWidth > avail && i > lineBegin)
		{
			size_t breakAt = i;
			// A closing mark may not open a line: carry the glyph before it
			// down as well, so the mark closes the shorter line instead.
			if (i - lineBegin > 1 && IsClosingMark(chars[i]))
				breakAt = i - 1;
			for (size_t k = lineBegin; k < breakAt; ++k)
				out += chars[k];
			if (++lines > kMaxValueLines)
				return out + "…"; // a runaway value must not push the button off-screen
			out += '\n';
			lineBegin = breakAt;
			lineWidth = 0.0;
			i = breakAt - 1; // the for loop re-reads this character with a fresh line
			continue;
		}
		lineWidth += charWidth;
	}
	for (size_t k = lineBegin; k < chars.size(); ++k)
		out += chars[k];
	return out;
}

// Every wrapped value is broken once, before any measuring, so the two Layout
// passes and the widgets all see the same number of lines.
void WrapAll(std::vector<InfoGroup> &aGroups, int aWrapWidth)
{
	// m185: EVERY value is broken to the column width, not just the ones a
	// caller remembered to flag as prose. An unflagged URL was drawn as one
	// unbroken line and cut off by the window frame - "all text must fit" is
	// a property of the dialog, not of each call site.
	for (InfoGroup &group : aGroups)
	{
		for (InfoField &field : group)
		{
			field.value = WrappedByWidth(field.value, aWrapWidth);
		}
	}
}

int LinesFor(const std::string &aText)
{
	int lines = 1;
	for (char c : aText)
	{
		if (c == '\n')
			++lines;
	}
	return lines;
}

int FieldHeight(const InfoField &aField)
{
	// The value already carries its own newlines, so every field - including
	// the selectable path - is as tall as the lines it was broken into.
	return kLineH * LinesFor(aField.value);
}

struct Placed
{
	const InfoField *field;
	int y;
	int h;
};

struct LayoutResult
{
	std::vector<Placed> fields;
	std::vector<int> rules; // y of each hairline
	int bottom = 0;
};

LayoutResult Layout(const std::vector<InfoGroup> &aGroups, int aTop)
{
	LayoutResult out;
	int y = aTop;
	for (size_t gi = 0; gi < aGroups.size(); ++gi)
	{
		if (gi > 0)
		{
			y += kGroupGap;
			out.rules.push_back(y);
			y += 1 + kGroupGap;
		}
		const InfoGroup &group = aGroups[gi];
		for (size_t fi = 0; fi < group.size(); ++fi)
		{
			const int h = FieldHeight(group[fi]);
			out.fields.push_back({ &group[fi], y, h });
			y += h;
			if (fi + 1 < group.size())
				y += 2;
		}
	}
	out.bottom = y;
	return out;
}

// Content taller than half the work area scrolls instead of stretching the
// window: a dialog that covers the desktop pushes its button row and its last
// rows out of sight, and its own frame stops reading as a dialog at all.
int MaxDialogHeight()
{
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	Fl::screen_work_area(x, y, w, h); // the work area excludes the taskbar
	int cap = h / 2;
	if (cap < 320)
		cap = 320; // a short screen must still hold the caption, a row and the button
	return cap;
}

class Rule : public Fl_Widget
{
public:
	Rule(int X, int Y, int W) : Fl_Widget(X, Y, W, 1) { box(FL_NO_BOX); }

	void draw() override
	{
		fl_color(ThemeBorderLine());
		fl_xyline(x(), y(), x() + w() - 1);
	}
};

class InfoDialog : public DeckDialog
{
public:
	InfoDialog(const char *aTitle, const std::vector<InfoGroup> &aGroups,
		int aHeight, bool aScroll, int aViewportHeight, bool aRounded,
		bool aShadow)
		: DeckDialog(kWidth, aHeight, aTitle), mGroups(aGroups),
		  mScroll(aScroll), mViewportHeight(aViewportHeight)
	{
		SetFrameOptions(aRounded, aShadow);
	}

	void Build()
	{
		const ThemeColors &colors = DeckColors();
		begin();

		const LayoutResult layout = Layout(mGroups, ContentTop() + kMargin);

		if (mScroll)
		{
			// Full window width, so the bar's track ends on the frame line the
			// way pecia's SettingsDialog does (its scroll spans x=0..w()). The
			// value column still stops a margin short of it, so the text never
			// runs under the bar and does not have to be re-broken.
			DeckScroll *viewport = new DeckScroll(0, ContentTop(), kWidth,
				mViewportHeight);
			viewport->box(FL_NO_BOX);
			viewport->color(colors.background1);
			viewport->type(Fl_Scroll::VERTICAL);
			viewport->begin();
			AddFields(layout, colors);
			viewport->end();
		}
		else
		{
			AddFields(layout, colors);
		}

		mBtnClose = new HoverButton(0, 0, 0, kBtnH, "关闭");
		mBtnClose->color(colors.background2);
		mBtnClose->labelcolor(colors.text1);
		mBtnClose->labelsize(TextSize());
		mBtnClose->callback(OnClose, this);
		FitButtonRow({ mBtnClose }, kWidth, h() - kMargin - kBtnH / 2,
			kMargin, kGap);

		end();
	}

	void Run()
	{
		ShowCentered();
		while (shown())
			Fl::wait();
	}

private:
	void AddFields(const LayoutResult &aLayout, const ThemeColors &aColors)
	{
		const int valueWidth = ValueWidth();
		for (const Placed &placed : aLayout.fields)
		{
			Fl_Box *label = new Fl_Box(FL_NO_BOX, kMargin, placed.y, kLabelW,
				placed.h, nullptr);
			label->copy_label(placed.field->label.c_str());
			label->align(FL_ALIGN_RIGHT | FL_ALIGN_TOP | FL_ALIGN_INSIDE
				| FL_ALIGN_CLIP);
			label->labelsize(SubTextSize());
			label->labelcolor(aColors.text2);

			const int valueX = kMargin + kLabelW + kGap;
			if (placed.field->selectable)
			{
				// Borderless so it reads as text, but selectable: a path is the
				// one value here a user may want on the clipboard.
				Fl_Multiline_Output *out = new Fl_Multiline_Output(valueX,
					placed.y, valueWidth, placed.h);
				out->box(FL_NO_BOX);
				out->color(aColors.background1);
				out->textcolor(aColors.text2);
				out->textsize(SubTextSize());
				out->textfont(FL_HELVETICA);
				// Fl_Multiline_Output paints a "^" caret at the insert point
				// while it holds the focus (Fl_Input_.cxx, "Draw '^' caret
				// cursor") - noise in a window that is read-only. The header
				// documents clear_visible_focus() as the way to turn it off.
				out->clear_visible_focus();
				out->value(placed.field->value.c_str());
			}
			else
			{
				// Full column width, not the wrap width: the value already
				// carries its own newlines, so the box only has to be wide
				// enough to not wrap them a second time.
				Fl_Box *value = new Fl_Box(FL_NO_BOX, valueX, placed.y,
					valueWidth, placed.h, nullptr);
				value->copy_label(placed.field->value.c_str());
				value->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE
					| FL_ALIGN_WRAP | FL_ALIGN_CLIP);
				value->labelsize(SubTextSize());
				value->labelcolor(aColors.text1);
			}
		}

		// Wide enough for the value column only: the rule must not run under
		// the scrollbar when the content scrolls.
		const int ruleWidth = kLabelW + kGap + valueWidth;
		for (int y : aLayout.rules)
			new Rule(kMargin, y, ruleWidth);
	}

	static void OnClose(Fl_Widget *, void *aData)
	{
		static_cast<InfoDialog *>(aData)->hide();
	}

	std::vector<InfoGroup> mGroups;
	bool mScroll = false;
	int mViewportHeight = 0;
	HoverButton *mBtnClose = nullptr;
};

} // namespace

void ShowInfoDialog(const char *aTitle, const std::vector<InfoGroup> &aGroups,
	bool aRounded, bool aShadow)
{
	std::vector<InfoGroup> groups = Pruned(aGroups);
	if (groups.empty())
		return; // nothing to say; never open an empty window

	const int contentTop = DeckTitleBar::kHeight + kMargin;
	const int valueWidth = ValueWidth();
	WrapAll(groups, valueWidth - kTextPad);

	// Height the content would need with no scrolling at all; the window only
	// takes it when it fits under the cap.
	const int wanted = Layout(groups, contentTop).bottom + kGap + kBtnH + kMargin;
	const int cap = MaxDialogHeight();
	const bool scroll = wanted > cap;

	int height = scroll ? cap : wanted;
	int viewportHeight = height - contentTop - kGap - kBtnH - kMargin;
	if (scroll)
	{
		// The bar sits outside the value column (the viewport spans the whole
		// window), so the text keeps the breaks it was measured with - there is
		// no second wrapping pass to run here.

		// Stop the first screen on a field boundary: a row sliced in half
		// reads as a clipping bug rather than as "there is more below".
		const LayoutResult layout = Layout(groups, contentTop + kMargin);
		const int limit = contentTop + viewportHeight;
		int bottom = contentTop;
		for (const Placed &placed : layout.fields)
		{
			if (placed.y + placed.h > limit)
				break;
			bottom = placed.y + placed.h;
		}
		if (bottom > contentTop)
		{
			// Not "bottom + gap": the next field starts 2px under the last
			// one, so any padding here would let its top edge peek through.
			viewportHeight = bottom - contentTop;
			height = contentTop + viewportHeight + kGap + kBtnH + kMargin;
		}
	}

	InfoDialog dialog(aTitle ? aTitle : "信息", groups, height, scroll,
		viewportHeight, aRounded, aShadow);
	dialog.Build();
	dialog.Run();
}

} // namespace ui
} // namespace ahkdeck

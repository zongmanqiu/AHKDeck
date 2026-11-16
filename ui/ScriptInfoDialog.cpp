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

#include <windows.h>
#include <shellapi.h>

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
			InfoField copy = field;
			copy.value = Trimmed(field.value);
			// m221: a link row keeps its content in `links`, not `value`.
			// Trim both halves and drop unusable links, so a field with no
			// value and no live link disappears just like an empty old field.
			copy.links.clear();
			for (const InfoLink &link : field.links)
			{
				InfoLink l = link;
				l.text = Trimmed(l.text);
				l.url = Trimmed(l.url);
				if (!l.text.empty() && !l.url.empty())
					copy.links.push_back(l);
			}
			if (copy.value.empty() && copy.links.empty())
				continue;
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

// m221: a clickable URL value. The value carries the newlines WrapAll inserted
// for display, so the browser gets them stripped before ShellExecute. Hover
// and underline follow AboutDialog's repository links - one link look for the
// whole app.
class LinkValueWidget : public Fl_Widget
{
public:
	LinkValueWidget(int X, int Y, int W, int H, const std::string &aValue,
		int aLineH, int aFont, int aSize, Fl_Color aBase, Fl_Color aHot,
		Fl_Color aBg)
		: Fl_Widget(X, Y, W, H), m_value(aValue), m_lineH(aLineH),
		  m_font(aFont), m_size(aSize), m_base(aBase), m_hot(aHot), m_bg(aBg)
	{
		box(FL_NO_BOX);
	}

	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		fl_rectf(x(), y(), w(), h(), m_bg);
		fl_font(m_font, m_size);
		const Fl_Color col = m_hover ? m_hot : m_base;
		fl_color(col);
		size_t begin = 0;
		int line = 0;
		while (true)
		{
			const size_t nl = m_value.find('\n', begin);
			const std::string part = m_value.substr(
				begin, nl == std::string::npos ? std::string::npos : nl - begin);
			const int ty = y() + line * m_lineH + (m_lineH - m_size) / 2 + m_size;
			fl_draw(part.c_str(), x(), ty);
			int tw = 0;
			int th = 0;
			fl_measure(part.c_str(), tw, th);
			fl_line_style(FL_SOLID, 1);
			fl_color(col);
			fl_line(x(), ty + 2, x() + tw, ty + 2);
			fl_line_style(0);
			if (nl == std::string::npos)
				break;
			begin = nl + 1;
			++line;
		}
		fl_pop_clip();
	}

	int handle(int event) override
	{
		switch (event)
		{
		case FL_ENTER:
		case FL_MOVE:
			if (!m_hover)
			{
				m_hover = true;
				redraw();
			}
			return 1;
		case FL_LEAVE:
			if (m_hover)
			{
				m_hover = false;
				redraw();
			}
			return 1;
		case FL_PUSH:
			return 1;
		case FL_RELEASE:
		{
			if (!Fl::event_inside(this))
				return 0;
			std::string url;
			for (char c : m_value)
			{
				if (c != '\n' && c != '\r')
					url += c;
			}
			if (!url.empty())
				ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
					SW_SHOWNORMAL);
			return 1;
		}
		}
		return 0;
	}

private:
	std::string m_value;
	int m_lineH;
	int m_font;
	int m_size;
	Fl_Color m_base;
	Fl_Color m_hot;
	Fl_Color m_bg;
	bool m_hover = false;
};

// m221: one row holding several short clickable labels ("Gitee | GitHub").
// Mirrors AboutDialog's RepoLinksWidget: each label is its own hit region, so
// both mirrors stay reachable from one field regardless of the chosen source.
class LinkRowWidget : public Fl_Widget
{
public:
	LinkRowWidget(int X, int Y, int W, int H,
		const std::vector<InfoLink> &aLinks, int aFont, int aSize,
		Fl_Color aBase, Fl_Color aHot, Fl_Color aBg)
		: Fl_Widget(X, Y, W, H), m_links(aLinks), m_font(aFont),
		  m_size(aSize), m_base(aBase), m_hot(aHot), m_bg(aBg)
	{
		box(FL_NO_BOX);
	}

	int handle(int event) override
	{
		switch (event)
		{
		case FL_ENTER:
		case FL_MOVE:
		{
			const int hit = hitAt(Fl::event_x());
			if (hit != m_hover)
			{
				m_hover = hit;
				redraw();
			}
			return 1;
		}
		case FL_LEAVE:
			if (m_hover != -1)
			{
				m_hover = -1;
				redraw();
			}
			return 1;
		case FL_PUSH:
			return 1;
		case FL_RELEASE:
		{
			if (!Fl::event_inside(this))
				return 0;
			const int hit = hitAt(Fl::event_x());
			if (hit >= 0 && hit < (int)m_links.size())
				ShellExecuteA(nullptr, "open", m_links[hit].url.c_str(),
					nullptr, nullptr, SW_SHOWNORMAL);
			return 1;
		}
		}
		return 0;
	}

	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		fl_rectf(x(), y(), w(), h(), m_bg);
		int starts[8] = { 0 }, widths[8] = { 0 };
		int ty = 0;
		layout(starts, widths, &ty);
		fl_color(m_base);
		for (size_t i = 0; i < m_links.size(); ++i)
		{
			if (i > 0)
			{
				fl_color(m_base);
				fl_draw(m_sep, starts[i] - m_sepW, ty);
			}
			const Fl_Color col = (i == (size_t)m_hover) ? m_hot : m_base;
			fl_color(col);
			fl_draw(m_links[i].text.c_str(), starts[i], ty);
			fl_line_style(FL_SOLID, 1);
			fl_color(col);
			// Tight underlines: the label uses the same baseline but has no
			// underline, so a 2px gap here made the whole row read as low.
			fl_line(starts[i], ty + 1, starts[i] + widths[i], ty + 1);
			fl_line_style(0);
		}
		fl_pop_clip();
	}

private:
	// draw() and hitAt() share these numbers, so the painted links and their
	// clickable regions cannot drift apart.
	void layout(int *aStarts, int *aWidths, int *aTy)
	{
		fl_font(m_font, m_size);
		m_sepW = (int)fl_width(m_sep);
		int cx = x();
		for (size_t i = 0; i < m_links.size() && i < 8; ++i)
		{
			if (i > 0)
				cx += m_sepW;
			aStarts[i] = cx;
			aWidths[i] = (int)fl_width(m_links[i].text.c_str());
			cx += aWidths[i];
		}
		// Exact baseline of a sibling Fl_Box drawn with FL_ALIGN_TOP.
		// fl_draw.cxx: ypos = y + height (FL_ALIGN_TOP), then the glyphs are
		// drawn at ypos - desc (line 369). So the real baseline is
		// y + fl_height() - fl_descent(), NOT y + fl_height() - that extra
		// descent (~3px) is why the links still sat visibly low.
		*aTy = y() + fl_height() - fl_descent();
	}

	int hitAt(int aMouseX)
	{
		int starts[8] = { 0 }, widths[8] = { 0 }, ty = 0;
		layout(starts, widths, &ty);
		for (size_t i = 0; i < m_links.size() && i < 8; ++i)
		{
			if (aMouseX >= starts[i] && aMouseX < starts[i] + widths[i])
				return (int)i;
		}
		return -1;
	}

	std::vector<InfoLink> m_links;
	int m_font;
	int m_size;
	Fl_Color m_base;
	Fl_Color m_hot;
	Fl_Color m_bg;
	const char *m_sep = " | ";
	int m_sepW = 0;
	int m_hover = -1;
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
			if (!placed.field->links.empty())
			{
				// m221: a row of short clickable mirrors ("Gitee | GitHub").
				new LinkRowWidget(valueX, placed.y, valueWidth, placed.h,
					placed.field->links, FL_HELVETICA, SubTextSize(),
					aColors.text1, aColors.highlight3, aColors.background1);
			}
			else if (placed.field->link)
			{
				// m221: an HTTPS URL the user may click to open. Styled like
				// AboutDialog's repository links (same base/hover tokens).
				new LinkValueWidget(valueX, placed.y, valueWidth, placed.h,
					placed.field->value, kLineH, FL_HELVETICA, SubTextSize(),
					aColors.text1, aColors.highlight3, aColors.background1);
			}
			else if (placed.field->selectable)
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

// ui/ScriptDownloadDialog.cpp - see ui/ScriptDownloadDialog.h (m146, m179).
//
// m179 reshaped this window in three ways that belong together:
//   * the official mirror is no longer a picker here - it is 选项 → 官方下载源,
//     so this window only reads the resolved manifest URL;
//   * subscribed manifests merge into the same list, with a 来源 column saying
//     where each row came from;
//   * the window resizes and maximizes, and every position comes from Layout()
//     instead of the kWidth constants, because a third-party catalog can hold
//     names that need more room than 560px.
//
// m180 moved subscription OUT of this window: the one URL box with a
// 订阅 / 移除 button that flipped meaning was a dead end (no list, so nothing
// to manage), and it is now a 订阅 button that opens ui/SubscribeDialog.
#include "ui/ScriptDownloadDialog.h"

#include "core/AppPaths.h"

#include "ui/BlinkInput.h"
#include "ui/DeckFrame.h"
#include "ui/DeckTitleBar.h"
#include "ui/ScriptInfoDialog.h"
#include "ui/SubscribeDialog.h"
#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"
#include "ui/UiMetrics.h"

#include "core/ScriptCatalog.h"
#include "core/ScriptHeader.h"
#include "core/Updater.h"

#include <windows.h>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ahkdeck
{
namespace ui
{
namespace
{

// m182: alias the shared layout vocabulary. The old 40px rows / 30px controls
// were this window's own invention and made its list look wider than the main
// window's 26px script rows (and its top/bottom margins asymmetric).
constexpr int kMargin = kUiEdge;
constexpr int kGap = kUiGap;
constexpr int kRowH = kUiRow;
constexpr int kControlH = kUiControl;
constexpr int kLabelW = kUiLabelW;
constexpr int kStatusH = kUiStatus;
constexpr int kInfoW = 28;  // 感叹号 is a square ICON cell, not a text button
constexpr int kVersionW = 56;
constexpr int kSourceW = 96;
constexpr int kListPad = kUiListPad;

// m184: no hard-coded button widths. Widget buttons use FitButtonRow (which
// calls HoverButton::fit()), painted cells use UiCellButtonWidth(). A table
// column keeps ONE width for every row, sized by its longest caption, so the
// cells stay aligned while the width still follows the text.
int ActionColumnWidth() { return UiCellButtonWidth("已下载"); }

// m179: taller and wider than the old 560 x 376 - the 订阅 row and the 来源
// column both had to fit, and the list is the point of the window.
constexpr int kDefaultW = 760;
constexpr int kDefaultH = 560;

std::wstring Utf8ToWideLocal(const std::string &aText)
{
	if (aText.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, aText.c_str(),
		(int)aText.size(), nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aText.c_str(), (int)aText.size(),
		out.data(), n);
	return out;
}

std::string LowerLocal(const std::string &aText)
{
	std::string out = aText;
	for (char &c : out)
		c = (char)std::tolower((unsigned char)c);
	return out;
}

std::string TrimLocal(const std::string &aText)
{
	size_t first = 0;
	while (first < aText.size() && (aText[first] == ' ' || aText[first] == '\t'))
		++first;
	size_t last = aText.size();
	while (last > first && (aText[last - 1] == ' ' || aText[last - 1] == '\t'))
		--last;
	return aText.substr(first, last - first);
}

// The one check a subscribed URL has to pass before a worker touches it.
bool LooksLikeCatalogUrl(const std::string &aUrl)
{
	if (aUrl.size() < 16 || aUrl.compare(0, 8, "https://") != 0)
		return false;
	for (char c : aUrl)
	{
		if ((unsigned char)c <= 0x20)
			return false;
	}
	return true;
}

bool MatchesFilter(const CatalogEntry &aEntry, const std::string &aQuery)
{
	return LowerLocal(aEntry.name).find(aQuery) != std::string::npos
		|| LowerLocal(aEntry.version).find(aQuery) != std::string::npos
		|| LowerLocal(aEntry.description).find(aQuery) != std::string::npos
		|| LowerLocal(aEntry.author).find(aQuery) != std::string::npos;
}

// m182: the one painted button. UiDrawCellButton owns the fill, the 1px ring
// and the hover/press tints so every list cell matches HoverButton exactly.
void DrawFlatButton(int aX, int aY, int aW, int aH, const std::string &aText,
	Fl_Color aFill, Fl_Color aTextColour, bool aHovered, bool aTinted)
{
	UiDrawCellButton(aX, aY, aW, aH, aText.c_str(), aFill, aTextColour,
		aHovered, aTinted);
}

// A single self-painted widget owns the whole list. FLTK widget groups would
// need N sets of children rebuilt on every filter keystroke; this keeps one
// truth for the row geometry and one place for the hit test.
class ScriptRows : public Fl_Widget
{
public:
	ScriptRows(int aX, int aY, int aW, int aH)
		: Fl_Widget(aX, aY, aW, aH)
	{
	}

	void SetRows(const std::vector<RowView> &aRows)
	{
		mRows = aRows;
		const int maxTop = (int)mRows.size() - VisibleRows();
		if (mTop > (maxTop > 0 ? maxTop : 0))
			mTop = maxTop > 0 ? maxTop : 0;
		mHover = -1;
		mHoverButton = 0;
		// A filter/refresh rebuilds the row order, so a numeric index no
		// longer names the same script. Keep the selection only if in range.
		if (mSelected >= (int)mRows.size())
			mSelected = -1;
		redraw();
	}

	void SetEmptyText(const std::string &aText)
	{
		mEmptyText = aText;
		redraw();
	}

	std::function<void(int)> onAction;
	std::function<void(int)> onInfo;

	int VisibleRows() const { return (h() - 2 * kListPad) / kRowH; }

	// m183: the card insets its rows by kListPad top and bottom. One place
	// owns the arithmetic so paint, hover and click can never disagree.
	int RowAt(int aLocalY) const
	{
		if (aLocalY < kListPad
			|| aLocalY >= kListPad + VisibleRows() * kRowH)
			return -1;
		return mTop + (aLocalY - kListPad) / kRowH;
	}
	int InRowY(int aLocalY, int aIdx) const
	{
		return aLocalY - kListPad - (aIdx - mTop) * kRowH;
	}

	// The merged order is the dialog's; the row widget only paints it. Callers
	// that need the row behind a click read it back through this.
	const std::vector<RowView> &Rows() const { return mRows; }

	void draw() override
	{
		const ThemeColors &colors = DeckColors();
		fl_color(colors.background1);
		fl_rectf(x(), y(), w(), h());
		fl_color(ThemeBorderLine());
		fl_rect(x(), y(), w(), h());

		if (mRows.empty())
		{
			fl_font(FL_HELVETICA, SubTextSize());
			fl_color(colors.text2);
			fl_draw(mEmptyText.c_str(), x() + kMargin, y() + kListPad,
				w() - 2 * kMargin, kRowH,
				FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
			return;
		}

		const int visible = VisibleRows();
		const int actionX = w() - kMargin - ActionColumnWidth();
		const int infoX = actionX - kGap - kInfoW;
		const int versionX = infoX - kGap - kVersionW;
		const int sourceX = versionX - kGap - kSourceW;
		const int nameW = sourceX - kMargin - kGap;
		const int btnY = (kRowH - kControlH) / 2;

		for (int v = 0; v < visible && (size_t)(mTop + v) < mRows.size(); ++v)
		{
			const RowView &row = mRows[(size_t)(mTop + v)];
			const int ry = y() + kListPad + v * kRowH;
			const int idx = mTop + v;
			// m182: two row states, both from the eight-key palette. The
			// selected row keeps a quiet grey band while the pointer is away;
			// hover takes the light green. No zebra striping: an alternating
			// band is a third colour rule and fights both states.
			if (idx == mSelected)
			{
				fl_color(colors.highlight1);
				fl_rectf(x() + 1, ry + 1, w() - 2, kRowH - 2);
			}
			else if (mHover == idx)
			{
				fl_color(colors.background3);
				fl_rectf(x() + 1, ry + 1, w() - 2, kRowH - 2);
			}
			// m176: no per-row rule. The row already has four anchors - a 40px
			// band, the name / 来源 / version columns, and the two buttons on
			// the right - and the card draws its own 1px frame. A line under
			// every row stacked a tenth line on top of those and read as
			// clutter; the hover tint is what actually locates a row.

			fl_font(FL_HELVETICA, TextSize());
			fl_color(colors.text1);
			fl_draw(UiFitText(row.entry.name, nameW, TextSize()).c_str(),
				x() + kMargin, ry, nameW, kRowH,
				FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

			// 来源: 官方 for our own catalog, the host for a subscribed one.
			// Clipped, never overlapping the version column - the window can
			// be resized, but a 96px column is still a 96px column.
			// m182: these x values are widget-relative (w() is the card's
			// width), but fl_draw/rectf take window coordinates. The old code
			// drew the columns and buttons 16px to the LEFT of where handle()
			// hit-tested them, so hovering the visible 感叹号 did nothing.
			// Add x() once here; ButtonAt() keeps using localX as before.
			fl_font(FL_HELVETICA, SubTextSize());
			fl_color(colors.text2);
			fl_draw(UiFitText(row.sourceLabel, kSourceW, SubTextSize()).c_str(),
				x() + sourceX, ry, kSourceW, kRowH,
				FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
			fl_draw(UiFitText(row.entry.version, kVersionW, SubTextSize()).c_str(),
				x() + versionX, ry, kVersionW,
				kRowH, FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

			// m182: the 感叹号 cell now hovers like every other button. The
			// cell tracks WHICH button the pointer is over, not just the row.
			DrawFlatButton(x() + infoX, ry + btnY, kInfoW, kControlH, "!",
				colors.background2, colors.text1,
				mHover == idx && mHoverButton == 1, false);

			Fl_Color fill = IdleGrey();
			Fl_Color text = colors.text2;
			if (row.tone == ActionTone::Primary)
			{
				fill = DownloadBlue();
				text = colors.background1;
			}
			else if (row.tone == ActionTone::Warning)
			{
				fill = UpgradeAmber();
				text = colors.background1;
			}
			const bool tinted = row.tone == ActionTone::Primary
				|| row.tone == ActionTone::Warning;
			DrawFlatButton(x() + actionX, ry + btnY, ActionColumnWidth(), kControlH,
				row.caption, fill, text,
				mHover == idx && mHoverButton == 2, tinted);
		}

		if ((int)mRows.size() > visible)
		{
			const int trackX = x() + w() - 5;
			fl_color(colors.background2);
			const int bandY = y() + kListPad;
			const int bandH = h() - 2 * kListPad;
			fl_rectf(trackX, bandY, 3, bandH);
			const int span = (int)mRows.size() - visible;
			// No std::max here: windows.h defines max as a macro.
			const int computed = bandH * visible / (int)mRows.size();
			const int thumbH = computed > 20 ? computed : 20;
			const int thumbY = bandY + (bandH - thumbH) * mTop / (span > 0 ? span : 1);
			fl_color(colors.text2);
			fl_rectf(trackX, thumbY, 3, thumbH);
		}
	}

	int handle(int aEvent) override
	{
		if (aEvent == FL_MOUSEWHEEL)
		{
			const int maxTop = (int)mRows.size() - VisibleRows();
			mTop += Fl::event_dy();
			if (mTop < 0)
				mTop = 0;
			if (mTop > maxTop)
				mTop = maxTop > 0 ? maxTop : 0;
			redraw();
			return 1;
		}
		const int localX = Fl::event_x() - x();
		const int localY = Fl::event_y() - y();
		// FL_ENTER arrives when the pointer comes in from outside the widget;
		// without it the first hover over a button drew nothing until the
		// pointer moved again (FLTK sends FL_ENTER before any FL_MOVE).
		if (aEvent == FL_MOVE || aEvent == FL_LEAVE || aEvent == FL_ENTER)
		{
			int hover = -1;
			int button = 0;
			if (aEvent != FL_LEAVE && localY >= 0 && localY < h()
				&& localX >= 0 && localX < w())
			{
				const int idx = RowAt(localY);
				if (idx >= 0 && (size_t)idx < mRows.size())
				{
					hover = idx;
					button = ButtonAt(localX, InRowY(localY, idx));
				}
			}
			if (hover != mHover || button != mHoverButton)
			{
				mHover = hover;
				mHoverButton = button;
				redraw();
			}
			return 1;
		}
		if (aEvent != FL_PUSH)
			return Fl_Widget::handle(aEvent);

		const int idx = RowAt(localY);
		if (idx < 0 || (size_t)idx >= mRows.size())
			return 1;
		// m182: clicking anywhere on a row selects it, so a long list keeps a
		// visible anchor. Selecting is not the action - the buttons still are.
		if (idx != mSelected)
		{
			mSelected = idx;
			redraw();
		}
		const int button = ButtonAt(localX, InRowY(localY, idx));
		if (button == 1 && onInfo)
			onInfo(idx);
		else if (button == 2 && onAction)
			onAction(idx);
		return 1;
	}

private:
	// 0 = row body, 1 = 感叹号, 2 = action button. One hit test shared by the
	// hover pass and the click pass, so the tint and the action never disagree.
	int ButtonAt(int aLocalX, int aInRowY) const
	{
		const int btnY = (kRowH - kControlH) / 2;
		if (aInRowY < btnY || aInRowY >= btnY + kControlH)
			return 0;
		const int actionX = w() - kMargin - ActionColumnWidth();
		const int infoX = actionX - kGap - kInfoW;
		if (aLocalX >= infoX && aLocalX < infoX + kInfoW)
			return 1;
		if (aLocalX >= actionX && aLocalX < actionX + ActionColumnWidth())
			return 2;
		return 0;
	}

	std::vector<RowView> mRows;
	std::string mEmptyText;
	int mTop = 0;
	int mHover = -1;
	int mHoverButton = 0;
	int mSelected = -1;
};

// One Fl::awake hop. `alive` is cleared on close, so a late worker drops its
// payload instead of writing into a destroyed dialog.
template <typename T>
struct Job
{
	std::shared_ptr<bool> alive;
	ScriptDownloadDialog *self;
	T payload;
};

struct InstallOutcome
{
	std::string name;
	bool ok = false;
	std::string error;
};

// m179: 刷新 walks every source on one worker, so the payload is per-source
// and index-aligned with mSources.
struct RefreshOutcome
{
	std::vector<std::string> texts;
	std::vector<std::string> errors;
};

} // namespace

ScriptDownloadDialog::ScriptDownloadDialog(ManifestSource aOfficial,
	const std::string &aManifestUrl, std::vector<CustomCatalogSource> aCustom,
	ScriptDownloadCallbacks aCallbacks)
	: DeckDialog(kDefaultW, kDefaultH, "脚本下载"),
	  mOfficial(aOfficial),
	  mOfficialUrl(aManifestUrl),
	  mCallbacks(std::move(aCallbacks))
{
	// m179: resizable + maximize; the list and the name column both grow.
	SetResizable(true);

	mCustom = std::move(aCustom);
	RebuildSources();
	Build();
	Layout();
}

// m180: one place turns the persisted subscriptions into the ordered source
// list, so adding / removing from 订阅管理 cannot drift from first load.
void ScriptDownloadDialog::RebuildSources()
{
	mSources.clear();
	// m185: the official manifest names itself in scripts.json; "官方" is
	// only the fallback used before the catalog has been read.
	mSources.push_back({ mOfficialUrl, mOfficialName, true });
	for (const CustomCatalogSource &source : mCustom)
	{
		if (!LooksLikeCatalogUrl(source.url))
			continue;
		std::string label = source.name;
		if (label.empty())
			label = CustomSourceLabel(source.url);
		mSources.push_back({ source.url, label.empty() ? source.url : label, false });
	}
}

ScriptDownloadDialog::~ScriptDownloadDialog()
{
	*mAlive = false;
}

void ScriptDownloadDialog::Build()
{
	const ThemeColors &colors = DeckColors();
	begin();

	// ① Search row. The label reads as ordinary text, like every other control
	// label (m147) - no muted colour, no smaller size. StyleInput already gives
	// the input text1 + TextSize().
	mSearchLabel = new Fl_Box(FL_NO_BOX, 0, 0, kLabelW, kRowH, "搜索");
	mSearchLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	mSearchLabel->labelsize(TextSize());
	mSearchLabel->labelcolor(colors.text1);
	// m182: BlinkInput shares the editing area's caret phase and IME handling.
	mSearch = new BlinkInput(0, 0, 0, kControlH);
	StyleInput(mSearch);
	mSearch->textsize(TextSize());
	mSearch->when(FL_WHEN_CHANGED);
	mSearch->callback(OnSearch, this);

	mBtnRefresh = new HoverButton(0, 0, 0, kControlH, "刷新");
	mBtnRefresh->color(colors.background2);
	mBtnRefresh->labelcolor(colors.text1);
	mBtnRefresh->labelsize(TextSize());
	mBtnRefresh->callback(OnRefresh, this);

	// ② 订阅 - m180 replaced the old URL box with a door: managing a list of
	// subscriptions needs a list, and that is its own window.
	mBtnSources = new HoverButton(0, 0, 0, kControlH, "订阅");
	mBtnSources->color(colors.background2);
	mBtnSources->labelcolor(colors.text1);
	mBtnSources->labelsize(TextSize());
	mBtnSources->callback(OnSources, this);

	// ③ Status line.
	mStatus = new Fl_Box(FL_NO_BOX, 0, 0, 0, kStatusH, nullptr);
	mStatus->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
	mStatus->labelsize(SubTextSize());
	mStatus->labelcolor(DownloadBlue());

	// ④ The list. One widget paints all rows and owns the hit test.
	ScriptRows *rows = new ScriptRows(0, 0, 0, kRowH);
	rows->onAction = [this](int aIndex) { OnRowAction(aIndex); };
	rows->onInfo = [this](int aIndex) { OnRowInfo(aIndex); };
	mRows = rows;

	// ⑤ Footer.
	mBtnClose = new HoverButton(0, 0, 0, kControlH, "关闭");
	mBtnClose->color(colors.background2);
	mBtnClose->labelcolor(colors.text1);
	mBtnClose->labelsize(TextSize());
	mBtnClose->callback(OnClose, this);

	end();
	SetFrameOptions(true, true);
}

void ScriptDownloadDialog::Layout()
{
	const int W = DeckClientWidth(this);
	const int H = DeckClientHeight(this);
	int y = ContentTop() + kMargin;

	const int fieldX = kMargin + kLabelW + kGap;
	// m184: FitButtonRow owns fitted widths AND right-aligned positions.
	// 刷新 sits left of 订阅; the search field takes what is left.
	FitButtonRow({ mBtnRefresh, mBtnSources }, W,
		UiControlY(y) + kControlH / 2, kMargin, kGap);
	const int fieldW = mBtnRefresh->x() - kGap - fieldX;

	mSearchLabel->resize(kMargin, y, kLabelW, kRowH);
	mSearch->resize(fieldX, UiControlY(y), fieldW, kControlH);
	y += kRowH + kGap;

	mStatus->resize(kMargin, y, W - 2 * kMargin, kStatusH);
	y += kStatusH + kGap;

	// m182: a full kUiRow for the footer, so the close button's distance to
	// the bottom edge equals the search row's distance below the caption.
	const int footerY = H - kMargin - kRowH;
	int listH = footerY - kGap - y;
	if (listH < kRowH * 2)
		listH = kRowH * 2;
	mRows->resize(kMargin, y, W - 2 * kMargin, listH);

	FitButtonRow({ mBtnClose }, W,
		UiControlY(footerY) + kControlH / 2, kMargin, kGap);

	redraw();
}

void ScriptDownloadDialog::resize(int X, int Y, int W, int H)
{
	DeckDialog::resize(X, Y, W, H);
	// mRows is created in Build(), which runs after the constructor: a resize
	// arriving before that (FLTK's own size changes) must not touch it.
	if (mRows)
		Layout();
}

void ScriptDownloadDialog::Run()
{
	ShowCentered();
	// Local-first: no network on entry. 刷新 and 订阅 are the only ways to fetch.
	LoadAll();
	while (shown())
		Fl::wait();
}

ManifestSource ScriptDownloadDialog::KindFor(int aSourceIndex) const
{
	if (aSourceIndex == 0)
		return mOfficial;
	return ManifestSource::Custom;
}

void ScriptDownloadDialog::SetStatus(const std::string &aText, bool aDanger)
{
	mStatusBuf = aText;
	mStatus->label(mStatusBuf.c_str());
	mStatus->labelcolor(aDanger ? AlertRed() : DownloadBlue());
	mStatus->redraw();
}

void ScriptDownloadDialog::LoadAll()
{
	mView.clear();
	int duplicates = 0;
	int failed = 0;
	std::string firstError;
	for (size_t i = 0; i < mSources.size(); ++i)
	{
		const int index = (int)i;
		std::string text;
		std::string error;
		if (!ReadCatalogCache(KindFor(index), mSources[i].url, text, error))
		{
			++failed;
			if (firstError.empty())
				firstError = error;
			continue;
		}
		MergeText(index, text, &duplicates);
	}
	RebuildView();
	if (failed > 0 && duplicates > 0)
		SetStatus(std::to_string(failed) + " 个来源暂无清单 · 已忽略 "
			+ std::to_string(duplicates) + " 个重名项", false);
	if (mCallbacks.log && duplicates > 0)
		mCallbacks.log("[脚本下载] 多来源合并时忽略 " + std::to_string(duplicates)
			+ " 个重名脚本（先出现的来源优先）");
	if (failed > 0 && mView.empty())
		SetStatus(firstError.empty() ? "点「刷新」获取脚本清单" : firstError, true);
}

void ScriptDownloadDialog::MergeText(int aSourceIndex, const std::string &aText,
	int *aDuplicates)
{
	CatalogResult result = ParseScriptCatalog(aText);
	if (!result.error.empty())
	{
		SetStatus(result.error, true);
		return;
	}
	// m185: source 0 is the official mirror. Its label is whatever the
	// manifest says about itself, so a name change does not need a rebuild.
	if (aSourceIndex == 0 && !result.name.empty())
	{
		mOfficialName = result.name;
		mSources[0].label = result.name;
	}
	if (result.skipped > 0 && mCallbacks.log)
		mCallbacks.log("[脚本下载] " + mSources[(size_t)aSourceIndex].label
			+ " 清单跳过 " + std::to_string(result.skipped) + " 项信息不全的条目");

	const std::string &label = mSources[(size_t)aSourceIndex].label;
	for (CatalogEntry &entry : result.entries)
	{
		// The first source wins a name collision: official first, then the
		// user's sources in subscription order. Two rows with one name would
		// both point at the same <Scripts>\<name>.ahk, so the second could only
		// ever overwrite the first without saying so.
		bool taken = false;
		for (const RowView &existing : mView)
		{
			if (_stricmp(existing.entry.name.c_str(), entry.name.c_str()) == 0)
			{
				taken = true;
				break;
			}
		}
		if (taken)
		{
			if (aDuplicates)
				++*aDuplicates;
			continue;
		}
		RowView view;
		view.entry = entry;
		view.sourceLabel = label;
		view.sourceIndex = aSourceIndex;
		mView.push_back(std::move(view));
	}
}

void ScriptDownloadDialog::RebuildView()
{
	// mView already holds the merged catalog order; this pass only recomputes
	// the per-row state, the action caption and the filter.
	const std::string query = LowerLocal(TrimLocal(mFilter));
	int ready = 0;
	int upgradable = 0;

	std::vector<RowView> visible;
	visible.reserve(mView.size());
	for (const RowView &row : mView)
	{
		if (!query.empty() && !MatchesFilter(row.entry, query))
			continue;

		const std::wstring localPath = Utf8ToWideLocal(
			ScriptsDirUtf8() + row.entry.name + ".ahk");
		const bool exists = !localPath.empty()
			&& GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES;
		const std::string localVersion = exists
			? LocalScriptVersion(localPath) : std::string();

		RowView view = row;
		view.state = ClassifyInstall(row.entry, localVersion, exists);
		const std::string &url = DownloadUrlFor(row.entry, KindFor(row.sourceIndex));

		if (!url.empty() && view.state == ScriptInstallState::Absent)
		{
			view.caption = "下载";
			view.tone = ActionTone::Primary;
			view.clickable = true;
			++ready;
		}
		else if (!url.empty() && view.state == ScriptInstallState::Upgradable)
		{
			view.caption = "升级";
			view.tone = ActionTone::Warning;
			view.clickable = true;
			++upgradable;
		}
		else
		{
			view.caption = (view.state == ScriptInstallState::Installed
				|| view.state == ScriptInstallState::Ahead) ? "已下载" : "下载";
			view.tone = ActionTone::Idle;
			view.clickable = false;
		}

		// Single-slot download: one row shows the percentage, all others wait.
		if (mBusy)
		{
			view.clickable = false;
			if (mDownloadName == row.entry.name)
				view.caption = mDownloadCaption.empty() ? "0%" : mDownloadCaption;
		}
		else if (mPendingConfirm == (int)visible.size() && view.clickable
			&& (view.state == ScriptInstallState::Upgradable
				|| (IsThirdPartyManifest(KindFor(view.sourceIndex))
					&& view.state == ScriptInstallState::Absent)))
		{
			// m181: the pending second click must visibly change the button,
			// otherwise the trust gate looks like a dead click.
			view.caption = "确认";
		}
		visible.push_back(std::move(view));
	}

	ScriptRows *rows = static_cast<ScriptRows *>(mRows);
	rows->SetEmptyText(mView.empty()
		? "（暂无清单，点「刷新」获取）" : "（没有匹配的脚本）");
	rows->SetRows(visible);

	// While a transfer owns the dialog, its own message stays on the status
	// line instead of being overwritten by the summary.
	if (mBusy)
		return;
	std::string status = std::to_string(ready) + " 个可下载";
	if (upgradable > 0)
		status += " · " + std::to_string(upgradable) + " 个可升级";
	if (mSources.size() > 1)
		status += " · " + std::to_string(mSources.size()) + " 个来源";
	if (!query.empty())
		status += " · 匹配 " + std::to_string(visible.size()) + " 个";
	SetStatus(status, false);
}

void ScriptDownloadDialog::OnRowAction(int aIndex)
{
	if (mBusy || aIndex < 0)
		return;
	ScriptRows *rows = static_cast<ScriptRows *>(mRows);
	const std::vector<RowView> &visible = rows->Rows();
	if ((size_t)aIndex >= visible.size())
		return;
	const RowView view = visible[(size_t)aIndex];
	if (!view.clickable)
		return;
	const std::string url = DownloadUrlFor(view.entry, KindFor(view.sourceIndex));
	if (url.empty())
		return;

	const bool overwrite = view.state == ScriptInstallState::Upgradable;
	// m181: a subscribed manifest is a stranger's file. m146 §7 already ruled
	// that the m160 "name+version only" check cannot prove integrity or
	// authorship for it, so installing one is a trust decision the user has to
	// actually see - the same two-click gate that already protects overwriting
	// a local script. Official rows keep their one-click behaviour.
	const ManifestSource kind = KindFor(view.sourceIndex);
	const bool thirdParty = IsThirdPartyManifest(kind);
	const bool needsConfirm = overwrite
		|| (thirdParty && view.state == ScriptInstallState::Absent);
	if (needsConfirm && mPendingConfirm != aIndex)
	{
		mPendingConfirm = aIndex;
		RebuildView();
		if (overwrite)
		{
			SetStatus("本机脚本可能被改过；再点「确认」将先停止运行中的该脚本，旧版留作 .old",
				true);
		}
		else
		{
			// Short enough for the single-line status bar (it clips): put the
			// required action first, keep the full TrustNoticeFor wording in
			// the info window and the subscription window where it can wrap.
			SetStatus("再点「确认」才会安装 · 第三方订阅源无完整性校验（来源："
				+ view.sourceLabel + "）", true);
		}
		return;
	}
	mPendingConfirm = -1;

	if (overwrite && mCallbacks.stopScript
		&& !mCallbacks.stopScript(view.entry.name))
	{
		RebuildView();
		SetStatus("无法停止正在运行的该脚本，已取消", true);
		return;
	}

	const std::string name = view.entry.name;
	const std::string target = ScriptsDirUtf8() + name + ".ahk";
	// m160: the manifest entry is the truth the download is verified against -
	// name and version, nothing else (no size check: the user's rule for
	// official scripts, and entry.sizeKb stays for the 信息 display only).
	const std::string version = view.entry.version;
	mBusy = true;
	mDownloadIndex = aIndex;
	mDownloadName = name;
	mDownloadCaption = "0%";
	SetStatus("下载中…", false);
	RebuildView();

	std::shared_ptr<bool> alive = mAlive;
	try
	{
		std::thread([this, alive, name, url, target, version]() {
			ScriptInstallRequest request;
			request.targetPathUtf8 = target;
			request.url = url;
			request.expectedName = name;
			request.expectedVersion = version;

			auto progress = [this, alive](long long got, long long total) {
				using ProgressJob = Job<std::pair<long long, long long>>;
				auto *job = new ProgressJob{ alive, this, { got, total } };
				Fl::awake([](void *aPayload) {
					auto *j = static_cast<ProgressJob *>(aPayload);
					if (*j->alive)
						j->self->OnProgress(j->payload.first, j->payload.second);
					delete j;
				}, job);
			};

			InstallOutcome outcome;
			outcome.name = name;
			try
			{
				outcome.ok = InstallScriptFile(request, outcome.error, progress);
			}
			catch (...)
			{
				outcome.ok = false;
				outcome.error = "下载线程异常";
			}
			auto *job = new Job<InstallOutcome>{ alive, this, std::move(outcome) };
			Fl::awake([](void *aPayload) {
				auto *j = static_cast<Job<InstallOutcome> *>(aPayload);
				if (*j->alive)
					j->self->ApplyInstallResult(j->payload.name,
						j->payload.ok, j->payload.error);
				delete j;
			}, job);
		}).detach();
	}
	catch (...)
	{
		mBusy = false;
		mDownloadIndex = -1;
		mDownloadName.clear();
		RebuildView();
		SetStatus("无法启动下载线程", true);
	}
}

void ScriptDownloadDialog::OnProgress(long long aReceived, long long aTotal)
{
	char text[32];
	if (aTotal > 0)
	{
		const int pct = (int)(100LL * aReceived / aTotal);
		std::snprintf(text, sizeof(text), "%d%%", pct > 100 ? 100 : pct);
	}
	else
	{
		std::snprintf(text, sizeof(text), "%lld KB", aReceived / 1024);
	}
	mDownloadCaption = text;
	// The percentage rides on the row caption, so the view has to be rebuilt
	// rather than patched: RebuildView re-reads the local state anyway, and a
	// finished install changes it.
	RebuildView();
}

void ScriptDownloadDialog::ApplyInstallResult(const std::string &aName, bool aOk,
	const std::string &aError)
{
	mBusy = false;
	mDownloadIndex = -1;
	mDownloadName.clear();
	mDownloadCaption.clear();
	if (!aOk)
	{
		RebuildView();
		SetStatus(aError.empty() ? "下载失败" : aError, true);
		return;
	}
	if (mCallbacks.refreshScripts)
		mCallbacks.refreshScripts();
	if (mCallbacks.log)
		mCallbacks.log("[脚本下载] 已安装 " + aName);
	RebuildView(); // re-read the local @version: the row turns grey
	SetStatus("已安装 " + aName, false);
}

void ScriptDownloadDialog::StartRefresh()
{
	if (mBusy)
		return;
	mBusy = true;
	SetStatus("更新中…", false);
	RebuildView();

	std::shared_ptr<bool> alive = mAlive;
	std::vector<std::string> urls;
	std::vector<ManifestSource> kinds;
	for (size_t i = 0; i < mSources.size(); ++i)
	{
		urls.push_back(mSources[i].url);
		kinds.push_back(KindFor((int)i));
	}
	try
	{
		std::thread([this, alive, urls, kinds]() {
			RefreshOutcome outcome;
			outcome.texts.resize(urls.size());
			outcome.errors.resize(urls.size());
			for (size_t i = 0; i < urls.size(); ++i)
			{
				std::string error;
				try
				{
					if (!RefreshCatalogCache(kinds[i], urls[i], outcome.texts[i], error))
						outcome.errors[i] = error.empty() ? "清单更新失败" : error;
				}
				catch (...)
				{
					outcome.errors[i] = "清单更新线程异常";
				}
			}
			auto *job = new Job<RefreshOutcome>{ alive, this, std::move(outcome) };
			Fl::awake([](void *aPayload) {
				auto *j = static_cast<Job<RefreshOutcome> *>(aPayload);
				if (*j->alive)
					j->self->ApplyRefresh(j->payload.texts, j->payload.errors);
				delete j;
			}, job);
		}).detach();
	}
	catch (...)
	{
		mBusy = false;
		RebuildView();
		SetStatus("无法启动清单更新线程", true);
	}
}

void ScriptDownloadDialog::ApplyRefresh(const std::vector<std::string> &aTexts,
	const std::vector<std::string> &aErrors)
{
	mBusy = false;
	int ok = 0;
	int failed = 0;
	std::string firstError;
	for (size_t i = 0; i < aErrors.size() && i < mSources.size(); ++i)
	{
		if (!aErrors[i].empty())
		{
			++failed;
			if (firstError.empty())
				firstError = mSources[i].label + "：" + aErrors[i];
			continue;
		}
		++ok;
	}
	// Rebuild the merged list from the sources that DID answer: a dead custom
	// source must not wipe the official catalog out of the window.
	mView.clear();
	int duplicates = 0;
	for (size_t i = 0; i < aTexts.size() && i < mSources.size(); ++i)
	{
		if (!aErrors[i].empty())
			continue;
		MergeText((int)i, aTexts[i], &duplicates);
	}
	RebuildView();
	if (failed == 0)
	{
		SetStatus("已更新 " + std::to_string(ok) + " 个来源", false);
		return;
	}
	if (ok == 0)
	{
		SetStatus(firstError, true);
		return;
	}
	SetStatus("已更新 " + std::to_string(ok) + " 个来源 · " + std::to_string(failed)
		+ " 个失败（" + firstError + "）", true);
}

void ScriptDownloadDialog::OnManageSources()
{
	if (mBusy)
		return;

	// The list itself lives in ui/SubscribeDialog (m180). It persists every
	// change through the same callback this window uses, so all that is left
	// here is to re-read the merged rows when it reports a change - and it has
	// already fetched any newly added manifest, so no second network trip.
	SubscribeCallbacks callbacks;
	callbacks.save = mCallbacks.saveCustom;
	callbacks.log = mCallbacks.log;

	std::vector<CustomCatalogSource> updated;
	if (!ShowSubscribeDialog(mCustom, mRounded, mShadow, callbacks, updated))
		return;

	mCustom = updated;
	RebuildSources();
	LoadAll();
	SetStatus("已更新订阅：" + std::to_string(mCustom.size()) + " 个来源", false);
}

void ScriptDownloadDialog::OnRowInfo(int aIndex) const
{
	ScriptRows *rows = static_cast<ScriptRows *>(mRows);
	const std::vector<RowView> &visible = rows->Rows();
	if (aIndex < 0 || (size_t)aIndex >= visible.size())
		return;
	// m151: the chrome is shared with the row menu's 信息 window; this caller
	// simply feeds it the manifest row instead of the local file's header.
	const RowView &row = visible[(size_t)aIndex];
	const CatalogEntry &entry = row.entry;
	// m181: m146 §7's "source annotation" made visible. A subscribed row is
	// labelled as such, so 来源 is never read as an assurance of authorship.
	// m186: the separate 校验 row is gone - it restated the same thing in
	// protocol wording nobody could act on. The first-download confirmation
	// still carries the full trust sentence where it matters.
	const ManifestSource kind = KindFor(row.sourceIndex);
	const bool thirdParty = IsThirdPartyManifest(kind);
	const std::string sourceText = thirdParty
		? "第三方订阅 · " + row.sourceLabel : row.sourceLabel;
	std::vector<InfoGroup> groups(1);
	groups[0] = {
		{ "名称", entry.name, false, false },
		{ "版本", entry.version, false, false },
		{ "日期", entry.date, false, false },
		{ "作者", entry.author, false, false },
		{ "许可", entry.license, false, false },
		{ "大小", FormatScriptSize((long long)(entry.sizeKb * 1024.0)), false, false },
		{ "来源", sourceText, false, false },
		{ "说明", entry.description, true, false },
		{ "运行环境", entry.requires, true, false },
		{ "宿主要求", entry.host, true, false },
		{ "入口", entry.entry, true, false },
	};
	ShowInfoDialog("脚本详情", groups, true, true);
}

void ScriptDownloadDialog::RequestClose()
{
	*mAlive = false;
	hide();
}

void ScriptDownloadDialog::OnSearch(Fl_Widget *, void *aData)
{
	auto *self = static_cast<ScriptDownloadDialog *>(aData);
	self->mFilter = self->mSearch->value() ? self->mSearch->value() : "";
	self->mPendingConfirm = -1;
	self->RebuildView();
}

void ScriptDownloadDialog::OnRefresh(Fl_Widget *, void *aData)
{
	static_cast<ScriptDownloadDialog *>(aData)->StartRefresh();
}

void ScriptDownloadDialog::OnSources(Fl_Widget *, void *aData)
{
	static_cast<ScriptDownloadDialog *>(aData)->OnManageSources();
}

void ScriptDownloadDialog::OnClose(Fl_Widget *, void *aData)
{
	static_cast<ScriptDownloadDialog *>(aData)->RequestClose();
}

} // namespace ui
} // namespace ahkdeck

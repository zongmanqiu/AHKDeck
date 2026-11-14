// ui/SubscribeDialog.cpp - see ui/SubscribeDialog.h (m180).
//
// Layout mirrors ScriptDownloadDialog: one 30px control row at the top, a status
// line, a self-painted list that takes the rest, a right-aligned 关闭 at the
// bottom, and every y derived from w()/h() so the window can be resized.
//
// The list is the whole point of the window, so it is a real table: 名称 /
// 地址 / 移除. Names come from the manifest itself (top-level "name"), which
// is why subscribing has to fetch before the row can appear - a URL alone
// cannot title a source, and inventing a "订阅 1 / 订阅 2" label would be a
// worse answer than the host name it already falls back to.
#include "ui/SubscribeDialog.h"

#include "ui/BlinkInput.h"
#include "ui/DeckDialog.h"
#include "ui/DeckFrame.h"
#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"
#include "ui/UiMetrics.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cstring>
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

// m182: alias the shared layout vocabulary; this window invents no numbers.
constexpr int kMargin = kUiEdge;
constexpr int kGap = kUiGap;
constexpr int kRowH = kUiRow;
constexpr int kControlH = kUiControl;
constexpr int kLabelW = kUiLabelW;
// m184: width follows the label. Widget buttons go through FitButtonRow;
// the hand-painted 移除 cell uses the shared painted-cell rule.
int RemoveButtonWidth() { return UiCellButtonWidth("移除"); }
// The name column takes a share of the width rather than a fixed 200px: a
// publisher can call their catalog anything, and a fixed column either wastes
// room on short names or clips long ones. 42% fits ~25 CJK-free characters at
// the default size and grows with the window.
constexpr int kNamePct = 42;
constexpr int kListPad = kUiListPad;

constexpr int kDefaultW = 648;
// m186: 262 showed only four rows, which felt cramped once a user collects a
// handful of sources. The height is derived from the real card metrics: the
// framed title bar is 49 (not the 30 the client rect starts below), the card
// needs 2*8 inset + 8 rows*28 = 240, and the rows above/below it are
// edge 16 + field 28 + gap 8 + gap 8 + footer 28 + edge 16 = 104. 49 + 240 +
// 104 = 393, plus a 4px allowance for the card's own 1px border pair so the
// eighth row is whole instead of starting a 4px scroll.
constexpr int kDefaultH = 397;

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

// The same gate ScriptDownloadDialog applies: the worker only touches an https
// URL, and never one carrying whitespace or control bytes.
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

// The title a row shows: the manifest's own name, else the host. A source we
// have never managed to read has no name at all, and the URL is the only
// honest thing left to print.
std::string LabelOf(const CustomCatalogSource &aSource)
{
	if (!aSource.name.empty())
		return aSource.name;
	const std::string host = CustomSourceLabel(aSource.url);
	return host.empty() ? aSource.url : host;
}

// m182: the one painted button, shared with the script window's cells.
void PaintCell(int aX, int aY, int aW, int aH, const std::string &aText,
	Fl_Color aFill, bool aHovered)
{
	UiDrawCellButton(aX, aY, aW, aH, aText.c_str(), aFill,
		DeckColors().text1, aHovered);
}

class SourceRows : public Fl_Widget
{
public:
	SourceRows(int aX, int aY, int aW, int aH)
		: Fl_Widget(aX, aY, aW, aH)
	{
	}

	void SetRows(const std::vector<CustomCatalogSource> &aRows)
	{
		mRows = aRows;
		const int maxTop = (int)mRows.size() - VisibleRows();
		if (mTop > maxTop)
			mTop = maxTop > 0 ? maxTop : 0;
		if (mHover >= (int)mRows.size())
			mHover = -1;
		if (mSelected >= (int)mRows.size())
			mSelected = -1;
		mHoverButton = false;
		redraw();
	}

	void SetEmptyText(const std::string &aText)
	{
		mEmptyText = aText;
		redraw();
	}

	std::function<void(int)> onRemove;

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
		const int nameW = w() * kNamePct / 100;
		const int urlX = x() + kMargin + nameW + kGap;
		const int removeX = x() + w() - kMargin - RemoveButtonWidth();
		const int urlW = removeX - kGap - urlX;
		const int btnY = (kRowH - kControlH) / 2;

		for (int v = 0; v < visible && (size_t)(mTop + v) < mRows.size(); ++v)
		{
			const CustomCatalogSource &source = mRows[(size_t)(mTop + v)];
			const int ry = y() + kListPad + v * kRowH;
			const int idx = mTop + v;
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

			fl_font(FL_HELVETICA, TextSize());
			fl_color(colors.text1);
			fl_draw(UiFitText(LabelOf(source), nameW, TextSize()).c_str(),
				x() + kMargin, ry, nameW, kRowH,
				FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

			fl_font(FL_HELVETICA, SubTextSize());
			fl_color(colors.text2);
			fl_draw(UiFitText(source.url, urlW > 0 ? urlW : 0,
					SubTextSize()).c_str(),
				urlX, ry, urlW > 0 ? urlW : 0, kRowH,
				FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

			PaintCell(removeX, ry + btnY, RemoveButtonWidth(), kControlH, "移除",
				colors.background2, mHover == idx && mHoverButton);
		}

		if ((int)mRows.size() > visible)
		{
			const int trackX = x() + w() - 5;
			fl_color(colors.background2);
			const int bandY = y() + kListPad;
			const int bandH = h() - 2 * kListPad;
			fl_rectf(trackX, bandY, 3, bandH);
			const int span = (int)mRows.size() - visible;
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
		// FL_ENTER is the first event when the pointer arrives from outside.
		if (aEvent == FL_MOVE || aEvent == FL_LEAVE || aEvent == FL_ENTER)
		{
			int hover = -1;
			bool button = false;
			if (aEvent != FL_LEAVE && localY >= 0 && localY < h()
				&& localX >= 0 && localX < w())
			{
				const int idx = RowAt(localY);
				if (idx >= 0 && (size_t)idx < mRows.size())
				{
					hover = idx;
					button = OverRemove(localX, InRowY(localY, idx));
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
		if (idx != mSelected)
		{
			mSelected = idx;
			redraw();
		}
		if (OverRemove(localX, InRowY(localY, idx)) && onRemove)
			onRemove(idx);
		return 1;
	}

private:
	bool OverRemove(int aLocalX, int aInRowY) const
	{
		const int removeX = w() - kMargin - RemoveButtonWidth();
		const int btnY = (kRowH - kControlH) / 2;
		return aLocalX >= removeX && aLocalX < removeX + RemoveButtonWidth()
			&& aInRowY >= btnY && aInRowY < btnY + kControlH;
	}

	std::vector<CustomCatalogSource> mRows;
	std::string mEmptyText;
	int mTop = 0;
	int mHover = -1;
	int mSelected = -1;
	bool mHoverButton = false;
};

// One Fl::awake hop, guarded by `alive` (cleared on close) exactly like the
// download window: a late worker must not write into a destroyed dialog.
struct SubscribeOutcome
{
	std::string url;
	bool ok = false;
	std::string name;
	std::string error;
};

class SubscribeDialog : public DeckDialog
{
public:
	SubscribeDialog(std::vector<CustomCatalogSource> aSources, bool aRounded,
		bool aShadow, const SubscribeCallbacks &aCallbacks)
		: DeckDialog(kDefaultW, kDefaultH, "订阅管理"),
		  mCallbacks(aCallbacks)
	{
		for (const CustomCatalogSource &source : aSources)
		{
			if (LooksLikeCatalogUrl(source.url))
				mSources.push_back(source);
		}
		SetResizable(true);
		// m183: three rows of subscriptions do not need a 400px minimum.
		SetMinClientHeight(kDefaultH);
		SetFrameOptions(aRounded, aShadow);
	}

	~SubscribeDialog() override { *mAlive = false; }

	void Build()
	{
		const ThemeColors &colors = DeckColors();
		begin();

		mInputLabel = new Fl_Box(FL_NO_BOX, 0, 0, kLabelW, kRowH, nullptr);
		mInputLabel->copy_label("地址");
		mInputLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		mInputLabel->labelsize(TextSize());
		mInputLabel->labelcolor(colors.text1);

		// m182: BlinkInput shares the editor's 500ms caret phase and IME
		// handling. A bare Fl_Input never blinked, so this window's field felt
		// like a different application from the editing area.
		mInput = new BlinkInput(0, 0, 0, kControlH);
		StyleInput(mInput);
		mInput->textsize(TextSize());

		mBtnAdd = new HoverButton(0, 0, 0, kControlH, "订阅");
		mBtnAdd->color(colors.background2);
		mBtnAdd->labelcolor(colors.text1);
		mBtnAdd->labelsize(TextSize());
		mBtnAdd->callback(OnAdd, this);

		mStatus = new Fl_Box(FL_NO_BOX, 0, 0, 0, kControlH, nullptr);
		mStatus->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
		mStatus->labelsize(SubTextSize());
		mStatus->labelcolor(DownloadBlue());

		SourceRows *rows = new SourceRows(0, 0, 0, kRowH);
		rows->onRemove = [this](int aIndex) { RemoveAt(aIndex); };
		mRows = rows;

		mBtnClose = new HoverButton(0, 0, 0, kControlH, "关闭");
		mBtnClose->color(colors.background2);
		mBtnClose->labelcolor(colors.text1);
		mBtnClose->labelsize(TextSize());
		mBtnClose->callback(OnClose, this);

		end();
		RefreshNames();
		SyncRows();
	}

	void Layout()
	{
		const int W = DeckClientWidth(this);
		const int H = DeckClientHeight(this);
		int y = ContentTop() + kMargin;

		const int fieldX = kMargin + kLabelW + kGap;
		FitButtonRow({ mBtnAdd }, W, UiControlY(y) + kControlH / 2,
			kMargin, kGap);
		const int fieldW = mBtnAdd->x() - kGap - fieldX;
		mInputLabel->resize(kMargin, y, kLabelW, kRowH);
		mInput->resize(fieldX, UiControlY(y),
			fieldW > 60 ? fieldW : 60, kControlH);
		y += kRowH + kGap;

		// m182: the footer is a full kUiRow, so the close button sits the same
		// 17px above the bottom edge as the first row's controls sit below the
		// caption. Previously the footer used kControlH and the first row used
		// (kRowH-kControlH)/2, which produced 16 vs 21 - visibly asymmetric.
		const int footerY = H - kMargin - kRowH;
		int listH = footerY - kGap - y;
		if (listH < kRowH * 2)
			listH = kRowH * 2;
		mRows->resize(kMargin, y, W - 2 * kMargin, listH);

		FitButtonRow({ mBtnClose }, W,
			UiControlY(footerY) + kControlH / 2, kMargin, kGap);
		// m185: the status is an operation RESULT, not a permanent caption.
		// It lives in the footer's empty left half, so it costs no row and
		// the list can start one gap below the address field.
		const int statusW = mBtnClose->x() - kGap - kMargin;
		mStatus->resize(kMargin, UiControlY(footerY),
			statusW > 0 ? statusW : 0, kControlH);
		redraw();
	}

	void Run()
	{
		ShowCentered();
		while (shown())
			Fl::wait();
	}

	const std::vector<CustomCatalogSource> &Sources() const { return mSources; }
	bool Changed() const { return mChanged; }

	void OnCaptionClose() override { hide(); }

private:
	void resize(int X, int Y, int W, int H) override
	{
		DeckDialog::resize(X, Y, W, H);
		if (mRows)
			Layout();
	}

	void SyncRows()
	{
		SourceRows *rows = static_cast<SourceRows *>(mRows);
		rows->SetEmptyText("还没有订阅");
		rows->SetRows(mSources);
	}

	void SetStatus(const std::string &aText, bool aDanger = false)
	{
		mStatusBuf = aText;
		mStatus->label(mStatusBuf.c_str());
		mStatus->labelcolor(aDanger ? AlertRed() : DownloadBlue());
		mStatus->redraw();
	}

	void Persist(bool aMarkChanged = true)
	{
		if (aMarkChanged)
			mChanged = true;
		if (mCallbacks.save)
			mCallbacks.save(mSources);
	}

	// The manifest is the truth about its own name, so the cached copy is read
	// on entry and the stored name follows it. Without this, a publisher who
	// renames their catalog would leave every subscriber showing the old title
	// until they removed and re-added the URL. A cache miss (temp cleaned, or
	// never fetched) simply keeps the name we already had.
	void RefreshNames()
	{
		bool renamed = false;
		for (CustomCatalogSource &source : mSources)
		{
			std::string text;
			std::string error;
			if (!ReadCatalogCache(ManifestSource::Custom, source.url, text, error))
				continue;
			const CatalogResult parsed = ParseScriptCatalog(text);
			if (!parsed.error.empty() || parsed.name.empty()
				|| parsed.name == source.name)
				continue;
			source.name = parsed.name;
			renamed = true;
		}
		if (renamed)
			Persist(false);
	}

	int IndexOf(const std::string &aUrl) const
	{
		for (size_t i = 0; i < mSources.size(); ++i)
		{
			if (_stricmp(mSources[i].url.c_str(), aUrl.c_str()) == 0)
				return (int)i;
		}
		return -1;
	}

	void RemoveAt(int aIndex)
	{
		if (mBusy || aIndex < 0 || (size_t)aIndex >= mSources.size())
			return;
		const std::string label = LabelOf(mSources[(size_t)aIndex]);
		mSources.erase(mSources.begin() + aIndex);
		SyncRows();
		Persist();
		if (mCallbacks.log)
			mCallbacks.log("[订阅] 已移除来源 " + label);
		SetStatus("已移除 " + label, false);
	}

	void StartAdd(const std::string &aUrl)
	{
		if (mBusy)
			return;
		if (!LooksLikeCatalogUrl(aUrl))
		{
			SetStatus("请输入以 https:// 开头的 json 清单地址", true);
			return;
		}
		if (IndexOf(aUrl) >= 0)
		{
			SetStatus("该地址已在列表中", true);
			return;
		}

		mBusy = true;
		mBtnAdd->deactivate();
		SetStatus("正在读取该清单…", false);

		std::shared_ptr<bool> alive = mAlive;
		try
		{
			std::thread([this, alive, aUrl]() {
				SubscribeOutcome outcome;
				outcome.url = aUrl;
				try
				{
					CatalogResult fetched;
					std::string text;
					std::string error;
					if (!RefreshCatalogCache(ManifestSource::Custom, aUrl, text, error))
					{
						outcome.error = error.empty() ? "该地址不是有效的清单" : error;
					}
					else
					{
						// RefreshCatalogCache already parsed it once to prove it
						// is a catalog; parsing again here is what yields the
						// name the publisher gave itself.
						fetched = ParseScriptCatalog(text);
						if (!fetched.error.empty())
						{
							outcome.error = fetched.error;
						}
						else
						{
							outcome.ok = true;
							outcome.name = fetched.name;
						}
					}
				}
				catch (...)
				{
					outcome.ok = false;
					outcome.error = "清单读取线程异常";
				}

				struct Job
				{
					std::shared_ptr<bool> alive;
					SubscribeDialog *self;
					SubscribeOutcome payload;
				};
				auto *job = new Job{ alive, this, std::move(outcome) };
				Fl::awake([](void *aPayload) {
					auto *j = static_cast<Job *>(aPayload);
					if (*j->alive)
						j->self->ApplyAdd(j->payload);
					delete j;
				}, job);
			}).detach();
		}
		catch (...)
		{
			mBusy = false;
			mBtnAdd->activate();
			SetStatus("无法启动清单读取线程", true);
		}
	}

	void ApplyAdd(const SubscribeOutcome &aOutcome)
	{
		mBusy = false;
		mBtnAdd->activate();
		if (!aOutcome.ok)
		{
			SetStatus(aOutcome.error.empty() ? "该地址不是有效的清单" : aOutcome.error,
				true);
			return;
		}

		CustomCatalogSource source;
		source.url = aOutcome.url;
		source.name = aOutcome.name;
		mSources.push_back(source);
		SyncRows();
		Persist();

		mInput->value("");
		const std::string label = LabelOf(source);
		if (mCallbacks.log)
			mCallbacks.log("[订阅] 已订阅 " + label
				+ (source.name.empty() ? "（清单未提供名称，显示域名）" : ""));
		SetStatus("已订阅 " + label
			+ (source.name.empty() ? "（清单未提供名称，显示域名）" : ""), false);
	}

	static void OnAdd(Fl_Widget *, void *aData)
	{
		auto *self = static_cast<SubscribeDialog *>(aData);
		if (self->mBusy)
			return;
		const std::string url = TrimLocal(self->mInput->value()
			? self->mInput->value() : "");
		if (url.empty())
		{
			self->SetStatus("请先填写 json 清单地址", true);
			return;
		}
		self->StartAdd(url);
	}

	static void OnClose(Fl_Widget *, void *aData)
	{
		static_cast<SubscribeDialog *>(aData)->hide();
	}

	int handle(int aEvent) override
	{
		if (aEvent == FL_KEYDOWN && Fl::event_key() == FL_Escape)
		{
			hide();
			return 1;
		}
		return DeckDialog::handle(aEvent);
	}

	bool mChanged = false;
	bool mBusy = false;
	SubscribeCallbacks mCallbacks;
	std::vector<CustomCatalogSource> mSources;
	std::string mStatusBuf;
	std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);

	Fl_Box *mInputLabel = nullptr;
	Fl_Input *mInput = nullptr;
	HoverButton *mBtnAdd = nullptr;
	Fl_Box *mStatus = nullptr;
	Fl_Widget *mRows = nullptr;
	HoverButton *mBtnClose = nullptr;
};

} // namespace

bool ShowSubscribeDialog(const std::vector<CustomCatalogSource> &aSources,
	bool aRounded, bool aShadow, const SubscribeCallbacks &aCallbacks,
	std::vector<CustomCatalogSource> &aOut)
{
	SubscribeDialog dialog(aSources, aRounded, aShadow, aCallbacks);
	dialog.Build();
	dialog.Layout();
	dialog.Run();
	aOut = dialog.Sources();
	return dialog.Changed();
}

} // namespace ui
} // namespace ahkdeck

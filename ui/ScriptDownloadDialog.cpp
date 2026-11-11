// ui/ScriptDownloadDialog.cpp - see ui/ScriptDownloadDialog.h (m146).
#include "ui/ScriptDownloadDialog.h"

#include "ui/DeckTitleBar.h"
#include "ui/SourceChoice.h"
#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"

#include "core/ScriptCatalog.h"
#include "core/Updater.h"

#include <windows.h>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
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

// The deck's dialog rhythm (m087) with the list card's 40px rows.
constexpr int kMargin = 16;
constexpr int kGap = 8;
constexpr int kRowH = 40;
constexpr int kControlH = 30;
constexpr int kBtnH = 26;
constexpr int kWidth = 560;
constexpr int kListH = 200;
constexpr int kActionW = 64;
constexpr int kInfoW = 28;
constexpr int kVersionW = 56;
constexpr int kChoiceW = 96;
constexpr int kRefreshW = 72;

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

bool MatchesFilter(const CatalogEntry &aEntry, const std::string &aQuery)
{
	return LowerLocal(aEntry.name).find(aQuery) != std::string::npos
		|| LowerLocal(aEntry.version).find(aQuery) != std::string::npos
		|| LowerLocal(aEntry.description).find(aQuery) != std::string::npos
		|| LowerLocal(aEntry.author).find(aQuery) != std::string::npos;
}

void DrawFlatButton(int aX, int aY, int aW, int aH, const std::string &aText,
	Fl_Color aFill, Fl_Color aTextColour)
{
	fl_color(aFill);
	fl_rectf(aX, aY, aW, aH);
	fl_color(ThemeBorderLine());
	fl_rect(aX, aY, aW, aH);
	fl_font(FL_HELVETICA, SubTextSize());
	fl_color(aTextColour);
	fl_draw(aText.c_str(), aX, aY, aW, aH, FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
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
		redraw();
	}

	void SetEmptyText(const std::string &aText)
	{
		mEmptyText = aText;
		redraw();
	}

	std::function<void(int)> onAction;
	std::function<void(int)> onInfo;

	int VisibleRows() const { return h() / kRowH; }

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
			fl_draw(mEmptyText.c_str(), x() + kMargin, y(), w() - 2 * kMargin,
				kRowH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
			return;
		}

		const int visible = VisibleRows();
		const int actionX = x() + w() - kMargin - kActionW;
		const int infoX = actionX - kGap - kInfoW;
		const int versionX = infoX - kGap - kVersionW;
		const int btnY = (kRowH - kBtnH) / 2;

		for (int v = 0; v < visible && (size_t)(mTop + v) < mRows.size(); ++v)
		{
			const RowView &row = mRows[(size_t)(mTop + v)];
			const int ry = y() + v * kRowH;
			if (mHover == mTop + v)
			{
				fl_color(colors.highlight1);
				fl_rectf(x() + 1, ry + 1, w() - 2, kRowH - 2);
			}
			fl_color(ThemeBorderLine());
			fl_line(x() + kMargin, ry + kRowH - 1,
				x() + w() - kMargin, ry + kRowH - 1);

			fl_font(FL_HELVETICA, TextSize());
			fl_color(colors.text1);
			fl_draw(row.entry.name.c_str(), x() + kMargin, ry,
				versionX - x() - kMargin - kGap, kRowH,
				FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

			fl_font(FL_HELVETICA, SubTextSize());
			fl_color(colors.text2);
			fl_draw(row.entry.version.c_str(), versionX, ry, kVersionW, kRowH,
				FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

			DrawFlatButton(infoX, ry + btnY, kInfoW, kBtnH, "!",
				colors.background2, colors.text1);

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
			DrawFlatButton(actionX, ry + btnY, kActionW, kBtnH, row.caption,
				fill, text);
		}

		if ((int)mRows.size() > visible)
		{
			const int trackX = x() + w() - 5;
			fl_color(colors.background2);
			fl_rectf(trackX, y() + 1, 3, h() - 2);
			const int span = (int)mRows.size() - visible;
			// No std::max here: windows.h defines max as a macro.
			const int computed = (h() - 2) * visible / (int)mRows.size();
			const int thumbH = computed > 20 ? computed : 20;
			const int thumbY = y() + 1 + (h() - 2 - thumbH) * mTop / (span > 0 ? span : 1);
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
		if (aEvent == FL_MOVE || aEvent == FL_LEAVE)
		{
			int hover = -1;
			if (aEvent == FL_MOVE && localY >= 0 && localY < h()
				&& localX >= 0 && localX < w())
			{
				const int idx = mTop + localY / kRowH;
				if (idx >= 0 && (size_t)idx < mRows.size())
					hover = idx;
			}
			if (hover != mHover)
			{
				mHover = hover;
				redraw();
			}
			return 1;
		}
		if (aEvent != FL_PUSH)
			return Fl_Widget::handle(aEvent);

		const int idx = mTop + localY / kRowH;
		if (idx < 0 || (size_t)idx >= mRows.size())
			return 1;
		const int inRowY = localY - (idx - mTop) * kRowH;
		const int btnY = (kRowH - kBtnH) / 2;
		if (inRowY < btnY || inRowY >= btnY + kBtnH)
			return 1;
		const int actionX = w() - kMargin - kActionW;
		const int infoX = actionX - kGap - kInfoW;
		if (localX >= infoX && localX < infoX + kInfoW)
		{
			if (onInfo)
				onInfo(idx);
		}
		else if (localX >= actionX && localX < actionX + kActionW)
		{
			if (onAction)
				onAction(idx);
		}
		return 1;
	}

private:
	std::vector<RowView> mRows;
	std::string mEmptyText;
	int mTop = 0;
	int mHover = -1;
};

// The 「!」 detail window. Owned, centred and modal like every other deck
// dialog; every field comes from the manifest, not from the local file.
class ScriptInfoDialog : public DeckDialog
{
public:
	explicit ScriptInfoDialog(const CatalogEntry &aEntry)
		: DeckDialog(480, DeckTitleBar::kHeight + 350, "脚本详情")
	{
		const ThemeColors &colors = DeckColors();
		char size[32];
		std::snprintf(size, sizeof(size), "%.1f KB", aEntry.sizeKb);
		struct Field
		{
			const char *label;
			std::string value;
			int h;
		};
		const std::vector<Field> fields = {
			{"名称", aEntry.name, 24},
			{"版本", aEntry.version, 24},
			{"日期", aEntry.date, 24},
			{"作者", aEntry.author, 24},
			{"许可", aEntry.license, 24},
			{"大小", size, 24},
			{"说明", aEntry.description, 48},
			{"运行环境", aEntry.requires, 24},
			{"宿主要求", aEntry.host, 24},
			{"入口", aEntry.entry, 24},
		};

		begin();
		int y = ContentTop() + kMargin;
		for (const Field &field : fields)
		{
			const std::string text = std::string(field.label) + "："
				+ (field.value.empty() ? std::string("（无）") : field.value);
			Fl_Box *box = new Fl_Box(FL_NO_BOX, kMargin, y,
				480 - 2 * kMargin, field.h, nullptr);
			box->copy_label(text.c_str());
			box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
			box->labelsize(SubTextSize());
			box->labelcolor(colors.text1);
			y += field.h + 2;
		}
		mBtnClose = new HoverButton(0, 0, 0, kControlH, "关闭");
		mBtnClose->color(colors.background2);
		mBtnClose->labelcolor(colors.text1);
		mBtnClose->labelsize(TextSize());
		mBtnClose->callback(OnClose, this);
		FitButtonRow({ mBtnClose }, 480, y + kControlH / 2, kMargin, kGap);
		end();
	}

	void Run()
	{
		ShowCentered();
		while (shown())
			Fl::wait();
	}

private:
	static void OnClose(Fl_Widget *, void *aData)
	{
		static_cast<ScriptInfoDialog *>(aData)->hide();
	}

	HoverButton *mBtnClose = nullptr;
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

struct RefreshOutcome
{
	std::string text;
	bool ok = false;
	std::string error;
};

} // namespace

ScriptDownloadDialog::ScriptDownloadDialog(const std::string &aManifestGitee,
	const std::string &aManifestGithub, int aSource,
	ScriptDownloadCallbacks aCallbacks)
	: DeckDialog(kWidth, DeckTitleBar::kHeight + 376, "脚本下载"),
	  mManifestGitee(aManifestGitee),
	  mManifestGithub(aManifestGithub),
	  mCallbacks(std::move(aCallbacks)),
	  mSource(aSource == 1 ? 1 : 0)
{
	const ThemeColors &colors = DeckColors();
	const int top = ContentTop();
	begin();

	int y = top + kMargin;

	// ① Search row: a plain label plus the input (FLTK 1.4 has no placeholder).
	// m147: the label reads as ordinary text, like every other control label -
	// no muted colour, no smaller size. The input itself is already themed by
	// StyleInput to text1 + TextSize().
	Fl_Box *searchLabel = new Fl_Box(FL_NO_BOX, kMargin, y, 48, kRowH, "搜索");
	searchLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	searchLabel->labelsize(TextSize());
	searchLabel->labelcolor(colors.text1);
	mSearch = new Fl_Input(kMargin + 48 + kGap, y + (kRowH - kControlH) / 2,
		kWidth - 2 * kMargin - 48 - kGap, kControlH);
	StyleInput(mSearch);
	mSearch->textsize(TextSize());
	mSearch->when(FL_WHEN_CHANGED);
	mSearch->callback(OnSearch, this);
	y += kRowH + kGap;

	// ② Status row: summary on the left, source + 刷新 on the right.
	const int refreshX = kWidth - kMargin - kRefreshW;
	const int choiceX = refreshX - kGap - kChoiceW;
	mStatus = new Fl_Box(FL_NO_BOX, kMargin, y,
		choiceX - kGap - kMargin, kRowH, nullptr);
	mStatus->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
	mStatus->labelsize(SubTextSize());
	mStatus->labelcolor(DownloadBlue());

	// m147: same factory as the updater's 更新源 dropdown - one item list, one
	// default, one text size. The old SubTextSize() override is gone.
	mSourceChoice = MakeSourceChoice(choiceX, y + (kRowH - kControlH) / 2,
		kChoiceW, kControlH, mSource, OnSource, this);

	mBtnRefresh = new HoverButton(refreshX, y + (kRowH - kControlH) / 2,
		kRefreshW, kControlH, "刷新");
	mBtnRefresh->color(colors.background2);
	mBtnRefresh->labelcolor(colors.text1);
	mBtnRefresh->labelsize(TextSize());
	mBtnRefresh->callback(OnRefresh, this);
	y += kRowH + kGap;

	// ③ The list. One widget paints all rows and owns the hit test.
	ScriptRows *rows = new ScriptRows(kMargin, y, kWidth - 2 * kMargin, kListH);
	rows->onAction = [this](int aIndex) { OnRowAction(aIndex); };
	rows->onInfo = [this](int aIndex) { OnRowInfo(aIndex); };
	mRows = rows;
	y += kListH + kGap;

	// ④ Footer, right-aligned and pinned to the bottom.
	mBtnClose = new HoverButton(0, 0, 0, kControlH, "关闭");
	mBtnClose->color(colors.background2);
	mBtnClose->labelcolor(colors.text1);
	mBtnClose->labelsize(TextSize());
	mBtnClose->callback(OnClose, this);
	FitButtonRow({ mBtnClose }, kWidth, y + kRowH / 2, kMargin, kGap);

	end();
	SetFrameOptions(true, true);
}

ScriptDownloadDialog::~ScriptDownloadDialog()
{
	*mAlive = false;
}

void ScriptDownloadDialog::Run()
{
	ShowCentered();
	// Local-first: no network on entry. 刷新 is the only way to fetch.
	LoadBundled();
	while (shown())
		Fl::wait();
}

void ScriptDownloadDialog::SetStatus(const std::string &aText, bool aDanger)
{
	mStatusBuf = aText;
	mStatus->label(mStatusBuf.c_str());
	mStatus->labelcolor(aDanger ? AlertRed() : DownloadBlue());
	mStatus->redraw();
}

void ScriptDownloadDialog::LoadBundled()
{
	std::string text;
	std::string error;
	if (!ReadBundledCatalog(text, error))
	{
		mCatalog.clear();
		RebuildView();
		SetStatus(error.empty() ? "点「刷新」获取脚本清单" : error, true);
		return;
	}
	ApplyCatalogText(text, "");
}

void ScriptDownloadDialog::ApplyCatalogText(const std::string &aText,
	const std::string &aNote)
{
	CatalogResult result = ParseScriptCatalog(aText);
	if (!result.error.empty())
	{
		SetStatus(result.error, true);
		return;
	}
	mCatalog = std::move(result.entries);
	mPendingConfirm = -1;
	RebuildView();
	if (result.skipped > 0 && mCallbacks.log)
		mCallbacks.log("[脚本下载] 清单跳过 " + std::to_string(result.skipped)
			+ " 项信息不全的条目");
	if (!aNote.empty() && mCallbacks.log)
		mCallbacks.log("[脚本下载] " + aNote);
}

void ScriptDownloadDialog::RebuildView()
{
	mView.clear();
	const std::string query = LowerLocal(TrimLocal(mFilter));
	int ready = 0;
	int upgradable = 0;

	for (const CatalogEntry &entry : mCatalog)
	{
		if (!query.empty() && !MatchesFilter(entry, query))
			continue;

		const std::wstring localPath = Utf8ToWideLocal(
			ScriptsDirUtf8() + entry.name + ".ahk");
		const bool exists = !localPath.empty()
			&& GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES;
		const std::string localVersion = exists
			? LocalScriptVersion(localPath) : std::string();

		RowView view;
		view.entry = entry;
		view.state = ClassifyInstall(entry, localVersion, exists);
		const std::string &url = DownloadUrlFor(entry,
			mSource == 1 ? CatalogSource::Github : CatalogSource::Gitee);

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

		const int index = (int)mView.size();
		// Single-slot download: one row shows the percentage, all others wait.
		if (mBusy)
		{
			view.clickable = false;
			if (mDownloadName == entry.name)
				view.caption = mDownloadCaption.empty() ? "0%" : mDownloadCaption;
		}
		else if (mPendingConfirm == index && view.clickable
			&& view.state == ScriptInstallState::Upgradable)
		{
			view.caption = "确认";
		}
		mView.push_back(std::move(view));
	}

	ScriptRows *rows = static_cast<ScriptRows *>(mRows);
	rows->SetEmptyText(mCatalog.empty()
		? "（暂无清单，点「刷新」获取）" : "（没有匹配的脚本）");
	rows->SetRows(mView);

	// While a transfer owns the dialog, its own message stays on the status
	// line instead of being overwritten by the summary.
	if (mBusy)
		return;
	std::string status = std::to_string(ready) + " 个可下载";
	if (upgradable > 0)
		status += " · " + std::to_string(upgradable) + " 个可升级";
	if (!query.empty())
		status += " · 匹配 " + std::to_string(mView.size()) + " 个";
	SetStatus(status, false);
}

void ScriptDownloadDialog::OnRowAction(int aIndex)
{
	if (mBusy || aIndex < 0 || (size_t)aIndex >= mView.size())
		return;
	const RowView &view = mView[(size_t)aIndex];
	if (!view.clickable)
		return;
	const std::string url = DownloadUrlFor(view.entry,
		mSource == 1 ? CatalogSource::Github : CatalogSource::Gitee);
	if (url.empty())
		return;

	const bool overwrite = view.state == ScriptInstallState::Upgradable;
	if (overwrite && mPendingConfirm != aIndex)
	{
		mPendingConfirm = aIndex;
		RebuildView();
		SetStatus("本机脚本可能被改过；再点「确认」将先停止运行中的该脚本，旧版留作 .old", true);
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
	const double sizeKb = view.entry.sizeKb;
	mBusy = true;
	mDownloadIndex = aIndex;
	mDownloadName = name;
	mDownloadCaption = "0%";
	SetStatus("下载中…", false);
	RebuildView();

	std::shared_ptr<bool> alive = mAlive;
	try
	{
		std::thread([this, alive, name, url, target, sizeKb]() {
			ScriptInstallRequest request;
			request.targetPathUtf8 = target;
			request.url = url;
			request.expectedSizeKb = sizeKb;

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
	for (RowView &view : mView)
	{
		if (view.entry.name == mDownloadName)
			view.caption = mDownloadCaption;
	}
	static_cast<ScriptRows *>(mRows)->SetRows(mView);
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
	const std::string url = mSource == 1 ? mManifestGithub : mManifestGitee;
	if (url.empty())
	{
		SetStatus("该来源没有配置清单地址", true);
		return;
	}
	mBusy = true;
	SetStatus("更新中…", false);
	RebuildView();

	std::shared_ptr<bool> alive = mAlive;
	try
	{
		std::thread([this, alive, url]() {
			RefreshOutcome outcome;
			try
			{
				outcome.ok = RefreshBundledCatalog(url, outcome.text,
					outcome.error);
			}
			catch (...)
			{
				outcome.ok = false;
				outcome.error = "清单更新线程异常";
			}
			auto *job = new Job<RefreshOutcome>{ alive, this, std::move(outcome) };
			Fl::awake([](void *aPayload) {
				auto *j = static_cast<Job<RefreshOutcome> *>(aPayload);
				if (*j->alive)
					j->self->ApplyRefresh(j->payload.ok,
						j->payload.text, j->payload.error);
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

void ScriptDownloadDialog::ApplyRefresh(bool aOk, const std::string &aText,
	const std::string &aError)
{
	mBusy = false;
	if (!aOk)
	{
		RebuildView();
		SetStatus(aError.empty() ? "清单更新失败" : aError, true);
		return;
	}
	ApplyCatalogText(aText, "清单已更新");
}

void ScriptDownloadDialog::OnRowInfo(int aIndex) const
{
	if (aIndex < 0 || (size_t)aIndex >= mView.size())
		return;
	ScriptInfoDialog dialog(mView[(size_t)aIndex].entry);
	dialog.SetFrameOptions(true, true);
	dialog.Run();
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

void ScriptDownloadDialog::OnSource(Fl_Widget *, void *aData)
{
	auto *self = static_cast<ScriptDownloadDialog *>(aData);
	if (self->mBusy)
	{
		self->mSourceChoice->value(self->mSource);
		return;
	}
	self->mSource = self->mSourceChoice->value() == 1 ? 1 : 0;
	if (self->mCallbacks.saveSource)
		self->mCallbacks.saveSource(self->mSource);
	self->mPendingConfirm = -1;
	// Switching a source only swaps the URL column - no fetch, no reason to
	// touch the network.
	self->RebuildView();
}

void ScriptDownloadDialog::OnRefresh(Fl_Widget *, void *aData)
{
	static_cast<ScriptDownloadDialog *>(aData)->StartRefresh();
}

void ScriptDownloadDialog::OnClose(Fl_Widget *, void *aData)
{
	static_cast<ScriptDownloadDialog *>(aData)->RequestClose();
}

} // namespace ui
} // namespace ahkdeck

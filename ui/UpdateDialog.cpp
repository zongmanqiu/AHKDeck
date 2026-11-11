// ui/UpdateDialog.cpp - see ui/UpdateDialog.h
#include "ui/UpdateDialog.h"

#include "ui/DeckTitleBar.h"
#include "ui/SourceChoice.h"
#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

namespace ahkdeck
{
namespace ui
{
namespace
{

// The deck's rhythm (m087): a control is 26px, a row is 26px, a bar is 32px.
constexpr int kMargin = 16;
constexpr int kGap = 8;
constexpr int kRowH = 26;
constexpr int kStatusH = 40; // two wrapped lines
constexpr int kBarW = 120;   // fixed column: the status text always starts after it
constexpr int kBarH = 18;
constexpr int kNotesH = 200;
constexpr int kWidth = 560;

// The changelog is a long text with Chinese lines; the editor's display is
// reused with the deck's flat scrollbar instead of FLTK's stock bevel.
class NotesDisplay : public Fl_Text_Display
{
public:
	NotesDisplay(int x, int y, int w, int h)
		: Fl_Text_Display(x, y, w, h)
	{
		scrollbar_size(10);
		StyleTextDisplay(this);
		// StyleTextDisplay is tuned for a display inside a pane and sets a flat
		// box ("the pane frame draws the only border"). This one sits directly
		// on the dialog, so it has to draw its own frame - set after the style
		// call, not before.
		box(FL_BORDER_BOX);
		const ThemeColors &colors = DeckColors();
		Fl_Scrollbar *bars[] = { mVScrollBar, mHScrollBar };
		for (Fl_Scrollbar *bar : bars)
		{
			if (!bar)
				continue;
			bar->type(bar == mHScrollBar ? FL_HOR_SLIDER : FL_VERT_SLIDER);
			bar->box(FL_FLAT_BOX);
			bar->color(colors.background1);
			bar->selection_color(colors.background2);
		}
	}

	void draw() override
	{
		Fl_Text_Display::draw();
		fl_push_clip(x(), y(), w(), h());
		DeckDrawScrollbarCorner(mVScrollBar, mHScrollBar);
		DeckDrawScrollbar(mVScrollBar, false);
		DeckDrawScrollbar(mHScrollBar, true);
		fl_pop_clip();
	}
};

// One Fl::awake hop. `alive` is cleared when the window closes, so a worker
// that finishes afterwards drops its result instead of writing into a
// destroyed dialog.
template <typename T>
struct Job
{
	std::shared_ptr<bool> alive;
	UpdateDialog *self;
	T payload;
};

} // namespace

UpdateDialog::UpdateDialog(const std::string &aCurrentVersion,
	const std::string &aUrlGitee, const std::string &aUrlGithub, int aSource,
	std::function<void(int)> aSaveSource, std::function<bool()> aConfirmRestart)
	: DeckDialog(kWidth, DeckTitleBar::kHeight + 408, "更新"),
	  mCurrentVersion(aCurrentVersion),
	  mUrlGitee(aUrlGitee),
	  mUrlGithub(aUrlGithub),
	  mSaveSource(std::move(aSaveSource)),
	  mConfirmRestart(std::move(aConfirmRestart))
{
	const ThemeColors &colors = DeckColors();
	const int top = ContentTop();

	begin();

	int y = top + 14;

	// No news is just the text: the gauge exists only while something is
	// transferring, so an idle row is a full-width line. Fl_Progress draws
	// label(), not value(), so an empty label gives a clean gauge.
	mStatusY = y;
	mBar = new Fl_Progress(kMargin, y + (kStatusH - kBarH) / 2, kBarW, kBarH);
	mBar->box(FL_BORDER_BOX);
	mBar->minimum(0.0);
	mBar->maximum(1.0);
	mBar->value(0.0);
	mBar->selection_color(colors.highlight2);
	mBar->color(colors.background1);
	mBar->label("");
	mBar->hide();

	mStatus = new Fl_Box(kMargin, y, kWidth - 2 * kMargin, kStatusH);
	mStatus->box(FL_NO_BOX);
	mStatus->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
	mStatus->labelsize(TextSize());
	mStatus->labelcolor(DangerColour());
	y += kStatusH + 10;

	// --- source + re-check: the control column starts after its label ---
	const int labelW = 64;
	Fl_Box *sourceLabel = new Fl_Box(FL_NO_BOX, kMargin, y, labelW, kRowH, "更新源");
	sourceLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	sourceLabel->labelsize(TextSize());
	sourceLabel->labelcolor(colors.text1);

	// m147: the mirror list and its styling live in SourceChoice.h now, shared
	// with the script-download dialog.
	mSource = MakeSourceChoice(kMargin + labelW, y, 110, kRowH,
		aSource, OnSource, this);

	mBtnRefresh = new HoverButton(0, y, 0, kRowH, "联网检查");
	mBtnRefresh->color(colors.background2);
	mBtnRefresh->selection_color(colors.highlight1);
	mBtnRefresh->labelcolor(colors.text1);
	mBtnRefresh->labelsize(TextSize());
	mBtnRefresh->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
	mBtnRefresh->fit();
	mBtnRefresh->position(kMargin + labelW + 110 + kGap * 2, y);
	mBtnRefresh->callback(OnRefresh, this);
	y += kRowH + 8;

	// --- versions ---
	const int half = (kWidth - 2 * kMargin) / 2;
	mCurVer = new Fl_Box(FL_NO_BOX, kMargin, y, half, kRowH, nullptr);
	mCurVer->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	mCurVer->labelsize(TextSize());
	mCurVer->labelcolor(colors.text1);
	mMfVer = new Fl_Box(FL_NO_BOX, kMargin + half, y, half, kRowH, nullptr);
	mMfVer->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	mMfVer->labelsize(TextSize());
	mMfVer->labelcolor(colors.text1);
	y += kRowH + 8;

	// --- changelog ---
	Fl_Box *notesLabel = new Fl_Box(FL_NO_BOX, kMargin, y, kWidth - 2 * kMargin, 20,
		"更新说明");
	notesLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	notesLabel->labelsize(TextSize());
	notesLabel->labelcolor(colors.text1);
	y += 20 + 4;

	mNotesBuf = new Fl_Text_Buffer();
	mNotes = new NotesDisplay(kMargin, y, kWidth - 2 * kMargin, kNotesH);
	// Fl_Text_Display does not own the buffer; it is detached and deleted in
	// the destructor so its own teardown cannot reach a freed pointer.
	mNotes->buffer(mNotesBuf);
	mNotes->textsize(TextSize());
	mNotes->textcolor(colors.text1);
	mNotes->color(colors.background1);
	mNotes->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
	y += kNotesH + 8;

	// --- footer: [更新][关闭]; the dismissive button stays rightmost ---
	mBtnDownload = new HoverButton(0, 0, 0, kRowH, "更新");
	mBtnDownload->callback(OnDownload, this);
	mBtnClose = new HoverButton(0, 0, 0, kRowH, "关闭");
	mBtnClose->callback(OnClose, this);
	FitButtonRow({ mBtnDownload, mBtnClose }, kWidth, y + 22, kMargin, kGap);

	size(kWidth, y + 44);
	end();

	// Nothing is installable until a manifest says so. Availability is shown
	// by dimming the LABEL only: Fl_Widget::deactivate() repaints the box with
	// an opaque grey wash, and then the hover colour is wrong too.
	SetInstallable(false);
	SetStatus("尚未检查更新", false);
}

UpdateDialog::~UpdateDialog()
{
	// Any worker still running sees this and drops its payload.
	*mAlive = false;
	if (mNotesBuf)
	{
		if (mNotes)
			mNotes->buffer(nullptr);
		delete mNotesBuf;
		mNotesBuf = nullptr;
	}
}

void UpdateDialog::Run()
{
	ShowCentered();
	// Deliberately no network here: a user who just wants to read the notes
	// of the version they already installed gets them from the local cache,
	// and the last check's answer stays on disk until they ask again.
	ShowCachedState();
	while (shown())
		Fl::wait();
}

void UpdateDialog::OnClose(Fl_Widget *, void *aData)
{
	static_cast<UpdateDialog *>(aData)->RequestClose();
}

void UpdateDialog::RequestClose()
{
	*mAlive = false; // drop any in-flight worker result
	hide();
}

void UpdateDialog::SetStatus(const char *aText, bool aDanger)
{
	mStatusBuf = aText ? aText : "";
	mStatus->label(mStatusBuf.c_str());
	mStatus->labelcolor(aDanger ? DangerColour() : HintColour());
	mStatus->redraw();
}

void UpdateDialog::SetBusy(bool aBusy)
{
	mBusy = aBusy;
	const Fl_Color normal = NormalColour();
	const Fl_Color dim = HintColour();
	// Labels only: the click guards in StartCheck()/StartDownload()/OnSource()
	// are what actually block a second concurrent worker.
	mBtnRefresh->labelcolor(aBusy ? dim : normal);
	mSource->textcolor(aBusy ? dim : normal);
	// m148: SettingsChoice draws its arrow with labelcolor(), so dim that too
	// or the arrow stays dark while the text goes grey during a check.
	mSource->labelcolor(aBusy ? dim : normal);
	mBtnRefresh->redraw();
	mSource->redraw();
	if (aBusy)
		SetInstallable(false);
	else
		SetInstallable(mCanInstall);
}

void UpdateDialog::ShowNotes(const std::string &aText)
{
	if (!mNotesBuf)
		return;
	mNotesBuf->text(aText.empty() ? " " : aText.c_str());
	mNotes->scroll(0, 0);
	mNotes->redraw();
}

// Show/hide the gauge and give the text back its full width. The text starts
// at a fixed x in each mode, so a long message never moves anything.
void UpdateDialog::SetTransferMode(bool aOn)
{
	const int fullW = w() - 2 * kMargin;
	const int textX = kMargin + kBarW + kGap * 2;
	if (aOn)
	{
		mBar->show();
		mStatus->resize(textX, mStatusY, fullW - kBarW - kGap * 2, kStatusH);
	}
	else
	{
		mBar->hide();
		mBar->value(0.0);
		mStatus->resize(kMargin, mStatusY, fullW, kStatusH);
	}
	redraw();
}

void UpdateDialog::SetProgress(double aFraction, const std::string &aText)
{
	if (aFraction < 0)
		aFraction = 0.0;
	mBar->value(static_cast<float>(aFraction > 1.0 ? 1.0 : aFraction));
	mBar->redraw();
	SetStatus(aText.c_str(), false);
}

void UpdateDialog::FinishFailure(const std::string &aMessage)
{
	SetBusy(false);
	SetTransferMode(false); // nothing is transferring any more
	SetStatus(aMessage.c_str());
	// Re-evaluate instead of forcing the button dim: a failed download must
	// be retryable, and the manifest on screen still says "newer". After a
	// failed check the manifest is empty, so this resolves to not installable.
	ApplyInstallability(CompareVersions(mCurrentVersion, mManifest.version) > 0);
}

Fl_Color UpdateDialog::NormalColour() const
{
	return DeckColors().text1;
}

Fl_Color UpdateDialog::HintColour() const
{
	return DeckColors().text2;
}

Fl_Color UpdateDialog::DangerColour() const
{
	return DeckColors().highlight3;
}

// Availability is a LABEL colour, not a widget state (see the constructor).
void UpdateDialog::SetInstallable(bool aCan)
{
	mCanInstall = aCan;
	mBtnDownload->labelcolor(aCan ? NormalColour() : HintColour());
	mBtnDownload->redraw();
}

std::string UpdateDialog::DownloadUrl() const
{
	const bool github = mSource->value() == 1;
	const std::string &preferred = github ? mManifest.urlGithub : mManifest.urlGitee;
	if (!preferred.empty())
		return preferred;
	// A manifest may publish only one mirror. Do not dim the button (or abort
	// a download) merely because the picker sits on the empty side.
	return github ? mManifest.urlGitee : mManifest.urlGithub;
}

void UpdateDialog::RenderManifest(const UpdateManifest &aManifest)
{
	mCurVerBuf = "当前版本：" + mCurrentVersion;
	mCurVer->label(mCurVerBuf.c_str());
	mMfVerBuf = aManifest.version.empty() ? std::string("最新版本：未知")
		: "最新版本：" + aManifest.version;
	mMfVer->label(mMfVerBuf.c_str());
	mCurVer->redraw();
	mMfVer->redraw();
	ShowNotes(aManifest.notes);
}

// What the last successful check left on disk. A cached answer is never
// reported as "latest" - the status line says where it came from.
void UpdateDialog::ShowCachedState()
{
	UpdateManifest manifest;
	long long age = -1;
	if (!ReadCachedManifest(manifest, age))
	{
		mManifest = UpdateManifest();
		RenderManifest(mManifest);
		SetStatus("尚未检查更新", false);
		SetInstallable(false);
		return;
	}
	mManifest = manifest;
	RenderManifest(mManifest);
	SetStatus("上次检查结果（本地缓存）", false);
	ApplyInstallability(CompareVersions(mCurrentVersion, mManifest.version) > 0);
}

// Two things make an update installable: a version newer than ours, and a
// download URL for the source the user picked.
void UpdateDialog::ApplyInstallability(bool aNewer)
{
	if (!aNewer)
	{
		SetInstallable(false);
		return;
	}
	SetInstallable(!DownloadUrl().empty());
}

void UpdateDialog::OnRefresh(Fl_Widget *, void *aData)
{
	static_cast<UpdateDialog *>(aData)->StartCheck();
}

void UpdateDialog::StartCheck()
{
	if (mBusy)
		return;
	// Read the fields once on the UI thread: the worker must not touch widgets.
	const std::string gitee = mUrlGitee;
	const std::string github = mUrlGithub;
	const int source = mSource->value();
	const std::string current = mCurrentVersion;

	SetBusy(true);
	SetStatus("正在联网检查…", false);

	std::shared_ptr<bool> alive = mAlive;
	// The worker body has its own catch-all; this one covers std::thread /
	// detach failing to start at all (a callback must never throw into the
	// FLTK event loop).
	try
	{
		std::thread([this, alive, current, source, gitee, github]() {
			try
			{
				UpdateCheckResult result = CheckUpdate(current, gitee, github, source);
				auto *job = new Job<UpdateCheckResult>{ alive, this, result };
				Fl::awake([](void *aPayload) {
					auto *j = static_cast<Job<UpdateCheckResult> *>(aPayload);
					if (*j->alive)
						j->self->ApplyCheckResult(j->payload);
					delete j;
				}, job);
			}
			catch (...)
			{
				auto *job = new Job<std::string>{ alive, this,
					"联网检查失败：检查线程异常" };
				Fl::awake([](void *aPayload) {
					auto *j = static_cast<Job<std::string> *>(aPayload);
					if (*j->alive)
						j->self->FinishFailure(j->payload);
					delete j;
				}, job);
			}
		}).detach();
	}
	catch (...)
	{
		FinishFailure("无法启动检查线程");
	}
}

void UpdateDialog::ApplyCheckResult(const UpdateCheckResult &aResult)
{
	SetBusy(false);
	SetTransferMode(false); // a re-check ends any leftover transfer display
	mManifest = aResult.manifest;
	RenderManifest(mManifest);

	if (!aResult.online && !aResult.fromCache)
	{
		// Only a fresh fetch may claim "latest"; an unreachable source is
		// reported as exactly that, including the transport reason.
		SetStatus(aResult.error.empty() ? "联网检查失败"
			: ("联网检查失败：" + aResult.error).c_str());
		SetInstallable(false);
		return;
	}
	// Whether there IS a newer version is readable in the 最新版本 field and
	// in the update button; the line itself only says where the answer came
	// from.
	SetStatus(aResult.fromCache ? "上次检查结果（本地缓存）" : "检查完成", false);
	// A fresh manifest may have come from the FALLBACK mirror (CheckUpdate
	// tries the selected source first). Move the picker to the source that
	// actually answered; otherwise the download would use the URL that just
	// failed.
	if (aResult.online && !aResult.fromCache)
	{
		mSource->value(aResult.sourceUsed == 1 ? 1 : 0);
		mSource->redraw();
		if (mSaveSource)
			mSaveSource(mSource->value());
	}
	ApplyInstallability(aResult.hasUpdate);
}

void UpdateDialog::OnDownload(Fl_Widget *, void *aData)
{
	static_cast<UpdateDialog *>(aData)->StartDownload();
}

void UpdateDialog::StartDownload()
{
	if (mBusy || !mCanInstall)
		return;
	const std::string url = DownloadUrl();
	if (url.empty())
		return; // belt and braces

	const std::string exeDir = GetExeDirUtf8();
	if (exeDir.empty())
	{
		FinishFailure("无法确定程序目录");
		return;
	}
	std::string version = mManifest.version;
	if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
		version.erase(0, 1);
	if (version.empty())
		version = "latest";
	const std::string zipPath = exeDir + "AHKDeck_" + version + ".zip";
	const std::string batPath = exeDir + "ahkdeck_update.bat";

	SetBusy(true);
	SetTransferMode(true);
	SetProgress(0.0, "正在下载更新包…");

	std::shared_ptr<bool> alive = mAlive;
	auto work = [this, alive, url, zipPath, batPath, exeDir]() {
		auto fail = [this, alive](const std::string &aMessage) {
			auto *job = new Job<std::string>{ alive, this, aMessage };
			Fl::awake([](void *aPayload) {
				auto *j = static_cast<Job<std::string> *>(aPayload);
				if (*j->alive)
					j->self->FinishFailure(j->payload);
				delete j;
			}, job);
		};

		try
		{
			auto progress = [this, alive](long long received, long long total) {
				using ProgressJob = Job<std::pair<long long, long long>>;
				auto *job = new ProgressJob{ alive, this, { received, total } };
				Fl::awake([](void *aPayload) {
					auto *j = static_cast<ProgressJob *>(aPayload);
					if (*j->alive)
					{
						const long long got = j->payload.first;
						const long long all = j->payload.second;
						char text[96];
						if (all > 0)
							std::snprintf(text, sizeof(text), "正在下载更新包  %.0f%%",
								100.0 * (double)got / (double)all);
						else
							std::snprintf(text, sizeof(text), "正在下载更新包  %lld KB",
								got / 1024);
						j->self->SetProgress(all > 0
							? (double)got / (double)all : -1.0, text);
					}
					delete j;
				}, job);
			};

			if (!DownloadFile(url, zipPath, progress))
			{
				fail("下载失败（网络中断或安装目录不可写）");
				return;
			}
			if (!WriteUpdateBat(exeDir, zipPath, batPath))
			{
				fail("无法写入升级脚本（安装目录不可写）");
				return;
			}

			auto *job = new Job<std::string>{ alive, this, batPath };
			Fl::awake([](void *aPayload) {
				auto *j = static_cast<Job<std::string> *>(aPayload);
				UpdateDialog *self = j->self;
				const std::string bat = j->payload;
				const bool aliveNow = *j->alive;
				delete j;
				if (!aliveNow)
					return; // the user closed the window mid-download
				// Handing off kills this process, so never lose work silently.
				if (self->mConfirmRestart && !self->mConfirmRestart())
				{
					self->SetBusy(false);
					self->SetTransferMode(false);
					self->SetStatus("已取消更新", false);
					self->ApplyInstallability(true);
					return;
				}
				self->RequestClose(); // stop every further worker callback
				LaunchUpdater(bat);
				std::exit(0);
			}, job);
		}
		catch (...)
		{
			fail("下载线程异常");
		}
	};
	// Same guard as StartCheck: a failing std::thread/detach must not throw
	// into the FLTK callback that called us.
	try
	{
		std::thread(work).detach();
	}
	catch (...)
	{
		FinishFailure("无法启动下载线程");
	}
}

void UpdateDialog::OnSource(Fl_Widget *, void *aData)
{
	auto *self = static_cast<UpdateDialog *>(aData);
	if (self->mBusy)
		return;
	const int source = self->mSource->value() == 1 ? 1 : 0;
	if (self->mSaveSource)
		self->mSaveSource(source);
	// The source only decides WHICH URL a package would come from, so
	// switching it re-evaluates availability - it starts no check or download.
	if (self->mManifest.version.empty())
		return;
	self->ApplyInstallability(
		CompareVersions(self->mCurrentVersion, self->mManifest.version) > 0);
}

} // namespace ui
} // namespace ahkdeck

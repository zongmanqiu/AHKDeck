// ui/UpdateDialog.h - "帮助 > 检查更新" (m092), ported from pecia.
//
// One modal window holds the whole flow: current state, source picker,
// re-check, changelog, download progress and the update action.
//
// Threading: the check and the download run on detached worker threads and
// marshal back with Fl::awake. `m_alive` is cleared when the window closes,
// so a late callback drops its result instead of touching a destroyed dialog
// - or launching the updater script behind the user's back.
#pragma once

#include "ui/DeckDialog.h"

#include <functional>
#include <memory>
#include <string>

#include "core/Updater.h"

class Fl_Box;
class Fl_Choice;
class Fl_Progress;
class Fl_Text_Buffer;
class Fl_Text_Display;

namespace ahkdeck
{
namespace ui
{

class HoverButton;

class UpdateDialog : public DeckDialog
{
public:
	// aUrlGitee / aUrlGithub are already resolved by the caller (settings.json
	// or the built-in defaults). aSaveSource persists the picker (0/1).
	// aConfirmRestart runs before the process is handed to the updater script;
	// returning false cancels the update.
	UpdateDialog(const std::string &aCurrentVersion,
		const std::string &aUrlGitee, const std::string &aUrlGithub, int aSource,
		std::function<void(int)> aSaveSource,
		std::function<bool()> aConfirmRestart);
	~UpdateDialog() override;

	// Modal run loop (same shape as AboutDialog's): shows whatever the local
	// manifest holds; the network is only touched by 联网检查.
	void Run();

private:
	void StartCheck();
	void ApplyCheckResult(const UpdateCheckResult &aResult);
	void StartDownload();

	void SetStatus(const char *aText, bool aDanger = true);
	void RenderManifest(const UpdateManifest &aManifest);
	void ShowCachedState();
	void ApplyInstallability(bool aNewer);
	void SetInstallable(bool aCan);
	std::string DownloadUrl() const;
	Fl_Color NormalColour() const;
	Fl_Color DangerColour() const;
	Fl_Color HintColour() const;
	void SetBusy(bool aBusy);
	void SetProgress(double aFraction, const std::string &aText);
	void SetTransferMode(bool aOn);
	void ShowNotes(const std::string &aText);
	void FinishFailure(const std::string &aMessage);
	void RequestClose();

	static void OnRefresh(Fl_Widget *, void *);
	static void OnDownload(Fl_Widget *, void *);
	static void OnSource(Fl_Widget *, void *);
	static void OnClose(Fl_Widget *, void *);

	void OnCaptionClose() override { RequestClose(); }

	std::string mCurrentVersion;
	std::string mUrlGitee;
	std::string mUrlGithub;
	std::function<void(int)> mSaveSource;
	std::function<bool()> mConfirmRestart;
	UpdateManifest mManifest;   // what is currently displayed
	bool mBusy = false;         // a worker thread owns the UI
	bool mCanInstall = false;
	std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);

	Fl_Box *mStatus = nullptr;
	Fl_Box *mCurVer = nullptr;
	Fl_Box *mMfVer = nullptr;
	Fl_Choice *mSource = nullptr;
	Fl_Text_Display *mNotes = nullptr;
	Fl_Text_Buffer *mNotesBuf = nullptr;
	Fl_Progress *mBar = nullptr; // visible only mid-transfer
	int mStatusY = 0;            // status row's y, for the resize
	HoverButton *mBtnRefresh = nullptr;
	HoverButton *mBtnDownload = nullptr;
	HoverButton *mBtnClose = nullptr;

	// Fl_Widget::label() keeps the pointer, it does not copy - every label
	// built at run time needs a member that outlives the call.
	std::string mStatusBuf;
	std::string mCurVerBuf;
	std::string mMfVerBuf;
};

} // namespace ui
} // namespace ahkdeck

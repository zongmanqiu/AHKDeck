// ui/ScriptDownloadDialog.h - "文件 > 获取脚本…" (m146).
//
// A modal DeckDialog that lists the official scripts.json catalog. It is
// local-first: opening it reads the bundled copy, and only 刷新 touches the
// network. Every fetch/install runs on a detached worker and marshals back
// with Fl::awake, guarded by mAlive exactly like UpdateDialog.
//
// The dialog owns no host state: the four abilities it needs (save the source,
// rescan the script list, stop a running script, log a line) are injected by
// DeckWindow::ShowScriptDownloadDialog().
#pragma once

#include "ui/DeckDialog.h"
#include "core/ScriptCatalog.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Choice;
class Fl_Input;
class Fl_Widget;

namespace ahkdeck
{
namespace ui
{

class HoverButton;

struct ScriptDownloadCallbacks
{
	std::function<void(int)> saveSource;
	std::function<void()> refreshScripts;
	std::function<bool(const std::string &)> stopScript;
	std::function<void(const std::string &)> log;
};

enum class ActionTone
{
	Primary, // 下载 - blue
	Warning, // 升级 - amber
	Idle     // 已下载 / 无地址 - grey, not clickable
};

// One painted row. `caption` is already resolved by the dialog, so the row
// widget only draws - it never decides what an action means.
struct RowView
{
	CatalogEntry entry;
	ScriptInstallState state = ScriptInstallState::Absent;
	std::string caption;
	ActionTone tone = ActionTone::Idle;
	bool clickable = false;
};

class ScriptDownloadDialog : public DeckDialog
{
public:
	ScriptDownloadDialog(const std::string &aManifestGitee,
		const std::string &aManifestGithub, int aSource,
		ScriptDownloadCallbacks aCallbacks);
	~ScriptDownloadDialog() override;

	void Run();

private:
	void LoadBundled();
	void ApplyCatalogText(const std::string &aText, const std::string &aNote);
	void RebuildView();
	void SetStatus(const std::string &aText, bool aDanger = false);
	void OnRowAction(int aIndex);
	void OnRowInfo(int aIndex) const;
	void StartInstall(int aIndex);
	void ApplyInstallResult(const std::string &aName, bool aOk,
		const std::string &aError);
	void OnProgress(long long aReceived, long long aTotal);
	void StartRefresh();
	void ApplyRefresh(bool aOk, const std::string &aText,
		const std::string &aError);
	void RequestClose();

	static void OnSearch(Fl_Widget *, void *);
	static void OnSource(Fl_Widget *, void *);
	static void OnRefresh(Fl_Widget *, void *);
	static void OnClose(Fl_Widget *, void *);

	void OnCaptionClose() override { RequestClose(); }

	std::string mManifestGitee;
	std::string mManifestGithub;
	ScriptDownloadCallbacks mCallbacks;
	std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
	bool mBusy = false;

	std::vector<CatalogEntry> mCatalog;
	std::vector<RowView> mView;
	std::string mFilter;
	std::string mStatusBuf;
	int mSource = 0;
	int mPendingConfirm = -1;
	int mDownloadIndex = -1;
	std::string mDownloadName;
	std::string mDownloadCaption;

	Fl_Input *mSearch = nullptr;
	Fl_Box *mStatus = nullptr;
	Fl_Choice *mSourceChoice = nullptr;
	HoverButton *mBtnRefresh = nullptr;
	HoverButton *mBtnClose = nullptr;
	Fl_Widget *mRows = nullptr; // ScriptRows, defined in the .cpp
};

} // namespace ui
} // namespace ahkdeck

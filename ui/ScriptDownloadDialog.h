// ui/ScriptDownloadDialog.h - "文件 > 获取脚本…" (m146, multi-source m179).
//
// A modal DeckDialog that lists one or more script catalogs: the official
// scripts.json (through the mirror picked in 设置 > 选项… → 官方下载源) plus
// whatever extra manifests the user subscribed to. It is local-first: opening
// it reads the cached copies, and only 刷新 / 订阅 touch the network. Every
// fetch/install runs on a detached worker and marshals back with Fl::awake,
// guarded by mAlive exactly like UpdateDialog.
//
// m180 removed the URL box that used to live here: 订阅 is now a button that
// opens ui/SubscribeDialog, where the subscriptions are an actual list
// (add / remove / see the name each manifest gives itself).
//
// m179 also made it resizable and maximizable: a catalog from someone else can
// hold script names far longer than ours, and a fixed 560px window would clip
// them with no recourse.
//
// The dialog owns no host state: the abilities it needs (rescan the script
// list, stop a running script, persist the custom source list, log a line) are
// injected by DeckWindow::ShowScriptDownloadDialog().
#pragma once

#include "ui/DeckDialog.h"
#include "core/ScriptCatalog.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Input;
class Fl_Widget;

namespace ahkdeck
{
namespace ui
{

class HoverButton;

struct ScriptDownloadCallbacks
{
	std::function<void()> refreshScripts;
	std::function<bool(const std::string &)> stopScript;
	std::function<void(const std::string &)> log;
	// m180: the subscribed manifests (url + the name each one gave itself)
	// after 订阅管理 returns.
	std::function<void(const std::vector<CustomCatalogSource> &)> saveCustom;
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
	std::string sourceLabel; // 官方, or the custom source's host
	int sourceIndex = 0;     // which manifest this row came from
	ActionTone tone = ActionTone::Idle;
	bool clickable = false;
};

class ScriptDownloadDialog : public DeckDialog
{
public:
	// aOfficial is OfficialGitee / OfficialGithub; aManifestUrl is the URL that
	// mirror resolves to. aCustom are the user's subscribed manifests.
	ScriptDownloadDialog(ManifestSource aOfficial, const std::string &aManifestUrl,
		std::vector<CustomCatalogSource> aCustom, ScriptDownloadCallbacks aCallbacks);
	~ScriptDownloadDialog() override;

	void Run();

	// m180: remembered so the 订阅管理 window can be opened with the same
	// rounded-corner / shadow switches as this one.
	void SetFrameOptions(bool aRounded, bool aShadow)
	{
		mRounded = aRounded;
		mShadow = aShadow;
		DeckDialog::SetFrameOptions(aRounded, aShadow);
	}

private:
	void Build();
	// Positions everything from w()/h(): one function, called once at build
	// time and again on every resize (the window is resizable since m179).
	void Layout();
	void resize(int X, int Y, int W, int H) override;

	void LoadAll();
	void MergeText(int aSourceIndex, const std::string &aText, int *aDuplicates);
	ManifestSource KindFor(int aSourceIndex) const;
	void RebuildView();
	void SetStatus(const std::string &aText, bool aDanger = false);
	void OnRowAction(int aIndex);
	void OnRowInfo(int aIndex) const;
	void StartInstall(int aIndex);
	void ApplyInstallResult(const std::string &aName, bool aOk,
		const std::string &aError);
	void OnProgress(long long aReceived, long long aTotal);
	void StartRefresh();
	void ApplyRefresh(const std::vector<std::string> &aTexts,
		const std::vector<std::string> &aErrors);
	void OnManageSources();
	void RebuildSources();
	void RequestClose();

	static void OnSearch(Fl_Widget *, void *);
	static void OnRefresh(Fl_Widget *, void *);
	static void OnSources(Fl_Widget *, void *);
	static void OnClose(Fl_Widget *, void *);

	void OnCaptionClose() override { RequestClose(); }

	struct SourceSpec
	{
		std::string url;
		std::string label;
		bool official = false;
	};

	ManifestSource mOfficial = ManifestSource::OfficialGitee;
	std::string mOfficialUrl;
	std::vector<SourceSpec> mSources; // [0] is always the official catalog
	std::vector<CustomCatalogSource> mCustom; // what 订阅管理 returns, persisted
	ScriptDownloadCallbacks mCallbacks;
	std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
	bool mBusy = false;
	bool mRounded = true;
	bool mShadow = true;

	std::vector<RowView> mView;
	std::string mFilter;
	std::string mOfficialName = "AHKDeck";
	std::string mStatusBuf;
	int mPendingConfirm = -1;
	int mDownloadIndex = -1;
	std::string mDownloadName;
	std::string mDownloadCaption;

	Fl_Box *mSearchLabel = nullptr;
	Fl_Input *mSearch = nullptr;
	HoverButton *mBtnRefresh = nullptr;
	HoverButton *mBtnSources = nullptr;
	Fl_Box *mStatus = nullptr;
	HoverButton *mBtnClose = nullptr;
	Fl_Widget *mRows = nullptr; // ScriptRows, defined in the .cpp
};

} // namespace ui
} // namespace ahkdeck

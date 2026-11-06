#include "ui/DeckUI.h"
#include "ui/BlinkInput.h"
#include "ui/CaretBlink.h"
#include "ui/DeckDialog.h"
#include "ui/DeckFrame.h"
#include "ui/DeckTitleBar.h"
#include "ui/HotkeyCapture.h"
#include "ui/HoverMenuBar.h"
#include "ui/ImeInPlace.h"
#include "ui/WindowFrame.h"
#include "ui/WindowShadow.h"

#include "agent/McpServer.h"
#include "agent/Toolbox.h"
#include "core/Hotkeys.h"
#include "core/Json.h"
#include "core/Settings.h"
#include "core/TextFile.h"
#include "core/Version.h"
#include "engine/DeckEngine.h"
#include "ui/NativeHooks.h"
#include "ui/ThemeWidgets.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <winver.h>

#include <FL/x.H>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Light_Button.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
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

// ---------------------------------------------------------------------------
// encoding helpers (same rules as the engine: UTF-8 is the UI's wire format)
// ---------------------------------------------------------------------------

std::string Utf8(const std::wstring &aWide)
{
	if (aWide.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring Wide(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), out.data(), n);
	return out;
}

std::wstring LeafOf(const std::wstring &aPath)
{
	size_t p = aPath.find_last_of(L"\\/");
	return p == std::wstring::npos ? aPath : aPath.substr(p + 1);
}

bool SamePathI(const std::wstring &aLeft, const std::wstring &aRight)
{
	if (aLeft.size() != aRight.size())
		return false;
	for (size_t i = 0; i < aLeft.size(); ++i)
	{
		wchar_t l = aLeft[i] == L'/' ? L'\\' : aLeft[i];
		wchar_t r = aRight[i] == L'/' ? L'\\' : aRight[i];
		if (towlower(l) != towlower(r))
			return false;
	}
	return true;
}

std::wstring ExeDirectory()
{
	wchar_t buf[MAX_PATH] = {0};
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring path(buf, n > 0 ? n : 0);
	size_t slash = path.find_last_of(L'\\');
	if (slash != std::wstring::npos)
		path.resize(slash);
	return path;
}

// m050: one JSON file for every setting. AHKDECK_CONFIG / AHKDECK_SETTINGS are
// overrides (the gates use the latter); otherwise use the user's local app dir.
std::wstring DefaultConfigPath()
{
	wchar_t explicitPath[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"AHKDECK_CONFIG", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;
	n = GetEnvironmentVariableW(L"AHKDECK_SETTINGS", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;
	return ExeDirectory() + L"\\settings.json";
}

// Portable mode: legacy files are looked up next to the exe (or via the test
// override), never in %LOCALAPPDATA%.
std::wstring LegacyUiConfigPath()
{
	wchar_t explicitPath[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"AHKDECK_SETTINGS", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;
	return ExeDirectory() + L"\\ui.ini";
}

// m053: a path ending in .ini is the old geometry format (and the gates'
// override), not a malformed settings.json. It may be migrated to JSON on
// save; a malformed .json must stay untouched.
bool IsLegacyIniPath(const std::wstring &aPath)
{
	return aPath.size() >= 4 && _wcsicmp(aPath.c_str() + aPath.size() - 4, L".ini") == 0;
}


std::wstring AppHotkeyPath()
{
	wchar_t explicitPath[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"AHKDECK_HOTKEYS", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;
	return ExeDirectory() + L"\\hotkeys.txt";
}

std::string EnvValue(const std::string &aName)
{
	if (aName.empty())
		return std::string();
	DWORD needed = GetEnvironmentVariableA(aName.c_str(), nullptr, 0);
	if (needed == 0)
		return std::string();
	std::string value(needed, '\0');
	const DWORD written = GetEnvironmentVariableA(aName.c_str(), value.data(), needed);
	value.resize(written);
	return value;
}

bool ReadJsonConfig(const std::wstring &aPath, Json &aOut, std::string &aError)
{
	if (GetFileAttributesW(aPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		aError = "settings.json not found";
		return false;
	}
	TextDocument document;
	if (!ReadTextDocument(aPath, document, aError))
		return false;
	Json parsed;
	if (!Json::Parse(document.utf8, parsed, aError))
		return false;
	if (!parsed.IsObject())
	{
		aError = "settings.json root is not an object";
		return false;
	}
	aOut = std::move(parsed);
	return true;
}

bool WriteJsonConfig(const std::wstring &aPath, const Json &aRoot, std::string &aError)
{
	const size_t slash = aPath.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
	{
		const std::wstring directory = aPath.substr(0, slash);
		CreateDirectoryW(directory.c_str(), nullptr);
	}
	return WriteTextDocument(aPath, aRoot.DumpPretty(), TextEncoding::Utf8NoBom, true,
		nullptr, aError);
}

Json JsonObjectMember(const Json &aObject, const char *aKey)
{
	const Json &value = aObject[aKey];
	return value.IsObject() ? value : Json::MakeObject();
}


// One row height for every content row: list rows, column captions, search,
// editor/console/AI headers. The list's 16px font + 5px linespacing is 26.
constexpr int kRowHeight = 26;

// m046: the log takes a quarter of the space below the editor toolbar, and the
// divider is a visible 3px line rather than the old 1px hairline.
constexpr int kConsoleButtonH = 24;
constexpr int kConsoleButtonGap = 10;
constexpr int kConsoleButtonPadX = 12;
constexpr int kConsoleButtonMinW = 48;
constexpr int kPaneBarH = 28; // editor/AI middle band that holds the actions
// m052: Pecia's AI defaults (endpoint is the full completions URL).
constexpr int kConsoleOutputPercent = 25;
constexpr int kConsoleOutputMinH = 60;
constexpr int kEditorMinH = 80;

// Right-align a row of console actions inside the bar (Pecia fitButtonRow):
// size each button to its label, then place right-to-left. The caller picks the
// font/padding so four actions still fit a narrow editor column.
void FitConsoleButtonRow(std::initializer_list<Fl_Button *> aButtons,
	int aRightEdge, int aCenterY, int aMargin, int aGap,
	int aPadX = kConsoleButtonPadX, int aMinW = kConsoleButtonMinW,
	int aFontSize = 16)
{
	for (Fl_Button *button : aButtons)
	{
		if (!button)
			continue;
		button->labelsize(aFontSize);
		int textW = 0;
		int textH = 0;
		button->measure_label(textW, textH);
		int width = textW + 2 * aPadX;
		if (width < aMinW)
			width = aMinW;
		// Pin the height: Fl_Group proportionally rescales children while the
		// window grows from its constructor size, and carrying that scaled
		// height into the bar made the button touch (and lose) its top/bottom
		// border. The bar owns the rhythm, not the scale factor.
		button->resize(button->x(), aCenterY - kConsoleButtonH / 2,
			width, kConsoleButtonH);
	}
	int x = aRightEdge - aMargin;
	Fl_Button *const *vec = aButtons.begin();
	for (size_t i = aButtons.size(); i-- > 0;)
	{
		Fl_Button *button = vec[i];
		if (!button)
			continue;
		button->position(x - button->w(), aCenterY - kConsoleButtonH / 2);
		x -= button->w() + aGap;
	}
}

// "2026-11-03T21:19:05.571" -> "21:19:05.571"; anything shorter is passed on.
std::string ShortTime(const std::string &aTimestamp)
{
	if (aTimestamp.size() >= 23 && aTimestamp[10] == 'T')
		return aTimestamp.substr(11, 12);
	return aTimestamp;
}

std::string UiClock()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

// The status bar names the kernel the deck is about to load, the way v1.3's
// "内核：v2.0.25 · 64 位" did. Version only: the build is x64-only by CMake fiat (a
// non-x64 configure stops with FATAL_ERROR), so a bitness column could repeat
// itself forever.
std::string KernelVersionOf(const std::wstring &aPath)
{
	const DWORD size = GetFileVersionInfoSizeW(aPath.c_str(), nullptr);
	if (!size)
		return std::string();
	std::vector<char> blob(size);
	if (!GetFileVersionInfoW(aPath.c_str(), 0, size, blob.data()))
		return std::string();
	VS_FIXEDFILEINFO *info = nullptr;
	UINT infoSize = 0;
	if (!VerQueryValueW(blob.data(), L"\\", reinterpret_cast<void **>(&info), &infoSize) || !info)
		return std::string();
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u",
		HIWORD(info->dwProductVersionMS), LOWORD(info->dwProductVersionMS),
		HIWORD(info->dwProductVersionLS));
	return buf;
}

// The official exit code for a session. A failing script's thread unwinds with
// 0, while the official dialog's "Exit" gives 2 - Engine exposes the raw thread
// code plus the fatal flag, and this is the one place the UI applies the same
// rule the CLI does (m010 §3.2), so the list and the console always agree.
DWORD OfficialRc(const Session &aSession)
{
	return (aSession.rc == 0 && aSession.fatal) ? 2 : aSession.rc;
}


// ---------------------------------------------------------------------------

class EditorBox;
class ScriptBrowser;

// Filter box with Pecia's left magnifier (m031). The custom boxtype insets the
// text by 24px, so the glyph is drawn in the empty gutter instead of over the
// first typed character.
class SearchInput : public BlinkInput
{
public:
	static Fl_Boxtype BoxType()
	{
		static const Fl_Boxtype type = []() {
			Fl_Boxtype t = (Fl_Boxtype)FL_FREE_BOXTYPE;
			Fl::set_boxtype(t, Fl::get_boxtype(FL_BORDER_BOX), 24, 1, 26, 2, nullptr);
			return t;
		}();
		return type;
	}

	// In the list pane the search sits inside the pane frame, so it must not
	// draw a second border of its own (Pecia's dialog search keeps its border).
	static Fl_Boxtype FlatBoxType()
	{
		static const Fl_Boxtype type = []() {
			Fl_Boxtype t = (Fl_Boxtype)(FL_FREE_BOXTYPE + 1);
			Fl::set_boxtype(t, Fl::get_boxtype(FL_FLAT_BOX), 24, 1, 26, 2, nullptr);
			return t;
		}();
		return type;
	}

	explicit SearchInput(int x, int y, int w, int h, bool aFlat = false,
		bool aBottomRule = false)
		: BlinkInput(x, y, w, h)
		, mFlat(aFlat)
		, mBottomRule(aBottomRule)
	{
		box(aFlat ? FlatBoxType() : BoxType());
	}

	void applyBox() { box(mFlat ? FlatBoxType() : BoxType()); }

	void draw() override
	{
		BlinkInput::draw();
		fl_color(DeckColors().text2);
		fl_line_style(FL_SOLID, 2);
		fl_arc(x() + 6, y() + 5, 11, 11, 0, 360);
		fl_line(x() + 15, y() + 14, x() + 20, y() + 19);
		fl_line_style(FL_SOLID, 1);
		// m049: the pane's bottom frame is part of the input's own paint, so a
		// focus redraw cannot erase it.
		if (mBottomRule)
		{
			fl_color(ThemeBorderLine());
			fl_rectf(x(), y() + h() - 1, w(), 1);
		}
	}

private:
	bool mFlat = false;
	bool mBottomRule = false;
};

// The whole script pane is one frame: header, rows and search are children
// inset by one pixel, so width changes move all three together.
static Fl_Boxtype PaneFrameBox()
{
	static const Fl_Boxtype type = []() {
		Fl_Boxtype t = (Fl_Boxtype)(FL_FREE_BOXTYPE + 2);
		Fl::set_boxtype(t, [](int x, int y, int w, int h, Fl_Color) {
			// The pane interior is the list colour: the header band, browser,
			// search and the small top gap must never expose the window grey.
			fl_color(DeckColors().background1);
			fl_rectf(x, y, w, h);
			fl_color(ThemeBorderLine());
			fl_rect(x, y, w, h);
		}, 1, 1, 1, 1);
		return t;
	}();
	return type;
}

// The pane fill is drawn by the frame box behind the children; this overlay
// is created after every child so its 1px border cannot be covered by the
// browser/search/editor/console backgrounds at the right and bottom edges.
static Fl_Boxtype BorderOnlyBox()
{
	static const Fl_Boxtype type = []() {
		Fl_Boxtype t = (Fl_Boxtype)(FL_FREE_BOXTYPE + 3);
		Fl::set_boxtype(t, [](int x, int y, int w, int h, Fl_Color) {
			fl_color(ThemeBorderLine());
			fl_rect(x, y, w, h);
		}, 1, 1, 1, 1);
		return t;
	}();
	return type;
}

// m037: headers are plain labels (no click sorting; the store stays in name
// order). The menu column is a gear between 状态 and 脚本.
static void DrawGear(int aCx, int aCy, int aRadius, Fl_Color aColor)
{
	fl_color(aColor);
	fl_line_style(FL_SOLID, 2);
	fl_arc(aCx - aRadius, aCy - aRadius, aRadius * 2, aRadius * 2, 0, 360);
	for (int i = 0; i < 8; ++i)
	{
		const double angle = i * 3.14159265358979 / 4.0;
		const int x1 = aCx + (int)std::lround(std::cos(angle) * aRadius);
		const int y1 = aCy + (int)std::lround(std::sin(angle) * aRadius);
		const int x2 = aCx + (int)std::lround(std::cos(angle) * (aRadius + 3));
		const int y2 = aCy + (int)std::lround(std::sin(angle) * (aRadius + 3));
		fl_line(x1, y1, x2, y2);
	}
	fl_line_style(FL_SOLID, 1);
	fl_arc(aCx - 2, aCy - 2, 4, 4, 0, 360);
}

// m043: the editing indicator. A diagonal pen body with a nib, drawn inside
// the same 10px box the play/pause glyph uses.
static Fl_Color AlertRed()
{
	return fl_rgb_color(0xC6, 0x28, 0x28);
}


class GearHeader : public Fl_Box
{
public:
	GearHeader(int x, int y, int w, int h)
		: Fl_Box(x, y, w, h)
	{
		box(FL_FLAT_BOX);
		color(DeckColors().background3);
	}

	void draw() override
	{
		Fl_Box::draw();
		DrawGear(x() + w() / 2, y() + h() / 2, 5, DeckColors().text1);
	}
};

class ShortcutDialogWindow;

// Fl_Box vertically centres labels through fl_draw's align flags, but the
// 16px font inside a 20px box still sits high in practice. Draw it against the
// measured font height so the baseline is truly centred.
class CenteredLabelBox : public Fl_Box
{
public:
	using Fl_Box::Fl_Box;

	void draw() override
	{
		draw_box();
		const char *text = label();
		if (!text || !*text)
			return;
		fl_font(labelfont(), labelsize());
		fl_color(labelcolor());
		// Measure the actual glyph box and centre that rectangle in the row.
		// A baseline computed from fl_height() left the text visibly low here.
		int textW = 0;
		int textH = 0;
		fl_measure(text, textW, textH, 0);
		// CJK glyphs in YaHei sit visually lower than the Latin font box that
		// fl_measure reports; lift by 3px so the ink itself is centred.
		const int top = y() + (h() - textH) / 2 - 3;
		fl_draw(text, x() + 8, top, textW, textH, FL_ALIGN_LEFT);
	}
};

class DeckWindow : public Fl_Double_Window
{
public:
	explicit DeckWindow(Engine &aEngine);
	~DeckWindow() override;

	void resize(int x, int y, int w, int h) override;
	int handle(int aEvent) override;
	void draw() override;

	// The client area is what the layout speaks; with the Pecia frame the
	// native window is 1px wider and taller than it (the 1px border lives in
	// the non-client strip). Ask Win32 rather than guessing what FLTK did with
	// the outer size it was given.
	int ClientWidth() const;
	int ClientHeight() const;

	DeckTitleBar *TitleBarWidget() const { return mTitleBar; }
	// The custom caption buttons route here: close keeps v1.3's hide-to-tray
	// meaning, never DestroyWindow.
	void TitleClose();
	// Called once after show(): installs the Pecia frame + shadow.
	void InstallFrame();

	// Every method below names a script by its index in mScripts, not by the line
	// the user sees: filtering and sorting make those two differ (m024). Use
	// IndexOfLine()/SelectedIndex() at the widget boundary and LineOfIndex() to
	// drive the browser itself.
	void ShowContextMenu(int aIndex);
	void EditRow(int aIndex);
	// The per-row 菜单 cell calls this, so it is not a private detail.
	void OpenScriptMenu(int aIndex); // ask the script to pop its own tray menu
	// Called by the browser when its inline hotkey cell records a combo.
	void CommitInlineHotkey(int aIndex, const std::string &aText);

	// Settings > 快捷键: write the dialog's whole table in one go (OK).
	bool ApplyShortcutRows(const std::vector<std::pair<std::string, std::string>> &aRows);

	// m033: Settings > Shortcuts dialog. It lists every script and records a
	// combo in a cell, the way Pecia's shortcut settings page does.
	int ScriptCount() const { return (int)mScripts.size(); }
	std::string ScriptNameAt(int aIndex) const;
	std::string ScriptBindingAt(int aIndex) const;
	void ShowShortcutDialog();

	// Translation between what the browser shows and what the deck stores. The
	// widgets speak lines (1-based, only what passes the filter); the deck speaks
	// mScripts indices.
	int IndexOfLine(int aLine) const;  // browser line -> mScripts index, or -1
	int LineOfIndex(int aIndex) const; // mScripts index -> browser line, or 0 when hidden

	// m026: what the browser needs in order to draw and hit-test its own cells.
	// Idle is "never started in this deck"; Finished is "the last run ended with
	// rc 0 and no fatal error" - a one-shot script is gone in ~50 ms, so without
	// a lasting mark the row looks exactly like it never ran (m068). Failed is a
	// non-zero exit or a fatal record; Running is the live state. A lasting state
	// never borrows the error colour.
	// The first column is a BUTTON: it says what clicking will do, never what
	// happened last time (m077). The last-run result lives on the row's left
	// edge, see LastRunFailed.
	enum class RowLight { Idle, Running };
	bool LastRunFailed(int aIndex) const;
	RowLight LightOf(int aIndex) const;
	void MenuCellRect(int aX, int aY, int aH, int &aRX, int &aRY, int &aRW, int &aRH) const
	{
		aRX = aX + mColumns[0];
		aRW = mColumns[1];
		aRY = aY + (aH - 24) / 2;
		aRH = 24;
	}
	// m031: the hotkey cell is a button too (click = edit the binding). The
	// row's first cell is the play/pause action glyph; that rect is just
	// 0..mColumns[0] and needs no helper.
	void HotkeyCellRect(int aX, int aY, int aH, int &aRX, int &aRY, int &aRW, int &aRH) const
	{
		aRX = aX + mColumns[0] + mColumns[1] + mColumns[2];
		aRW = mColumns[3] - 6;
		aRY = aY + (aH - 24) / 2;
		aRH = 24;
	}
	void NameCellRect(int aX, int &aRX, int &aRW) const
	{
		aRX = aX + mColumns[0] + mColumns[1];
		aRW = mColumns[2];
	}
	int DotColumnWidth() const { return mColumns[0]; }
	bool IsEditingLine(int aLine) const
	{
		const int index = IndexOfLine(aLine);
		return index >= 0 && (size_t)index < mScripts.size() &&
			!mEditPath.empty() && SamePathI(mScripts[(size_t)index].path, mEditPath);
	}
	// m047: the list appends " *" while the open script has unsaved edits.
	bool IsEditingLineDirty(int aLine) const
	{
		return IsEditingLine(aLine) && mEditDirty;
	}

	// Called by EditorBox for Ctrl+S / Ctrl+R: keeping the shortcut on the
	// widget itself is the only path that is guaranteed to run before the text
	// editor consumes the key (Fl::add_handler did not see it on this build).
	void SaveFromShortcut() { SaveEditor(); }
	void ReloadFromShortcut() { ReloadEditor(); }
	void FocusFind();
	void FocusReplace();
	void FindNextFromShortcut() { FindNext(); }
	// Double-click on a row is a start/stop switch, exactly like v1.3's ListView
	// double-click and like the bound hotkey: one gesture, one meaning (m023).
	void RunOrStopRow(int aIndex) { ToggleRow(aIndex); }

	// m064: the MCP endpoint. It runs on its own thread inside this process, so
	// the client that spawned us (WorkBuddy / Qoder, both configured through
	// {"mcpServers":{"<name>":{"command","args","env"}}} - stdio, no url) drives
	// the very window the user is looking at: same editing area, same sessions,
	// same "宿主关闭 = 脚本全关". Only "AHKDeck.exe --mcp-stdio" starts it.
	void StartMcp();
	void StopMcp();
	bool McpStarted() const { return mMcpStarted; }

	// The event loop keeps turning while the deck is only hidden to the tray, so
	// quitting needs an explicit request rather than "no windows left".
	bool ShouldQuit() const { return mQuitRequested; }

	// Geometry + list view preferences. One writer, one file (core/Settings).
	bool SaveSettings(std::string *aError = nullptr);

private:
	enum MenuId
	{
		IdNew = 1,
		IdOpen,
		IdSave,
		IdSaveAs,
		IdSaveRun,
		IdReloadEditor,
		IdLogs,
		IdQuit,
		IdFind,
		IdReplace,
		IdClearConsole,
		IdCopyConsole,
		IdViewList,
		IdViewEditor,
		IdToggleRounded,
		IdToggleShadow,
		IdRun,
		IdStop,
		IdRestart,
		IdScriptMenu,
		IdStopAll,
		IdRescan,
		IdRemove,
		IdRename,
		IdHotkeyEdit,
		IdNotepad,
		IdHideTray,
		IdRunAll,
		IdOpenScriptsFolder,
		IdOpenConfigFile,
		IdEditSelected,
		IdAbout
	};

	void HandleMenu(int aId);
	void ApplyFrameOptions();

	struct ScriptRow
	{
		std::wstring path;
		std::string name;   // UTF-8 leaf, the only part the list shows
		DWORD session = 0;  // latest session started for this script
	};

	void Layout();
	void Append(const std::string &aLine);
	void AppendIfTest(const std::wstring &aPath, const std::string &aLine);
	void SyncConsoleSelection();
	std::wstring ActiveScriptPath() const;
	void OpenSessionLog(int aIndex);
	void AppendRecord(const ErrorRecord &aRecord);
	void ScanScripts();
	bool AddScript(const std::wstring &aPath);
	int FindScript(const std::wstring &aPath) const;
	void RefreshList();
	void DrainRecords();
	void TrackSessions();

	// Starting a script never touches the console: the pane belongs to the file
	// open in the editor (m076). Other scripts' output stays in their JSONL.
	DWORD StartPath(const std::wstring &aPath);
	void RunSelected();
	void StopSelected();
	void StopEverything();

	void OpenLogs();
	void EditSelectedExternally();
	void RefreshFromDisk();

	// --- m023: the v1.3 workbench actions -----------------------------------
	void ToggleRow(int aIndex); // run if stopped, stop if running
	void RestartRow(int aIndex);     // stop, then start once it really finished
	void BeginHotkeyEdit(int aIndex);
	void RebindHotkeys();
	void LoadHotkeyFile();
	void HandleIntent(const NativeHooks::Intent &aIntent);
	void HideToTray();
	void RequestQuit();
	std::string BindingOf(const ScriptRow &aRow) const;
	void UpdateTrayTip();
	void UpdateKernelSummary();

	// --- m024/m026: the list view (filter + the columns it shows) --------------
	void RebuildView();
	bool MatchesFilter(size_t aIndex) const;
	bool IsRunning(const ScriptRow &aRow) const;

	bool LoadIntoEditor(const std::wstring &aPath);
	bool SaveEditor();

	bool SaveEditorTo(const std::wstring &aPath);
	void SaveAndRun();
	void NewScript();
	void RenameSelected();
	void PromptSaveNewScript(bool aRun);
	void ShowNameDialog(const char *aTitle, const std::string &aInitial,
		std::function<std::string(const std::string &)> aValidate,
		std::function<void(const std::string &)> aAccept);
	std::string ScriptTargetPath(const std::string &aLeaf) const;
	std::string ValidateScriptLeaf(const std::string &aLeaf, const std::wstring &aExcept) const;
	void CloseScript();
	void RunEverything();
	void OpenScriptsFolder();
	void EditSelected();
	bool EditingScriptRunning(DWORD *aSession = nullptr) const;
	void RunOrStopEditing();
	void SyncEditorRunButton();
	void ReloadEditor();
	void SyncEditorActions();
	void FindNext();
	void ReplaceOne();
	void ReplaceAll();
	void OnScriptWritten(const std::string &aUtf8Path);
	void ApplyToolEditorContent(const std::string &aText);
	void ApplyWidgetTheme();
	void ApplyView();
	void SyncViewMenu();
	void UpdateLineNumberWidth();
	void EnsureEditorVisible();
	void LayoutEditorPane(int x, int y, int w, int h);
	void LayoutListPane(int x, int y, int w, int h);

	int SelectedIndex() const;
	std::string NameOfSession(DWORD aSessionId) const;
	void UpdateStatus();

	static void OnRun(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->RunSelected(); }
	static void OnStop(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->StopSelected(); }
	static void OnRestart(Fl_Widget *, void *aSelf)
	{
		static_cast<DeckWindow *>(aSelf)->RestartRow(static_cast<DeckWindow *>(aSelf)->SelectedIndex());
	}
	static void OnStopAll(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->StopEverything(); }

	static void OnReload(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->RefreshFromDisk(); }
	static void OnNotepad(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->EditSelectedExternally(); }

	static void OnLogs(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->OpenLogs(); }
	static void OnClear(Fl_Widget *, void *aSelf);
	static void OnCopy(Fl_Widget *, void *aSelf);
	static void OnSelect(Fl_Widget *, void *aSelf);
	static void OnSave(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->SaveEditor(); }

	static void OnSaveRun(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->SaveAndRun(); }
	static void OnRunOrStop(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->RunOrStopEditing(); }
	static void OnCloseScript(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->CloseScript(); }
	static void OnEditorReload(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->ReloadEditor(); }
	static void OnFindNext(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->FindNext(); }
	static void OnReplaceOne(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->ReplaceOne(); }
	static void OnReplaceAll(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->ReplaceAll(); }
	static void OnMenu(Fl_Widget *aWidget, void *aData);
	static void OnTick(void *aSelf);
	static void OnBufferModified(int aPos, int aInserted, int aDeleted, int aRestyled,
		const char *aText, void *aSelf);

	Engine &mEngine;
	bool mReady = false;

	Fl_Button *mRun = nullptr;
	Fl_Button *mStop = nullptr;
	Fl_Button *mRestart = nullptr;
	Fl_Button *mStopAll = nullptr;
	Fl_Button *mOpen = nullptr;
	Fl_Button *mReload = nullptr;
	Fl_Button *mRemove = nullptr;
	Fl_Button *mNotepad = nullptr;
	Fl_Button *mLogs = nullptr;
	Fl_Button *mClear = nullptr;
	Fl_Button *mCopy = nullptr;
	Fl_Button *mHotkey = nullptr;

	std::wstring mPendingRestart; // path to start when its session has finished

	Fl_Browser *mList = nullptr;
	ScriptBrowser *mBrowser = nullptr; // same object, typed for the inline hotkey record
	ShortcutDialogWindow *mShortcutDialog = nullptr;
	// m024/m026: one header row above the list - the filter, and the column
	// captions that have to line up with the cells inside the browser.
	Fl_Input *mFilter = nullptr;
	// One outer frame around the whole pane (header, rows, search); the
	// children are inset one pixel so a width change moves them together.
	Fl_Box *mListFrame = nullptr;
	Fl_Box *mEditorFrame = nullptr;
	Fl_Box *mListBorder = nullptr;
	Fl_Box *mEditorBorder = nullptr;
	// One continuous band behind the captions: header, list and search then
	// share the same left/right edges instead of four floating captions.
	Fl_Box *mHeaderBar = nullptr;
	Fl_Box *mFilterRule = nullptr;
	Fl_Box *mCaptionStatus = nullptr;
	Fl_Box *mCaptionMenu = nullptr;
	Fl_Box *mCaptionName = nullptr;
	Fl_Box *mCaptionHotkey = nullptr;

	std::vector<Fl_Widget *> mListWidgets;
	// Visible browser line (1-based) -> index in mScripts.
	std::vector<int> mVisible;
	std::vector<int> mLastVisible; // to notice a change the label text cannot show
	// The list is a table: dot | name | hotkey | menu. This one array drives both
	// the cells (Fl_Browser::column_widths reads it live) and the captions above.
	// m032: 状态列要放得下“状态”两个字，也给播放/暂停一个不挤的命中区。
	int mColumns[5] = { 36, 36, 300, 136, 0 };
	Fl_Text_Display *mConsole = nullptr;
	Fl_Text_Buffer *mBuffer = nullptr;
	// One console, one test script: set when a run starts; the session JSONLs
	// are the permanent per-script record.
	std::wstring mConsolePath;
	// Last editor file we looked at. Ownership of the console only follows the
	// editor when that file *changes*, so a manual run can keep it (m067).
	std::wstring mConsoleEditorPath;
	std::map<DWORD, std::wstring> mSessionPath;
	Fl_Box *mStatus = nullptr;

	Fl_Text_Editor *mEditor = nullptr;
	Fl_Text_Buffer *mEditBuffer = nullptr;
	Fl_Button *mSave = nullptr;
	Fl_Button *mSaveAs = nullptr;
	Fl_Button *mSaveRun = nullptr;
	Fl_Button *mEditorReload = nullptr;
	Fl_Box *mEditorBar = nullptr;
	HoverButton *mConsoleSave = nullptr;
	HoverButton *mConsoleClose = nullptr;
	HoverButton *mConsoleRun = nullptr;
	HoverButton *mConsoleClear = nullptr;

	Fl_Input *mFindInput = nullptr;
	Fl_Input *mReplaceInput = nullptr;
	Fl_Button *mFindNext = nullptr;
	Fl_Button *mReplaceOne = nullptr;
	Fl_Button *mReplaceAll = nullptr;
	Fl_Light_Button *mMatchCase = nullptr;


	// m063/m064: the MCP side gets its own Toolbox instance on purpose - each
	// client keeps its own script-id table, while the tool list and every tool
	// implementation live once, in agent/Toolbox.cpp.
	agent::Toolbox mMcpTools;
	std::thread mMcpThread;
	std::atomic<bool> mMcpRunning{false};
	bool mMcpStarted = false;

	// FLTK text buffers may only be read on the UI thread, so tools never touch
	// the editor: the tick copies the editing area into this snapshot and the
	// tool thread reads the copy.
	std::mutex mSnapshotMutex;
	std::string mEditorSnapshot;
	long mSnapshotChars = -1;
	std::string mSnapshotKey;
	std::string EditorSnapshot();
	void RefreshEditorSnapshot();

	// What a tool asked the window to do, applied on the UI thread.
	struct ToolJob
	{
		enum class Kind
		{
			Editor, // text = new editing-area content, path = the tool's note
			Note,   // text = a console line
			Write,  // path = a script that changed on disk
		};
		Kind kind = Kind::Note;
		std::string text;
		std::string path;
	};
	std::mutex mToolJobMutex;
	std::deque<ToolJob> mToolJobs;
	void DrainToolJobs();

	HoverMenuBar *mMenu = nullptr;
	DeckTitleBar *mTitleBar = nullptr;
	bool mFrameInstalled = false;
	bool mRoundedCorners = true;
	bool mWindowShadow = true;
	bool mShowList = true;
	bool mShowEditor = true;
	bool mViewFromEnv = false;
	bool mConfigMissing = false;
	bool mConfigWritable = false;
	bool mConfigDirty = false;
	std::vector<Fl_Widget *> mEditorWidgets;

	std::vector<ScriptRow> mScripts;
	std::vector<std::string> mLastLabels; // rebuilt only when something changed
	// m027: one Sessions() snapshot per tick. Sessions() copies every session,
	// reaps each thread handle and takes the engine lock; the old tick called it
	// three times (track + tray tip + status) plus once per row in RefreshList.
	mutable std::vector<Session> mSessionsCache;
	mutable bool mSessionsCacheValid = false;
	const std::vector<Session> &CachedSessions() const;
	void InvalidateSessions() const { mSessionsCacheValid = false; }
	std::string mLastStatus; // avoid a copy_label/redraw when nothing changed
	bool mUpdatingList = false;
	std::map<DWORD, SessionState> mSeenState;
	std::map<DWORD, std::string> mSessionName;
	std::map<DWORD, bool> mExitReported;

	std::wstring mEditPath;
	std::wstring mSettingsPath;
	Json mConfigRoot; // full settings.json, unknown fields preserved
	bool mEditDirty = false;
	bool mEditLoading = false;
	bool mEditIsNew = false;
	DeckDialog *mNameDialog = nullptr;
	ULONGLONG mLastAutoSave = 0;
	ULONGLONG mLastTrayCheck = 0;

	// m023: tray + global hotkeys live in Win32, the deck's state lives here.
	NativeHooks mHooks;
	std::vector<HotkeyEntry> mHotkeys;
	std::wstring mHotkeyPath;
	std::map<int, std::wstring> mHotkeyTargets; // hotkey id -> script path
	std::string mLastTrayTip;                   // NIM_MODIFY only on change
	std::vector<Fl_Menu_Item> mContextItems;    // the row menu, rebuilt per popup
	std::vector<std::string> mContextLabels;    // ... and the text it points at
	bool mQuitRequested = false;
	std::string mKernelSummary; // "2.0.25", read once from the loaded kernel
};

// A browser that runs the selected script on a double-click; a single click
// only selects it, and the right-click menu edits/reveals explicitly (m022).
class ScriptBrowser : public Fl_Browser
{
public:
	ScriptBrowser(int x, int y, int w, int h, DeckWindow *aOwner)
		: Fl_Browser(x, y, w, h)
		, mOwner(aOwner)
	{
	}

	void draw() override
	{
		Fl_Browser::draw();
		fl_push_clip(x(), y(), w(), h());
		DrawPeciaScrollbar(&scrollbar, false);
		fl_pop_clip();
	}

	// m032: click a hotkey cell and record the combo right there (Pecia's
	// ShortcutDialog gesture, without a dialog). Enter commits, Esc/blur
	// cancels; the cell itself paints the pending combo.
	void BeginHotkeyRecord(int aLine)
	{
		if (aLine <= 0 || aLine > size())
			return;
		mRecordLine = aLine;
		mRecordText.clear();
		take_focus();
		redraw();
	}

	int FocusLine() const { return mFocusLine; }
	void SetFocusLine(int aLine)
	{
		if (aLine != mFocusLine)
		{
			mFocusLine = aLine;
			redraw();
		}
	}

	int handle(int aEvent) override
	{
		// Hover feedback for the hotkey cell: Fl_Browser paints no row hover, so
		// the cell itself owns the highlight (m042).
		if (aEvent == FL_ENTER || aEvent == FL_MOVE || aEvent == FL_LEAVE)
		{
			int hoverLine = 0;
			if (aEvent != FL_LEAVE && size() >= 1)
			{
				hoverLine = LineAt(Fl::event_y());
				if (hoverLine > 0)
				{
					int bx = 0, by = 0, bw = 0, bh = 0;
					bbox(bx, by, bw, bh);
					int rx = 0, ry = 0, rw = 0, rh = 0;
					const int rowH = item_height(item_at(1));
					mOwner->HotkeyCellRect(bx, 0, rowH, rx, ry, rw, rh);
					if (Fl::event_x() < rx || Fl::event_x() >= rx + rw)
						hoverLine = 0;
				}
			}
			if (hoverLine != mHoverHotkeyLine)
			{
				mHoverHotkeyLine = hoverLine;
				redraw();
			}
			return 1;
		}
		if (mRecordLine > 0)
		{
			if (aEvent == FL_KEYDOWN)
			{
				const int key = Fl::event_key();
				if (key == FL_Escape)
				{
					CancelHotkeyRecord();
					return 1;
				}
				switch (key)
				{
				case FL_Control_L: case FL_Control_R:
				case FL_Shift_L: case FL_Shift_R:
				case FL_Alt_L: case FL_Alt_R:
				case FL_Meta_L: case FL_Meta_R:
				case FL_Caps_Lock: case FL_Num_Lock: case FL_Scroll_Lock:
					return 1; // wait for the real key
				default:
					break;
				}
				const std::string token = HotkeyKeyToken(key);
				if (!token.empty())
				{
					// Pecia commits on the captured key itself; there is no Enter
					// step, so clicking elsewhere after pressing the combo cannot
					// throw the new binding away.
					const std::string text = HotkeyRecordText(key);
					const int line = mRecordLine;
					mRecordLine = 0;
					mRecordText.clear();
					redraw();
					if (line > 0)
						mOwner->CommitInlineHotkey(mOwner->IndexOfLine(line), text);
				}
				return 1;
			}
			if (aEvent == FL_UNFOCUS)
			{
				CancelHotkeyRecord();
				return 0;
			}
			if (aEvent == FL_PUSH)
				CancelHotkeyRecord(); // a click elsewhere cancels, like Pecia
		}

		// Right-click: ONE meaning, whatever the row is doing - the deck's own
		// script menu (m026; the old "running row pops the script's tray menu"
		// takeover is gone, that action now lives in the row's 菜单 cell).
		if (aEvent == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE)
		{
			mMenuHit = 0;
			const int line = LineAt(Fl::event_y());
			if (line > 0)
			{
				SetFocusLine(line);
				mOwner->ShowContextMenu(mOwner->IndexOfLine(line));
			}
			return 1;
		}

		// m031: three cells are buttons now - the play/pause glyph, the hotkey
		// cell and (while running) the 菜单 cell. Hit-test before the browser's
		// own selection so clicking a button never changes the selection.
		if (aEvent == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE)
		{
			const int line = LineAt(Fl::event_y());
			mMenuHit = 0;
			mPlayHit = 0;
			mHotkeyHit = 0;
			if (line > 0)
			{
				// bbox() is where item_draw gets its row origin from, so the hit
				// test and the painted cell cannot drift apart. Only the x range
				// matters: `line` already came from the click's y.
				int bx = 0, by = 0, bw = 0, bh = 0;
				bbox(bx, by, bw, bh);
				if (Fl::event_x() < bx + mOwner->DotColumnWidth())
				{
					// The cell still runs/stops the row when it shows the editing
					// pen: the pen is a status hint, not a new action.
					mPlayHit = line;
					return 1;
				}
				int rx = 0, ry = 0, rw = 0, rh = 0;
				const int rowH = item_height(item_at(1));
				mOwner->HotkeyCellRect(bx, 0, rowH, rx, ry, rw, rh);
				if (Fl::event_x() >= rx && Fl::event_x() < rx + rw)
				{
					mHotkeyHit = line;
					return 1;
				}
				mOwner->MenuCellRect(bx, 0, rowH, rx, ry, rw, rh);
				if (Fl::event_x() >= rx && Fl::event_x() < rx + rw)
				{
					// The gear is a cell even when idle: swallowing the click keeps
					// it from turning into a name focus.
					if (mOwner->LightOf(mOwner->IndexOfLine(line)) == DeckWindow::RowLight::Running)
						mMenuHit = line;
					return 1; // wait for the release, like a button does
				}
			}
		}
		else if (aEvent == FL_RELEASE)
		{
			const int line = LineAt(Fl::event_y());
			if (mPlayHit)
			{
				const int hit = mPlayHit;
				mPlayHit = 0;
				if (line == hit)
					mOwner->RunOrStopRow(mOwner->IndexOfLine(hit));
				return 1;
			}
			if (mHotkeyHit)
			{
				const int hit = mHotkeyHit;
				mHotkeyHit = 0;
				if (line == hit)
					BeginHotkeyRecord(hit);
				return 1;
			}
			if (mMenuHit)
			{
				const int hit = mMenuHit;
				mMenuHit = 0;
				if (line == hit)
					mOwner->OpenScriptMenu(mOwner->IndexOfLine(hit));
				return 1;
			}
		}

		// The remaining left-click area is the name (or the row body outside the
		// three buttons). Focus that cell only; never select/highlight the whole
		// browser row. Double-click still runs the script.
		if (aEvent == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE)
		{
			const int line = LineAt(Fl::event_y());
			const int x = Fl::event_x();
			const bool onScrollbar = scrollbar.visible()
				&& x >= scrollbar.x() && x < scrollbar.x() + scrollbar.w();
			if (line > 0 && !onScrollbar)
			{
				SetFocusLine(line);
				deselect(); // kill any browser row selection/focus box
				take_focus(); // F2/F5/F8 are window shortcuts; keep FLTK focus here
				const ULONGLONG now = GetTickCount64();
				const int y = Fl::event_y();
				const int dx = x > mLastX ? x - mLastX : mLastX - x;
				const int dy = y > mLastY ? y - mLastY : mLastY - y;
				if (mHasLast && now - mLastTick <= (ULONGLONG)GetDoubleClickTime() && dx <= 4 && dy <= 4)
				{
					mHasLast = false;
					mOwner->RunOrStopRow(mOwner->IndexOfLine(line));
				}
				else
				{
					mHasLast = true;
					mLastTick = now;
					mLastX = x;
					mLastY = y;
				}
				return 1;
			}
		}
		const int handled = Fl_Browser::handle(aEvent);
		if (selection())
			deselect(); // FLTK paints a focus box around selection_; we never want it
		return handled;
	}

protected:
	// The two cells the text columns cannot carry: the state light and the button
	// look of the 菜单 cell. Everything else is Fl_Browser's own column drawing.
	void item_draw(void *item, int X, int Y, int W, int H) const override
	{
		const int line = lineno(item);
		const bool editing = mOwner->IsEditingLine(line);
		const bool editingDirty = mOwner->IsEditingLineDirty(line);
		const DeckWindow::RowLight light = mOwner->LightOf(mOwner->IndexOfLine(line));

		int rx = 0, ry = 0, rw = 0, rh = 0;
		mOwner->MenuCellRect(X, Y, H, rx, ry, rw, rh);

		// Pecia's combo cell: panel-coloured fill (background3) so it reads
		// as an input, then the binding text from Fl_Browser on top.
		int hx = 0, hy = 0, hw = 0, hh = 0;
		mOwner->HotkeyCellRect(X, Y, H, hx, hy, hw, hh);

		Fl_Browser::item_draw(item, X, Y, W, H);

		if (mRecordLine == line)
		{
			// Recording: cover the old binding, show the pending combo (or the
			// prompt) inside the cell itself - no separate strip.
			fl_color(DeckColors().highlight1);
			fl_rectf(hx, hy, hw, hh);
			fl_color(DeckColors().text1);
			fl_rect(hx, hy, hw, hh);
			fl_color(DeckColors().text1);
			fl_font(FL_HELVETICA, 16);
			const std::string text = mRecordText.empty() ? "请按下按键组合..." : mRecordText;
			fl_font(FL_HELVETICA, HotkeyTextSize(text, hw - 12));
			fl_draw(text.c_str(), hx + 8, hy, hw - 12, hh,
				FL_ALIGN_LEFT | FL_ALIGN_CLIP);
		}
		else
		{
			// Cover Fl_Browser's own text and redraw with Pecia's 8px inset.
			const int index = mOwner->IndexOfLine(line);
			const std::string binding = mOwner->ScriptBindingAt(index);
			fl_color(mHoverHotkeyLine == line ? DeckColors().highlight1
											 : DeckColors().background3);
			fl_rectf(hx, hy, hw, hh);
			fl_color(DeckColors().text1);
			fl_rect(hx, hy, hw, hh);
			fl_color(DeckColors().text1);
			const std::string display = HotkeyDisplayText(binding);
			fl_font(FL_HELVETICA, HotkeyTextSize(display, hw - 12));
			fl_draw(display.c_str(), hx + 8, hy, hw - 12, hh,
				FL_ALIGN_LEFT | FL_ALIGN_CLIP);
		}

		// m037: the menu column is a gear between status and script. Running =
		// live (click opens the script's tray menu); idle = grey and inert.
		{
			const bool menuOn = light == DeckWindow::RowLight::Running;
			DrawGear(rx + rw / 2, ry + rh / 2, 6,
				menuOn ? DeckColors().text1 : DeckColors().text2);
		}

		// m037: the focused row gets a filled selection background behind the
		// name text only (never the whole row); the buttons keep their state.
		if (mFocusLine == line)
		{
			int nx = 0, nw = 0;
			mOwner->NameCellRect(X, nx, nw);
			const std::string name = mOwner->ScriptNameAt(mOwner->IndexOfLine(line));
			fl_font(textfont(), textsize());
			const int tw = (int)fl_width(name.c_str());
			const int bw = tw + 6 < nw - 4 ? tw + 6 : nw - 4;
			const Fl_Color sel = DeckColors().highlight1;
			fl_color(sel);
			fl_rectf(nx + 2, Y + 1, bw, H - 2);
			fl_color(fl_contrast(DeckColors().text1, sel));
			fl_draw(name.c_str(), nx + 3, Y, nw - 6, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
		}

		// m043: the file open in the editor turns its name red. The focus row
		// above may already have painted a highlight; repaint just the text span
		// so both states coexist.
		if (editing)
		{
			int nx = 0, nw = 0;
			mOwner->NameCellRect(X, nx, nw);
			const std::string name = mOwner->ScriptNameAt(mOwner->IndexOfLine(line));
			fl_font(textfont(), textsize());
			const int avail = nw - 6;
			const int starW = editingDirty ? (int)fl_width(" *") : 0;
			const int nameW = (int)fl_width(name.c_str());
			const int nameRoom = avail - starW > 0 ? avail - starW : 0;
			const int shownNameW = nameW < nameRoom ? nameW : nameRoom;
			const int totalW = shownNameW + starW;
			const int bw = totalW + 6 < nw - 4 ? totalW + 6 : nw - 4;
			const Fl_Color bg = (mFocusLine == line) ? DeckColors().highlight1
													 : DeckColors().background1;
			fl_color(bg);
			fl_rectf(nx + 2, Y + 1, bw, H - 2);
			fl_color(AlertRed());
			fl_draw(name.c_str(), nx + 3, Y, nameRoom > 0 ? nameRoom : 1, H,
				FL_ALIGN_LEFT | FL_ALIGN_CLIP);
			if (editingDirty)
				fl_draw(" *", nx + 3 + shownNameW, Y, starW + 1, H,
					FL_ALIGN_LEFT | FL_ALIGN_CLIP);
		}

		// m077: the row's left edge carries the LAST RESULT - a 2px red strip
		// when the last run ended badly. A row-level mark, so the clickable cell
		// below can stay a pure button.
		if (mOwner->LastRunFailed(mOwner->IndexOfLine(line)))
		{
			fl_color(AlertRed());
			fl_rectf(X, Y, 2, H);
		}

		// First cell = the action, and only the action: green play when clicking
		// will start it, red pause while it runs. "This is the file being edited"
		// is the red name plus " *" above (m043); the pen used to overwrite this
		// cell and hide the state, which is what m069 filed.
		const int d = 10;
		const int cx = X + (mOwner->DotColumnWidth() - d) / 2;
		const int cy = Y + (H - d) / 2;
		if (light == DeckWindow::RowLight::Running)
		{
			const int bar = 4;
			fl_color(AlertRed());
			fl_rectf(cx, cy, bar, d);
			fl_rectf(cx + d - bar, cy, bar, d);
		}
		else
		{
			fl_color(fl_rgb_color(0x2E, 0x7D, 0x32));
			fl_begin_polygon();
			fl_vertex(cx, cy);
			fl_vertex(cx, cy + d);
			fl_vertex(cx + d, cy + d / 2);
			fl_end_polygon();
		}
	}

private:

	void CancelHotkeyRecord()
	{
		mRecordLine = 0;
		mRecordText.clear();
		redraw();
	}

	int LineAt(int aY)
	{
		if (aY < y() || aY >= y() + h())
			return 0;
		if (size() < 1)
			return 0;
		// find_item() is null below the last row and in the empty tail. The old
		// arithmetic clamped to size(), so the empty area under the list acted
		// as the last row (double-click started/stopped the last script).
		void *item = find_item(aY);
		if (!item)
			return 0;
		return lineno(item);
	}

	DeckWindow *mOwner;
	bool mHasLast = false;
	int mMenuHit = 0;   // line whose 菜单 cell is being pressed, 0 = none
	int mPlayHit = 0;   // line whose play/pause glyph is being pressed
	int mHotkeyHit = 0; // line whose hotkey button is being pressed
	int mRecordLine = 0;      // line recording a new combo (m032)
	int mHoverHotkeyLine = 0; // line whose hotkey cell is under the mouse (m042)
	int mFocusLine = 0;       // line whose name is focused (m037 local selection)
	std::string mRecordText;  // the combo captured so far
	ULONGLONG mLastTick = 0;
	int mLastX = 0;
	int mLastY = 0;
};

// Pecia's clipEllipsis, copied as-is: shorten a name to fit the row and end it
// with "...", backing off to a UTF-8 character boundary. The full name stays
// reachable through the row tooltip.
static std::string ClipEllipsis(const std::string &aText, int aMaxW)
{
	if (aMaxW < 24)
		return aText.substr(0, 1);
	int textW = 0;
	int textH = 0;
	fl_measure(aText.c_str(), textW, textH);
	if (textW <= aMaxW)
		return aText;
	const std::string dots = "...";
	int lo = 0;
	int hi = (int)aText.size();
	while (lo < hi)
	{
		const int mid = (lo + hi + 1) / 2;
		const std::string probe = aText.substr(0, mid) + dots;
		fl_measure(probe.c_str(), textW, textH);
		if (textW <= aMaxW)
			lo = mid;
		else
			hi = mid - 1;
	}
	while (lo > 0 && ((unsigned char)aText[lo] & 0xC0) == 0x80)
		--lo;
	return aText.substr(0, lo) + dots;
}

// m073: Pecia's Settings > Shortcuts dialog, copied function for function.
// One row per script: the name occupies the row, the combo is a 130px box at
// its right. Click the combo to record (the accent fill is the recording
// state), the hint in the filter bar shows the prompt/error, and the filter
// bar sits at the BOTTOM of the list with the button bar under it.
class ShortcutDialogWindow : public DeckDialog
{
public:
	class RowWidget;

	explicit ShortcutDialogWindow(DeckWindow *aDeck);
	~ShortcutDialogWindow() override = default;

	// Clicking the caption's close box is Cancel: nothing was committed yet.
	void OnCaptionClose() override { hide(); }

	int handle(int aEvent) override;

private:
	struct RowState
	{
		std::string name;   // script leaf name, the hotkey table's key
		std::string combo;  // canonical AHK spelling, empty = unbound
		bool conflict = false;
	};

	static std::string Lower(std::string aText);

	// The same three rejection classes as Pecia's shortcutValidate, plus the
	// rule that comes from OUR scope: these bindings are system-wide
	// RegisterHotKey entries, so a bare key (even F5) would swallow that key in
	// every application. Pecia can accept bare keys because its shortcuts are
	// in-window menu accelerators.
	static bool ValidateCombo(int aKey, unsigned aMods, std::string &aCanonical,
		std::string &aError);

	static bool SameBinding(const std::string &aLeft, const std::string &aRight);

	std::string ComboText(const std::string &aCombo) const;

	void BuildRows(int aPad);
	void RepositionRows();
	bool rowVisible(size_t aIndex) const;
	void ApplyFilter();
	void UpdateHintVisibility();
	void UpdateHintText();
	void SetError(const char *aText);
	void RebuildComboLabels();
	void RecomputeConflicts();
	void OnRowComboClicked(RowWidget *aRow);
	void OnRowBlur(RowWidget *aRow);
	void StopRecording();
	void OnRowComboChanged(RowWidget *aRow, int aKey, unsigned aMods);
	void ResetToDefaults();
	void FinishOk();

	DeckWindow *mDeck = nullptr;
	PeciaScroll *mScroll = nullptr;
	SearchInput *mFilter = nullptr;
	Fl_Box *mFilterHint = nullptr;
	Fl_Button *mResetBtn = nullptr;
	Fl_Button *mOkBtn = nullptr;
	Fl_Button *mCancelBtn = nullptr;
	Fl_Box *mTopSpacer = nullptr;
	Fl_Box *mBottomSpacer = nullptr;
	std::vector<RowState> mRows;
	std::vector<RowWidget *> mWidgets;
	RowWidget *mRecording = nullptr;
	std::string mFilterText;
	std::string mError;
};

// One action row: full-width, name on the left (clipped with "..." and the
// full name in the tooltip), the 130px combo box on the right. The combo's
// fill is the recording state; a conflict turns its border/text red.
class ShortcutDialogWindow::RowWidget : public Fl_Box
{
public:
	RowWidget(int aX, int aY, int aW, int aH)
		: Fl_Box(aX, aY, aW, aH)
	{
		box(FL_NO_BOX);
		// take_focus() bails out without this, and a row that never gets
		// keyboard focus cannot record a combo.
		set_visible_focus();
	}

	void setDialog(ShortcutDialogWindow *aDialog) { mDialog = aDialog; }
	void setIndex(int aIndex) { mIndex = aIndex; }
	void setRecording(bool aOn)
	{
		mRecording = aOn;
		redraw();
	}
	void setConflict(bool aOn)
	{
		mConflict = aOn;
		redraw();
	}
	void setComboText(const std::string &aText)
	{
		mComboText = aText;
		redraw();
	}
	void setName(const std::string &aName)
	{
		mName = aName;
		tooltip(aName.c_str());
	}
	int index() const { return mIndex; }

	int handle(int aEvent) override;
	void draw() override;

private:
	static constexpr int kComboW = 130;
	int ComboX() const { return x() + w() - kComboW - 12; }

	ShortcutDialogWindow *mDialog = nullptr;
	int mIndex = -1;
	bool mRecording = false;
	bool mConflict = false;
	std::string mName;
	std::string mComboText;
};

ShortcutDialogWindow::ShortcutDialogWindow(DeckWindow *aDeck)
	: DeckDialog(520, 560 + DeckTitleBar::kHeight, "快捷键")
	, mDeck(aDeck)
{
	const int top = ContentTop();
	const int margin = 12;
	const int barH = 32;
	const int btnH = 24;
	const int pad = 16;      // Pecia's dialog_pad: above and below the rows
	const int searchH = 22;
	const int iconW = 24;
	const int filterTop = top + 560 - 2 * barH;
	const int btnTop = filterTop + barH;

	begin();

	// The list owns the whole pane: flush against the caption above and the
	// filter bar below. Rows are drawn inside it, not as a second frame.
	mScroll = new PeciaScroll(0, top, 520, filterTop - top);
	mScroll->type(Fl_Scroll::VERTICAL);
	mScroll->scrollbar.type(FL_VERT_SLIDER);
	mScroll->box(FL_FLAT_BOX);
	mScroll->color(DeckColors().background1);
	mScroll->end();

	// Filter bar: the find-bar look - panel background, one bordered input
	// with the magnifier inside, and a light in-input hint. The hint doubles
	// as the recording prompt and the error line, so the bar never grows.
	Fl_Group *filterBar = new Fl_Group(0, filterTop, 520, barH);
	filterBar->box(FL_FLAT_BOX);
	filterBar->color(DeckColors().background3);
	mFilter = new SearchInput(24, filterTop + (barH - searchH) / 2,
		520 - 48, searchH);
	mFilter->textsize(16);
	mFilter->color(DeckColors().background1);
	mFilter->textcolor(DeckColors().text1);
	mFilter->selection_color(DeckColors().highlight2);
	mFilter->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
	mFilter->callback([](Fl_Widget *, void *aSelf) {
		static_cast<ShortcutDialogWindow *>(aSelf)->ApplyFilter();
	}, this);
	mFilterHint = new Fl_Box(FL_NO_BOX, 24 + iconW + 4,
		filterTop + (barH - searchH) / 2, 520 - 48 - iconW - 8, searchH,
		"过滤");
	mFilterHint->labelsize(16);
	mFilterHint->labelcolor(DeckColors().text2);
	mFilterHint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	filterBar->resizable(nullptr);
	filterBar->end();

	// Button bar: [恢复默认] [确定] [取消], right-aligned, auto width.
	Fl_Group *btnBar = new Fl_Group(0, btnTop, 520, barH);
	btnBar->box(FL_FLAT_BOX);
	btnBar->color(DeckColors().background2);
	const int btnY = btnTop + (barH - btnH) / 2;
	HoverButton *reset = new HoverButton(0, btnY, 0, btnH, "恢复默认");
	HoverButton *ok = new HoverButton(0, btnY, 0, btnH, "确定");
	HoverButton *cancel = new HoverButton(0, btnY, 0, btnH, "取消");
	mResetBtn = reset;
	mOkBtn = ok;
	mCancelBtn = cancel;
	for (HoverButton *button : { reset, ok, cancel })
	{
		button->color(DeckColors().background2);
		button->selection_color(DeckColors().highlight1);
		button->labelsize(16);
		button->labelcolor(DeckColors().text1);
	}
	FitButtonRow({ mResetBtn, mOkBtn, mCancelBtn }, 520, btnTop + barH / 2,
		margin, 8);
	mOkBtn->callback([](Fl_Widget *, void *aSelf) {
		static_cast<ShortcutDialogWindow *>(aSelf)->FinishOk();
	}, this);
	mCancelBtn->callback([](Fl_Widget *, void *aSelf) {
		static_cast<ShortcutDialogWindow *>(aSelf)->hide();
	}, this);
	mResetBtn->callback([](Fl_Widget *, void *aSelf) {
		static_cast<ShortcutDialogWindow *>(aSelf)->ResetToDefaults();
	}, this);
	btnBar->resizable(nullptr);
	btnBar->end();

	end();

	BuildRows(pad);
	UpdateHintVisibility();
}

int ShortcutDialogWindow::handle(int aEvent)
{
	// Pecia: a click on empty dialog space also cancels the recording; row
	// widgets consume their own clicks and stop it there.
	if (aEvent == FL_PUSH && mRecording)
	{
		StopRecording();
		UpdateHintText();
	}
	// Pecia's dialog-wide OK shortcut (its default combo): Ctrl+Enter confirms
	// unless a row is recording - then the keys belong to the recording row.
	if ((aEvent == FL_KEYDOWN || aEvent == FL_SHORTCUT) && !mRecording &&
		Fl::event_key() == FL_Enter && (Fl::event_state() & FL_CTRL))
	{
		FinishOk();
		return 1;
	}
	if (aEvent == FL_CLOSE)
	{
		hide();
		return 1;
	}
	const int ret = Fl_Double_Window::handle(aEvent);
	if (aEvent == FL_FOCUS || aEvent == FL_UNFOCUS || aEvent == FL_KEYUP)
		UpdateHintVisibility();
	return ret;
}

std::string ShortcutDialogWindow::Lower(std::string aText)
{
	for (char &c : aText)
	{
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	}
	return aText;
}

bool ShortcutDialogWindow::ValidateCombo(int aKey, unsigned aMods,
	std::string &aCanonical, std::string &aError)
{
	if ((aMods & FL_CTRL) && (aMods & FL_ALT) && aKey == FL_Delete)
	{
		aError = "Ctrl+Alt+Del 已被系统保留";
		return false;
	}
	const unsigned mods = aMods & (FL_CTRL | FL_SHIFT | FL_ALT | FL_META);
	if (mods == 0)
	{
		aError = "全局快捷键至少要有一个修饰键（Ctrl / Alt / Shift / Win）";
		return false;
	}
	if (mods == FL_SHIFT &&
		((aKey >= 'a' && aKey <= 'z') || (aKey >= 'A' && aKey <= 'Z') ||
			(aKey >= '0' && aKey <= '9')))
	{
		aError = "Shift+字母或数字会干扰打字";
		return false;
	}
	const std::string token = HotkeyKeyToken(aKey);
	if (token.empty())
	{
		aError = "这个键不能作为全局快捷键";
		return false;
	}
	std::string text;
	if (mods & FL_CTRL)
		text += "Ctrl+";
	if (mods & FL_ALT)
		text += "Alt+";
	if (mods & FL_SHIFT)
		text += "Shift+";
	if (mods & FL_META)
		text += "Win+";
	text += token;
	unsigned int parsedMods = 0;
	unsigned int parsedKey = 0;
	return ParseAhkHotkey(text, parsedMods, parsedKey, aCanonical, aError);
}

bool ShortcutDialogWindow::SameBinding(const std::string &aLeft, const std::string &aRight)
{
	if (aLeft.empty() || aRight.empty())
		return false;
	unsigned int modsA = 0, keyA = 0, modsB = 0, keyB = 0;
	std::string canonicalA, canonicalB, error;
	if (!ParseAhkHotkey(aLeft, modsA, keyA, canonicalA, error) ||
		!ParseAhkHotkey(aRight, modsB, keyB, canonicalB, error))
		return false;
	return keyA == keyB && (modsA & 0x00FF) == (modsB & 0x00FF);
}

std::string ShortcutDialogWindow::ComboText(const std::string &aCombo) const
{
	return HotkeyDisplayText(aCombo);
}

void ShortcutDialogWindow::BuildRows(int aPad)
{
	mScroll->begin();
	// The top spacer is part of the scrollable content: without it FL_Scroll
	// starts at a negative offset and the top pad vanishes on the first drag.
	mTopSpacer = new Fl_Box(FL_NO_BOX, 0, mScroll->y(), 0, aPad, nullptr);
	const int count = mDeck ? mDeck->ScriptCount() : 0;
	for (int i = 0; i < count; ++i)
	{
		RowState state;
		state.name = mDeck->ScriptNameAt(i);
		state.combo = mDeck->ScriptBindingAt(i);
		// The file is hand-editable ("^!a"), so normalize the spelling the way
		// Pecia does on load: parse and re-serialize. Unparseable text is kept
		// verbatim so the row still shows what the file actually says.
		if (!state.combo.empty())
		{
			unsigned int mods = 0;
			unsigned int key = 0;
			std::string canonical;
			std::string error;
			if (ParseAhkHotkey(state.combo, mods, key, canonical, error))
				state.combo = canonical;
		}
		mRows.push_back(state);
		RowWidget *row = new RowWidget(12, 0, 520 - 24, 26);
		row->setDialog(this);
		row->setIndex(i);
		row->setName(state.name);
		row->setComboText(ComboText(state.combo));
		mWidgets.push_back(row);
	}
	mBottomSpacer = new Fl_Box(FL_NO_BOX, 0, 0, 0, aPad, nullptr);
	mScroll->end();
	RepositionRows();
	RecomputeConflicts();
}

// Visible rows stack from the top with Pecia's 26px/2px rhythm; the top and
// bottom spacers keep dialog_pad at both ends of the scrollable content.
void ShortcutDialogWindow::RepositionRows()
{
	if (!mScroll)
		return;
	const int pad = 16;
	if (mTopSpacer)
		mTopSpacer->resize(0, mScroll->y(), 0, pad);
	int y = mScroll->y() + pad;
	int lastRowBottom = y;
	for (size_t i = 0; i < mWidgets.size(); ++i)
	{
		RowWidget *row = mWidgets[i];
		if (!rowVisible(i))
		{
			row->hide();
			continue;
		}
		row->show();
		row->resize(12, y, w() - 24, 26);
		y += 28;
		lastRowBottom = y - 2;
	}
	if (mBottomSpacer)
		mBottomSpacer->resize(0, lastRowBottom, 0, pad);
	mScroll->redraw();
}

// Pecia's filter: a lowercase substring of the name or of the shortcut text.
bool ShortcutDialogWindow::rowVisible(size_t aIndex) const
{
	if (mFilterText.empty())
		return true;
	if (aIndex >= mRows.size())
		return false;
	const RowState &state = mRows[aIndex];
	if (Lower(state.name).find(mFilterText) != std::string::npos)
		return true;
	return Lower(ComboText(state.combo)).find(mFilterText) != std::string::npos;
}

void ShortcutDialogWindow::ApplyFilter()
{
	StopRecording();
	const char *value = mFilter ? mFilter->value() : nullptr;
	mFilterText = Lower(value ? value : "");
	RepositionRows();
	UpdateHintVisibility();
}

// Same rule as Pecia's find bar: the hint is visible only while the input
// is empty AND unfocused.
void ShortcutDialogWindow::UpdateHintVisibility()
{
	if (!mFilterHint || !mFilter)
		return;
	const char *value = mFilter->value();
	if (!value || !*value)
		(Fl::focus() == mFilter) ? mFilterHint->hide() : mFilterHint->show();
	else
		mFilterHint->hide();
}

// Hint priority: error (red), recording prompt, plain "过滤".
void ShortcutDialogWindow::UpdateHintText()
{
	if (!mFilterHint)
		return;
	if (!mError.empty())
	{
		mFilterHint->label(mError.c_str());
		mFilterHint->labelcolor(DeckColors().highlight3);
	}
	else if (mRecording)
	{
		mFilterHint->label("请按下按键组合...");
		mFilterHint->labelcolor(DeckColors().text1);
	}
	else
	{
		mFilterHint->label("过滤");
		mFilterHint->labelcolor(DeckColors().text2);
	}
	mFilterHint->redraw();
}

void ShortcutDialogWindow::SetError(const char *aText)
{
	mError = aText ? aText : "";
	UpdateHintText();
}

void ShortcutDialogWindow::RebuildComboLabels()
{
	for (size_t i = 0; i < mRows.size() && i < mWidgets.size(); ++i)
		mWidgets[i]->setComboText(ComboText(mRows[i].combo));
}

void ShortcutDialogWindow::RecomputeConflicts()
{
	for (size_t i = 0; i < mRows.size(); ++i)
	{
		bool conflict = false;
		if (!mRows[i].combo.empty())
		{
			for (size_t j = 0; j < mRows.size() && !conflict; ++j)
			{
				if (i != j && SameBinding(mRows[i].combo, mRows[j].combo))
					conflict = true;
			}
		}
		mRows[i].conflict = conflict;
		if (i < mWidgets.size())
			mWidgets[i]->setConflict(conflict);
	}
}

void ShortcutDialogWindow::OnRowComboClicked(RowWidget *aRow)
{
	if (mRecording == aRow)
	{
		// Clicking the same box again cancels the recording.
		StopRecording();
		SetError(nullptr);
		return;
	}
	StopRecording();
	if (aRow->index() >= 0 && (size_t)aRow->index() < mRows.size())
		mRows[(size_t)aRow->index()].combo.clear();
	mRecording = aRow;
	aRow->setRecording(true);
	aRow->take_focus();
	RebuildComboLabels();
	RecomputeConflicts();
	SetError(nullptr);
	UpdateHintText();
}

// Click outside a combo box (or focus loss) cancels the recording.
void ShortcutDialogWindow::OnRowBlur(RowWidget *aRow)
{
	if (mRecording != aRow)
		return;
	StopRecording();
	SetError(nullptr);
}

void ShortcutDialogWindow::StopRecording()
{
	if (!mRecording)
		return;
	mRecording->setRecording(false);
	mRecording = nullptr;
	UpdateHintText();
}

void ShortcutDialogWindow::OnRowComboChanged(RowWidget *aRow, int aKey, unsigned aMods)
{
	if (mRecording != aRow)
		return;
	std::string canonical;
	std::string error;
	if (!ValidateCombo(aKey, aMods, canonical, error))
	{
		SetError(error.c_str());
		return; // keep recording
	}
	const int index = aRow->index();
	if (index < 0 || (size_t)index >= mRows.size())
		return;
	// New binding wins: the action that already owned the combo is cleared,
	// exactly like Pecia (one combo per action, no silent duplicates).
	for (size_t i = 0; i < mRows.size(); ++i)
	{
		if ((int)i != index && SameBinding(mRows[i].combo, canonical))
			mRows[i].combo.clear();
	}
	StopRecording();
	mRows[(size_t)index].combo = canonical;
	RebuildComboLabels();
	RecomputeConflicts();
	SetError(nullptr);
}

void ShortcutDialogWindow::ResetToDefaults()
{
	StopRecording();
	// Scripts have no built-in shortcut: the default is "unbound".
	for (RowState &state : mRows)
		state.combo.clear();
	RebuildComboLabels();
	RecomputeConflicts();
	SetError(nullptr);
}

void ShortcutDialogWindow::FinishOk()
{
	std::vector<std::pair<std::string, std::string>> rows;
	rows.reserve(mRows.size());
	for (const RowState &state : mRows)
		rows.push_back({ state.name, state.combo });
	if (mDeck && mDeck->ApplyShortcutRows(rows))
		hide();
}

int ShortcutDialogWindow::RowWidget::handle(int aEvent)
{
	if (aEvent == FL_PUSH)
	{
		const bool inCombo = Fl::event_x() >= ComboX() &&
			Fl::event_x() < ComboX() + kComboW;
		if (mDialog)
		{
			if (inCombo)
				mDialog->OnRowComboClicked(this);
			else
				mDialog->OnRowBlur(this); // click elsewhere cancels
		}
		return 1;
	}
	if (aEvent == FL_KEYDOWN && mRecording)
	{
		const int key = Fl::event_key();
		// Modifiers alone are not a combo; wait for the real key.
		switch (key)
		{
		case FL_Control_L: case FL_Control_R:
		case FL_Shift_L: case FL_Shift_R:
		case FL_Alt_L: case FL_Alt_R:
		case FL_Meta_L: case FL_Meta_R:
		case FL_Caps_Lock: case FL_Num_Lock: case FL_Scroll_Lock:
			return 1;
		default:
			break;
		}
		if (mDialog)
			mDialog->OnRowComboChanged(this, key,
				(unsigned)(Fl::event_state() & (FL_CTRL | FL_SHIFT | FL_ALT | FL_META)));
		return 1;
	}
	if (aEvent == FL_UNFOCUS && mRecording)
	{
		if (mDialog)
			mDialog->OnRowBlur(this);
		return 1;
	}
	// Accept focus (and keep it) so the row receives FL_KEYDOWN while recording.
	if (aEvent == FL_FOCUS)
		return 1;
	return Fl_Box::handle(aEvent);
}

void ShortcutDialogWindow::RowWidget::draw()
{
	Fl_Box::draw();
	const ThemeColors &colors = DeckColors();

	// Name: same colour in every state, clipped to leave room for the combo.
	fl_color(colors.text1);
	fl_font(FL_HELVETICA, 16);
	const int avail = w() - kComboW - 32;
	const std::string clipped = ClipEllipsis(mName, avail - 12);
	fl_draw(clipped.c_str(), x() + 12, y(), avail, h(),
		FL_ALIGN_LEFT | FL_ALIGN_CENTER, nullptr, 0);

	// Combo: bordered, panel fill; recording gets the accent background and
	// a conflict turns both border and text red.
	const int cx = ComboX();
	const int cy = y() + 1;
	const int ch = h() - 2;
	fl_color(mRecording ? colors.highlight1 : colors.background3);
	fl_rectf(cx, cy, kComboW, ch);
	fl_color(mConflict ? colors.highlight3 : colors.text1);
	fl_rect(cx, cy, kComboW, ch);
	fl_color(mRecording ? colors.text1
		: (mConflict ? colors.highlight3 : colors.text1));
	fl_font(FL_HELVETICA, 16);
	// Left-aligned so every shortcut starts at the same x ("F3", "Ctrl+F"
	// and "Delete" share one column; centering would float each one).
	fl_draw(mComboText.c_str(), cx + 8, cy, kComboW - 12, ch,
		FL_ALIGN_LEFT | FL_ALIGN_CENTER, nullptr, 0);
}


// m054: one name field for both "new unsaved script" and "rename a row".
// Keeping it in the deck (not a native save dialog) means the name is always a
// script leaf in exe\Scripts; the list and the editor can never disagree about
// where the file lives.
class NameWindow : public DeckDialog
{
public:
	using Validator = std::function<std::string(const std::string &)>;
	using Acceptor = std::function<void(const std::string &)>;

	NameWindow(const char *aTitle, const char *aInitial, Validator aValidate, Acceptor aAccept)
		: DeckDialog(400, 172 + DeckTitleBar::kHeight, aTitle)
		, mValidate(std::move(aValidate))
		, mAccept(std::move(aAccept))
	{
		const int top = ContentTop();
		begin();
		Fl_Box *label = new Fl_Box(24, top + 18, 352, 20, "脚本名称（后缀自动为 .ahk）");
		label->box(FL_NO_BOX);
		label->labelcolor(DeckColors().text2);
		label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		label->labelsize(16);
		mInput = new BlinkInput(24, top + 44, 352, 26);
		mInput->value(aInitial);
		StyleInput(mInput);
		mInput->when(FL_WHEN_ENTER_KEY_ALWAYS);
		mInput->callback([](Fl_Widget *, void *aSelf) {
			static_cast<NameWindow *>(aSelf)->Accept();
		}, this);
		mHint = new Fl_Box(24, top + 76, 352, 20, "");
		mHint->box(FL_NO_BOX);
		mHint->labelcolor(DeckColors().highlight2);
		mHint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		mHint->labelsize(14);
		HoverButton *ok = new HoverButton(216, top + 124, 76, 26, "确定");
		HoverButton *cancel = new HoverButton(300, top + 124, 76, 26, "取消");
		StyleButton(ok);
		StyleButton(cancel);
		ok->callback([](Fl_Widget *, void *aSelf) {
			static_cast<NameWindow *>(aSelf)->Accept();
		}, this);
		cancel->callback([](Fl_Widget *, void *aSelf) {
			static_cast<NameWindow *>(aSelf)->hide();
		}, this);
		end();
	}

	void focusInput()
	{
		mInput->take_focus();
		mInput->insert_position(0, (int)std::strlen(mInput->value()));
	}

	int handle(int aEvent) override
	{
		if (aEvent == FL_KEYBOARD && Fl::event_key() == FL_Escape)
		{
			hide();
			return 1;
		}
		return Fl_Double_Window::handle(aEvent);
	}

private:
	void Accept()
	{
		const char *raw = mInput->value();
		std::string name = raw ? raw : "";
		const size_t first = name.find_first_not_of(" \t\r\n");
		const size_t last = name.find_last_not_of(" \t\r\n");
		name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
		if (name.empty())
		{
			mHint->copy_label("名称不能为空");
			redraw();
			mInput->take_focus();
			return;
		}
		if (mValidate)
		{
			const std::string error = mValidate(name);
			if (!error.empty())
			{
				mHint->copy_label(error.c_str());
				redraw();
				mInput->take_focus();
				return;
			}
		}
		hide();
		if (mAccept)
			mAccept(name);
	}

	BlinkInput *mInput = nullptr;
	Fl_Box *mHint = nullptr;
	Validator mValidate;
	Acceptor mAccept;
};

// m050: settings window. Only the AI fields are exposed; advanced users edit
// settings.json through the 设置 menu.

class DeckDisplay : public Fl_Text_Display
{
public:
	DeckDisplay(int x, int y, int w, int h) : Fl_Text_Display(x, y, w, h)
	{
		box(FL_FLAT_BOX);
		scrollbar_size(10);
	}

	void draw() override
	{
		Fl_Text_Display::draw();
		fl_push_clip(x(), y(), w(), h());
		DrawPeciaScrollbarCorner(mVScrollBar, mHScrollBar);
		DrawPeciaScrollbar(mVScrollBar, false);
		DrawPeciaScrollbar(mHScrollBar, true);
		fl_pop_clip();
	}

	void StyleScrollbars()
	{
		const ThemeColors &colors = DeckColors();
		Fl_Scrollbar *bars[] = {mHScrollBar, mVScrollBar};
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
};

// A text editor that owns the file shortcuts, so they work even while the
// editor widget has focus (the global-handler route missed them), and that
// hosts Pecia's in-place IME composition so pinyin input draws ONE caret at
// the insertion point instead of FLTK's floating window plus its own caret.
class EditorBox : public Fl_Text_Editor, public ImeInPlace::Host
{
public:
	EditorBox(int x, int y, int w, int h, DeckWindow *aOwner)
		: Fl_Text_Editor(x, y, w, h)
		, mOwner(aOwner)
	{
		box(FL_FLAT_BOX);
		scrollbar_size(10);
	}

	~EditorBox() override
	{
		ImeInPlace::release(this);
	}

	void StyleScrollbars()
	{
		const ThemeColors &colors = DeckColors();
		Fl_Scrollbar *bars[] = {mHScrollBar, mVScrollBar};
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

	// --- ImeInPlace::Host ------------------------------------------------
	Fl_Widget *imeWidget() override { return this; }

	void imeCompositionStart() override
	{
		mCompActive = true;
		int start = insert_position();
		int end = start;
		if (buffer())
		{
			int selStart = 0, selEnd = 0;
			if (buffer()->selection_position(&selStart, &selEnd))
			{
				start = selStart;
				end = selEnd;
			}
		}
		if (start > end)
		{
			const int swap = start;
			start = end;
			end = swap;
		}
		mCompStart = start;
		mCompEnd = end;
		mCompText.clear();
		mCompCursorBytes = 0;
	}

	void imeCompositionUpdate(const char *utf8, int cursorBytes) override
	{
		if (!mCompActive)
			imeCompositionStart();
		if (!mCompActive)
			return;
		mCompText = utf8 ? utf8 : "";
		if (cursorBytes < 0)
			cursorBytes = 0;
		if (cursorBytes > (int)mCompText.size())
			cursorBytes = (int)mCompText.size();
		mCompCursorBytes = cursorBytes;
		redraw();
	}

	void imeCompositionCommit(const char *utf8) override
	{
		if (!mCompActive)
			imeCompositionStart();
		const char *text = utf8 ? utf8 : "";
		int start = mCompStart;
		int end = mCompEnd;
		const int len = buffer() ? buffer()->length() : 0;
		if (start < 0)
			start = 0;
		if (start > len)
			start = len;
		if (end < start)
			end = start;
		if (end > len)
			end = len;
		if (start != end || *text)
			buffer()->replace(start, end, text);
		insert_position(start + (int)std::strlen(text));
		mCompActive = false;
		mCompText.clear();
		mCompCursorBytes = 0;
		mCompStart = mCompEnd = -1;
		if (Fl::focus() == this)
			show_cursor(1);
		redraw();
	}

	void imeCompositionEnd() override
	{
		if (!mCompActive)
			return;
		mCompActive = false;
		mCompText.clear();
		mCompCursorBytes = 0;
		mCompStart = mCompEnd = -1;
		if (Fl::focus() == this)
			show_cursor(CaretBlink::visible() ? 1 : 0);
		redraw();
	}

	void draw() override
	{
		const bool composing = mCompActive && !mCompText.empty();
		// FLTK's caret sits at the insertion point; during composition that is
		// the start of the composing string, so showing both would give the
		// "two carets" the in-place host exists to remove.
		if (composing)
			show_cursor(0);
		Fl_Text_Editor::draw();
		// Pecia resizes the horizontal bar to the widget's left edge in draw():
		// FLTK leaves the line-number gutter under the bar's left end, which
		// showed as a stray colour block beside the track.
		if (mHScrollBar && mHScrollBar->visible())
		{
			const int vscrollW = (mVScrollBar && mVScrollBar->visible()) ? mVScrollBar->w() : 0;
			const int barW = w() - vscrollW;
			if (barW > 0 && (mHScrollBar->x() != x() || mHScrollBar->w() != barW))
				mHScrollBar->resize(x(), mHScrollBar->y(), barW, mHScrollBar->h());
		}
		DrawLineNumberSeparator();
		fl_push_clip(x(), y(), w(), h());
		DrawPeciaScrollbarCorner(mVScrollBar, mHScrollBar);
		DrawPeciaScrollbar(mVScrollBar, false);
		DrawPeciaScrollbar(mHScrollBar, true);
		DrawCursorScrollMarker();
		fl_pop_clip();
		if (!composing)
			return;

		int x = 0, y = 0;
		if (position_to_xy(mCompStart, &x, &y))
		{
			fl_font(textfont(), textsize());
			const int lineH = fl_height();
			ImeInPlace::drawComposition(
				mCompText.c_str(), mCompCursorBytes, x, y, lineH,
				textcolor(), cursor_color(),
				CaretBlink::visible() && Fl::focus() == this,
				textfont(), textsize(), window(),
				text_area.x, text_area.y, text_area.w, text_area.h);
		}
		if (Fl::focus() == this)
			show_cursor(CaretBlink::visible() ? 1 : 0);
	}

	int handle(int aEvent) override
	{
		// Focus must show the caret immediately; otherwise the last blink tick
		// could have left it hidden for up to 500 ms.
		if (aEvent == FL_FOCUS)
		{
			ImeInPlace::attach(this);
			CaretBlink::reset();
			show_cursor(1);
		}
		else if (aEvent == FL_UNFOCUS)
		{
			ImeInPlace::detach(this);
		}
		else if (aEvent == FL_KEYBOARD)
		{
			ImeInPlace::attach(this);
		}
		if ((aEvent == FL_KEYDOWN || aEvent == FL_SHORTCUT) && (Fl::event_state() & FL_CTRL) != 0)
		{
			const int key = Fl::event_key();
			if (key == 's' || key == 'S')
			{
				mOwner->SaveFromShortcut();
				return 1;
			}
			if (key == 'r' || key == 'R')
			{
				mOwner->ReloadFromShortcut();
				return 1;
			}
			if (key == 'f' || key == 'F')
			{
				mOwner->FocusFind();
				return 1;
			}
			if (key == 'h' || key == 'H')
			{
				mOwner->FocusReplace();
				return 1;
			}
		}
		if (aEvent == FL_KEYDOWN && Fl::event_key() == FL_F + 3)
		{
			mOwner->FindNextFromShortcut();
			return 1;
		}
		return Fl_Text_Editor::handle(aEvent);
	}

private:
	void DrawCursorScrollMarker()
	{
		if (!mVScrollBar || !mVScrollBar->visible_r() || !buffer())
			return;
		const double minv = mVScrollBar->minimum();
		const double maxv = mVScrollBar->maximum();
		if (maxv <= minv)
			return;
		int visibleLine = 0;
		int absoluteLine = 0;
		if (position_to_line(insert_position(), &visibleLine) &&
			visibleLine >= 0 && visibleLine < mNVisibleLines)
			absoluteLine = mTopLineNum + visibleLine;
		else
			absoluteLine = buffer()->count_lines(0, insert_position()) + 1;
		double ratio = (static_cast<double>(absoluteLine) - minv) / (maxv - minv);
		if (ratio < 0.0)
			ratio = 0.0;
		else if (ratio > 1.0)
			ratio = 1.0;
		const int sy = mVScrollBar->y();
		const int sh = mVScrollBar->h();
		const int markerH = 2;
		if (sh < markerH)
			return;
		int y = sy + static_cast<int>(ratio * (sh - markerH) + 0.5);
		if (y < sy)
			y = sy;
		if (y > sy + sh - markerH)
			y = sy + sh - markerH;
		fl_push_clip(mVScrollBar->x(), sy, mVScrollBar->w(), sh);
		fl_color(DeckColors().text1);
		fl_rectf(mVScrollBar->x(), y, mVScrollBar->w(), markerH);
		fl_pop_clip();
	}

	void DrawLineNumberSeparator()
	{
		if (linenumber_width() <= 0)
			return;
		const int sx = text_area.x - 1;
		if (sx < x())
			return;
		fl_color(DeckColors().text2);
		const int y1 = text_area.y + text_area.h - 1;
		for (int y = text_area.y; y <= y1; y += 6)
		{
			int yEnd = y + 2;
			if (yEnd > y1)
				yEnd = y1;
			fl_yxline(sx, y, yEnd);
		}
	}

	DeckWindow *mOwner;
	bool mCompActive = false;
	int mCompStart = -1;
	int mCompEnd = -1;
	std::string mCompText;
	int mCompCursorBytes = 0;
};

DeckWindow::DeckWindow(Engine &aEngine)
	: Fl_Double_Window(600, 400, "AHKDeck")
	, mEngine(aEngine)
{
	// Pecia frame: no OS caption. DeckTitleBar takes over dragging and the
	// three window buttons; InstallDeckFrame() re-adds WS_THICKFRAME for the
	// resize edges after show().
	border(0);
	ApplyDeckTheme();
	color(DeckColors().background3);
	labelsize(16);

	// Optional startup view for automation (and power users): a comma list of
	// list/editor/chat or "all". The default stays list-only (m019).
	{
		char spec[64] = {0};
		if (GetEnvironmentVariableA("AHKDECK_START_VIEW", spec, sizeof(spec)) > 0)
		{
			const std::string value(spec);
			if (value == "all")
			{
				mShowList = mShowEditor = true;
				mViewFromEnv = true;
			}
			else
			{
				const bool list = value.find("list") != std::string::npos;
				const bool editor = value.find("editor") != std::string::npos;
				if (list || editor)
				{
					mShowList = list;
					mShowEditor = editor;
					mViewFromEnv = true;
				}
			}
		}
	}

	char title[64];
	std::snprintf(title, sizeof(title), "AHKDeck");
	copy_label(title);

	begin();

	mTitleBar = new DeckTitleBar(0, 0, w(), DeckTitleBar::kHeight);
	mTitleBar->setOnMinimize([this]() { iconize(); });
	mTitleBar->setOnToggleMaximize([this]() { ToggleDeckMaximize(this); });
	mTitleBar->setOnClose([this]() { TitleClose(); });

	mMenu = new HoverMenuBar(0, 0, 0, 24);
	mMenu->when(FL_WHEN_RELEASE | FL_WHEN_NOT_CHANGED);
	mMenu->add("文件/新建", FL_CTRL + 'n', OnMenu, (void *)(intptr_t)IdNew);
	mMenu->add("文件/重新扫描", 0, OnMenu, (void *)(intptr_t)IdRescan);
	mMenu->add("文件/打开脚本文件夹", 0, OnMenu, (void *)(intptr_t)IdOpenScriptsFolder);
	mMenu->add("文件/隐藏到托盘", 0, OnMenu, (void *)(intptr_t)IdHideTray);
	mMenu->add("文件/退出", FL_CTRL + 'q', OnMenu, (void *)(intptr_t)IdQuit);
	mMenu->add("视图/脚本列表", FL_CTRL + '1', OnMenu, (void *)(intptr_t)IdViewList, FL_MENU_TOGGLE);
	mMenu->add("视图/编辑器与控制台", FL_CTRL + '2', OnMenu, (void *)(intptr_t)IdViewEditor, FL_MENU_TOGGLE);
	mMenu->add("视图/窗口圆角", 0, OnMenu, (void *)(intptr_t)IdToggleRounded, FL_MENU_TOGGLE);
	mMenu->add("视图/窗口阴影", 0, OnMenu, (void *)(intptr_t)IdToggleShadow, FL_MENU_TOGGLE);
	mMenu->add("脚本/运行", 0, OnMenu, (void *)(intptr_t)IdRun);
	mMenu->add("脚本/停止", 0, OnMenu, (void *)(intptr_t)IdStop);
	mMenu->add("脚本/重启", 0, OnMenu, (void *)(intptr_t)IdRestart);
	mMenu->add("脚本/全部停止", 0, OnMenu, (void *)(intptr_t)IdStopAll);
	mMenu->add("脚本/全部运行", 0, OnMenu, (void *)(intptr_t)IdRunAll);
	mMenu->add("脚本/编辑", 0, OnMenu, (void *)(intptr_t)IdEditSelected);
	mMenu->add("脚本/用记事本打开", 0, OnMenu, (void *)(intptr_t)IdNotepad);
	mMenu->add("设置/快捷键…", 0, OnMenu, (void *)(intptr_t)IdHotkeyEdit);
	mMenu->add("设置/打开日志文件夹", 0, OnMenu, (void *)(intptr_t)IdLogs);
	mMenu->add("设置/打开配置文件", 0, OnMenu, (void *)(intptr_t)IdOpenConfigFile);
	mMenu->add("设置/关于（包含更新）", 0, OnMenu, (void *)(intptr_t)IdAbout);

	// m031: the old toolbar lived here. Every action it carried is now in the
	// menu bar (文件/编辑/脚本), the row's play/pause glyph, or the hotkey cell.
	// Keeping the widgets would only be a second way to say the same thing.

	// m032: the old hotkey strip is gone; the cell itself records the combo.

	mListFrame = new Fl_Box(0, 0, 0, 0);
	mListFrame->box(PaneFrameBox());
	// Create the border overlay before the interactive children. Children are
	// inset by 1px so the border still shows, but a later-created full-pane box
	// would become Fl::belowmouse() and swallow every FL_MOVE, killing the
	// per-cell hover feedback.
	mListBorder = new Fl_Box(0, 0, 0, 0);
	mListBorder->box(BorderOnlyBox());
	mBrowser = new ScriptBrowser(0, 0, 0, 0, this);
	mList = mBrowser;
	mList->type(FL_HOLD_BROWSER);
	// The table is width-fitted; only the vertical bar may ever appear.
	mList->has_scrollbar(Fl_Browser_::VERTICAL);
	mList->scrollbar_size(10);
	mList->textsize(16);
	mList->linespacing(5); // Pecia's list pitch is font 16 + 10 = 26
	mList->when(FL_WHEN_CHANGED);
	mList->callback(OnSelect, this);
	mList->column_char('\t');
	mList->column_widths(mColumns);

	// m024: one header row above the list. Every keystroke rebuilds the view, so
	// the browser only ever contains the rows that match - the actions then need
	// no idea that a filter exists.
	mFilter = new SearchInput(0, 0, 0, 0, true, true);
	mFilter->tooltip("只显示名字包含这些字的脚本（不区分大小写，中文可以）");
	mFilter->when(FL_WHEN_CHANGED | FL_WHEN_NOT_CHANGED);
	mFilter->callback([](Fl_Widget *, void *aSelf) {
		static_cast<DeckWindow *>(aSelf)->RefreshList();
	}, this);

	// m026: the list is a table. The captions sit in the header row but their x
	// comes from the same mColumns array the cells are laid out with, so the two
	// cannot drift apart.
	mHeaderBar = new Fl_Box(0, 0, 0, 0);
	mHeaderBar->box(FL_FLAT_BOX);
	mHeaderBar->color(DeckColors().background3);
	mCaptionStatus = new Fl_Box(0, 0, 0, 0, "运行");
	mCaptionMenu = new Fl_Box(0, 0, 0, 0, "菜单");
	mCaptionName = new Fl_Box(0, 0, 0, 0, "脚本");
	mCaptionHotkey = new Fl_Box(0, 0, 0, 0, "快捷键");
	Fl_Box *captions[] = {mCaptionStatus, mCaptionMenu, mCaptionName, mCaptionHotkey};
	for (Fl_Box *caption : captions)
	{
		caption->box(FL_FLAT_BOX);
		caption->color(DeckColors().background3);
		caption->labelcolor(DeckColors().text1);
		caption->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		caption->labelsize(16);
	}
	mFilterRule = new Fl_Box(0, 0, 0, 0);
	mFilterRule->box(FL_FLAT_BOX);
	mFilterRule->color(DeckColors().background2);
	mListWidgets = {mListFrame, mHeaderBar, mFilterRule, mList, mFilter,
		mCaptionStatus, mCaptionName, mCaptionHotkey, mCaptionMenu, mListBorder};

	// m031: the editor bar keeps only the encoding choice and the file name;
	// save / save-as / save-and-run / reload now live in 文件.
	mEditorFrame = new Fl_Box(0, 0, 0, 0);
	mEditorFrame->box(PaneFrameBox());

	// m045: the old filename/encoding row is gone. This bar is now the compact
	// editor toolbar; the list row already carries the script name/editing state.
	mEditorBar = new Fl_Box(0, 0, 0, 0);
	mEditorBar->box(FL_FLAT_BOX);

	// Find/replace bar (m016): results go to the console, never to a dialog.
	mFindInput = new BlinkInput(0, 0, 0, 0);
	mFindInput->tooltip("查找（Ctrl+F），回车即查找下一个");
	mFindInput->when(FL_WHEN_ENTER_KEY_ALWAYS);
	mFindInput->callback(OnFindNext, this);

	mFindNext = new HoverButton(0, 0, 0, 0, "下一个");
	mFindNext->tooltip("查找下一个（F3）");
	mFindNext->callback(OnFindNext, this);

	mReplaceInput = new BlinkInput(0, 0, 0, 0);
	mReplaceInput->tooltip("替换为（Ctrl+H 进入）");

	mReplaceOne = new HoverButton(0, 0, 0, 0, "替换");
	mReplaceOne->tooltip("替换当前选中处，并查找下一个");
	mReplaceOne->callback(OnReplaceOne, this);

	mReplaceAll = new HoverButton(0, 0, 0, 0, "全部替换");
	mReplaceAll->callback(OnReplaceAll, this);

	mMatchCase = new Fl_Light_Button(0, 0, 0, 0, "Aa");
	mMatchCase->tooltip("区分大小写");

	mEditor = new EditorBox(0, 0, 0, 0, this);
	mEditBuffer = new Fl_Text_Buffer();
	mEditor->buffer(mEditBuffer);
	mEditor->textsize(16);
	// FLTK 1.4.5 ships a line-number margin; no custom gutter needed (m016).
	mEditor->linenumber_width(0);
	mEditor->linenumber_size(16);
	mEditor->linenumber_fgcolor(DeckColors().text1);
	mEditor->linenumber_bgcolor(DeckColors().background1);
	mEditor->linenumber_font(FL_COURIER);
	mEditor->linenumber_align(FL_ALIGN_RIGHT);
	// Pecia's editor caret: SIMPLE_CURSOR is the same 2px, cap-less line the
	// inputs draw, and it must blink off the shared 500 ms phase.
	mEditor->cursor_style(Fl_Text_Display::SIMPLE_CURSOR);
	mEditor->cursor_color(DeckColors().text1);
	CaretBlink::attach(mEditor);
	mEditBuffer->add_modify_callback(OnBufferModified, this);

	mConsole = new DeckDisplay(0, 0, 0, 0);
	mBuffer = new Fl_Text_Buffer();
	mConsole->buffer(mBuffer);
	mConsole->textsize(16);
	mConsole->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);

	mEditorBorder = new Fl_Box(0, 0, 0, 0);
	mEditorBorder->box(BorderOnlyBox());

	// Pecia's Lua console header: "输出" on the left, actions right-aligned.
	// Created after the editor and its border overlay so the bar and buttons are
	// the topmost children - otherwise the full-pane border box becomes
	// Fl::belowmouse() and the HoverButton never sees FL_ENTER.
	mConsoleSave = new HoverButton(0, 0, 0, kConsoleButtonH, "保存");
	mConsoleSave->tooltip("保存当前脚本到本地");
	mConsoleSave->callback(OnSave, this);
	mConsoleClose = new HoverButton(0, 0, 0, kConsoleButtonH, "关闭");
	mConsoleClose->tooltip("关闭当前脚本（有未保存修改会先保存）");
	mConsoleClose->callback(OnCloseScript, this);
	mConsoleClear = new HoverButton(0, 0, 0, kConsoleButtonH, "清空");
	mConsoleClear->tooltip("清空输出");
	mConsoleClear->callback(OnClear, this);
	mConsoleRun = new HoverButton(0, 0, 0, kConsoleButtonH, "运行");
	mConsoleRun->tooltip("先保存到本地，再运行当前编辑器中的脚本");
	mConsoleRun->callback(OnRunOrStop, this);

	mStatus = new Fl_Box(0, 0, 0, 0);
	mStatus->box(FL_FLAT_BOX);
	mStatus->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	mStatus->labelsize(14);


	mEditorWidgets = {mEditorFrame, mEditorBar, mEditorBorder,
		mConsoleSave, mConsoleClose, mConsoleClear, mConsoleRun,
		mFindInput, mFindNext, mReplaceInput, mReplaceOne, mReplaceAll, mMatchCase,
		mEditor, mConsole};

	end();

	resizable(this);
	size_range(600, 400);

	// Window geometry (m017/m050): one settings.json, with a legacy ui.ini
	// fallback for existing installs and the UI gates.
	mSettingsPath = DefaultConfigPath();
	mConfigMissing = GetFileAttributesW(mSettingsPath.c_str()) == INVALID_FILE_ATTRIBUTES;
	mConfigWritable = mConfigMissing || IsLegacyIniPath(mSettingsPath);
	mConfigRoot = Json::MakeObject();
	{
		Json loaded;
		std::string configError;
		if (!mConfigMissing && ReadJsonConfig(mSettingsPath, loaded, configError))
		{
			mConfigRoot = std::move(loaded);
			mConfigWritable = true;
		}

		WindowGeometry geometry;
		const Json &ui = mConfigRoot["ui"];
		if (ui.IsObject())
		{
			geometry.x = ui["x"].AsInt(0);
			geometry.y = ui["y"].AsInt(0);
			geometry.w = ui["w"].AsInt(0);
			geometry.h = ui["h"].AsInt(0);
			geometry.rounded = ui["rounded"].AsBool(true);
			geometry.shadow = ui["shadow"].AsBool(true);
			geometry.valid = geometry.w > 0 && geometry.h > 0;
			// m053: the view toggles are part of the config too. The automation
			// env var wins over the saved view.
			if (!mViewFromEnv)
			{
				if (ui.Has("list"))
					mShowList = ui["list"].AsBool(mShowList);
				if (ui.Has("editor"))
					mShowEditor = ui["editor"].AsBool(mShowEditor);
				if (ui.Has("chat"))
				if (!mShowList && !mShowEditor)
					mShowList = true;
			}
		}
		else
		{
			std::string settingsError;
			LoadUiSettings(LegacyUiConfigPath(), geometry, settingsError);
		}
		mRoundedCorners = geometry.rounded;
		mWindowShadow = geometry.shadow;
		if (geometry.valid)
		{
			if (geometry.w < 600)
				geometry.w = 600;
			if (geometry.h < 400)
				geometry.h = 400;
			int sx = 0, sy = 0, sw = 0, sh = 0;
			const int screen = Fl::screen_num(geometry.x, geometry.y);
			Fl::screen_work_area(sx, sy, sw, sh, screen < 0 ? 0 : screen);
			if (screen < 0)
			{
				geometry.x = sx + (sw - geometry.w) / 2;
				geometry.y = sy + (sh - geometry.h) / 2;
			}
			resize(geometry.x, geometry.y, geometry.w, geometry.h);
			resize(geometry.x, geometry.y, geometry.w, geometry.h);
		}
	}

	SyncEditorActions();

	std::string error;
	if (!mEngine.Load(error))
	{
		mReady = false;
		Append("[engine] " + error);
	}
	else
	{
		mReady = true;
		Append("[engine] kernel ready (in-process, no second process)");
	}


	// m064: the MCP tool thread reads the editing area through this snapshot, so
	// run_editor can never see text older than what the user is looking at.
	mMcpTools.SetEditorSupplier([this] { return EditorSnapshot(); });
	RefreshEditorSnapshot();


	ScanScripts();
	LoadHotkeyFile();
	RefreshList();
	Append("[ui] 双击行＝启/停切换；右键行＝编辑 / 重启 / 脚本菜单 / 快捷键；报错只进本控制台。");
	SyncViewMenu();
	ApplyWidgetTheme();
	ApplyView();

	if (mConfigDirty)
	{
		std::string saveError;
		if (!SaveSettings(&saveError))
			Append("[settings] 无法写入配置: " + saveError);
		mConfigDirty = false;
	}
	mConfigMissing = false;

	// m023: the tray and the global hotkeys are Win32-side and best-effort. If the
	// tray cannot be installed the close box goes back to meaning "exit" - a window
	// nobody can reach again is worse than no tray.
	UpdateKernelSummary();
	{
		std::string hookError;
		if (!mHooks.Create(GetModuleHandleW(nullptr), hookError))
		{
			Append("[deck] 控制窗口创建失败：" + hookError + "（全局快捷键与托盘均不可用）");
		}
		else
		{
			std::string trayError;
			if (!mHooks.AddTrayIcon("AHKDeck", trayError))
				Append("[deck] " + trayError + "：关闭按钮按原样退出，不隐藏");
			RebindHotkeys();
			UpdateTrayTip();
		}
	}

	Fl::add_timeout(0.1, OnTick, this);
}

DeckWindow::~DeckWindow()
{
	delete mShortcutDialog;
	mShortcutDialog = nullptr;
	delete mNameDialog;
	mNameDialog = nullptr;
	Fl::remove_timeout(OnTick, this);
	windowShadow::detach(this);
	mHooks.Destroy(); // the tray icon must be gone before the process is
	// FLTK 1.4.5 displays do not own their buffers, and group teardown happens
	// in the Fl_Window base destructor - release widget + buffer explicitly.
	CaretBlink::detach(mEditor);
	delete mEditor;
	mEditor = nullptr;
	delete mEditBuffer;
	mEditBuffer = nullptr;
	delete mConsole;
	mConsole = nullptr;
	delete mBuffer;
	mBuffer = nullptr;
}

int DeckWindow::ClientWidth() const
{
	return DeckClientWidth(const_cast<DeckWindow *>(this));
}

int DeckWindow::ClientHeight() const
{
	return DeckClientHeight(const_cast<DeckWindow *>(this));
}

void DeckWindow::TitleClose()
{
	if (mHooks.HasTrayIcon())
	{
		HideToTray();
		return;
	}
	RequestQuit();
	hide();
}

int DeckWindow::handle(int aEvent)
{
	// The close box hides the deck to the tray (v1.3 did the same: OnClose ->
	// g.Hide, scripts still running). Without a tray there is no way back in, so
	// the box keeps its old meaning there - an unreachable process is worse.
	if (aEvent == FL_CLOSE)
	{
		if (mHooks.HasTrayIcon())
		{
			HideToTray();
			return 1;
		}
		RequestQuit();
		hide();
		return 1;
	}

	// F2 = edit the selected script; Ctrl+Shift+M = ask the selected script to pop
	// its own menu. Both at the window level so they work regardless of which child
	// holds keyboard focus, and because a gate can post a key but not a menu (m022).
	if (aEvent == FL_KEYDOWN && mList)
	{
		if (Fl::event_key() == FL_F + 2)
		{
			const int index = SelectedIndex();
			if (index >= 0)
			{
				EditRow(index);
				return 1;
			}
		}
		if ((Fl::event_key() == 'm' || Fl::event_key() == 'M')
			&& (Fl::event_ctrl() != 0) && (Fl::event_shift() != 0))
		{
			const int index = SelectedIndex();
			if (index >= 0)
			{
				OpenScriptMenu(index);
				return 1;
			}
		}

		// m031: the toolbar is gone, so the actions it carried get keyboard
		// routes. F5/F6/F7/F8 are the media-control pair plus restart/hotkey.
		const int key = Fl::event_key();
		const bool ctrl = Fl::event_ctrl() != 0;
		const bool shift = Fl::event_shift() != 0;
		if (key == FL_F + 5)
		{
			const int index = SelectedIndex();
			if (index >= 0)
			{
				ToggleRow(index);
				return 1;
			}
		}
		if (key == FL_F + 6)
		{
			const int index = SelectedIndex();
			if (index >= 0)
			{
				RestartRow(index);
				return 1;
			}
		}
		if (key == FL_F + 7)
		{
			StopEverything();
			return 1;
		}
		if (key == FL_F + 8)
		{
			BeginHotkeyEdit(SelectedIndex());
			return 1;
		}
		if (key == FL_F + 9)
		{
			// F9, not Ctrl+Shift+C: the text editor consumes Ctrl+C itself.
			OnCopy(nullptr, this);
			return 1;
		}
		if (key == FL_F + 11)
		{
			OpenLogs();
			return 1;
		}
		if (ctrl && shift && (key == 'r' || key == 'R'))
		{
			RefreshFromDisk();
			return 1;
		}
		if (ctrl && shift && (key == 'l' || key == 'L'))
		{
			OpenLogs();
			return 1;
		}
		if (ctrl && shift && (key == 'c' || key == 'C'))
		{
			OnCopy(nullptr, this);
			return 1;
		}
		if (ctrl && !shift && (key == 'l' || key == 'L'))
		{
			mBuffer->text("");
			return 1;
		}
	}
	return Fl_Double_Window::handle(aEvent);
}

void DeckWindow::resize(int x, int y, int w, int h)
{
	Fl_Double_Window::resize(x, y, w, h);
	Layout();
	if (mFrameInstalled)
		SyncDeckFrame(this);
}

void DeckWindow::draw()
{
	Fl_Double_Window::draw();

	// The 1px top run and the inner half of the arcs are one element with the
	// dialogs; the kit owns how they are drawn.
	if (mFrameInstalled)
		PaintDeckClientBorder(this);
}

void DeckWindow::Layout()
{
	const int cw = ClientWidth();
	const int ch = ClientHeight();
	const int pad = 8;
	const int menuH = 24;
	const int titleH = DeckTitleBar::kHeight;
	const int top = titleH + menuH + 2; // caption + menu (no toolbar, no strip)
	const int statusH = 24;

	if (mTitleBar)
		mTitleBar->resize(0, 0, cw, titleH);
	mMenu->resize(0, titleH, cw, menuH);


	// The status bar hugs the pane: only the same 2px the pane has above it.
	const int paneH = ch - top - statusH - 2;
	if (paneH <= 0)
		return;

	int visible = 0;
	if (mShowList) ++visible;
	if (mShowEditor) ++visible;
	if (visible == 0)
		return;

	// Equal columns: the visible count splits the usable width, with the last
	// column taking the rounding remainder so the three edges line up.
	const int usable = (cw - 2 * pad) - 6 * (visible - 1);
	const int each = usable / visible;
	int column = 0;
	auto nextWidth = [&]() {
		++column;
		return column == visible ? usable - each * (visible - 1) : each;
	};
	int x = pad;
	if (mShowList)
	{
		const int width = nextWidth();
		LayoutListPane(x, top, width, paneH);
		x += width + 6;
	}
	if (mShowEditor)
	{
		const int width = nextWidth();
		LayoutEditorPane(x, top, width, paneH);
		x += width + 6;
	}


	mStatus->resize(pad, ch - statusH + 1, cw - 2 * pad, statusH - 2);
}

// The list pane is a header row (filter + column captions) and the browser under
// it. Keeping the header inside the pane means the browser's own geometry stays
// uniform, so a gate can still address a row by (pane top + header + n * row).
void DeckWindow::LayoutListPane(int x, int y, int w, int h)
{
	const int rowH = kRowHeight;
	const int headerH = kRowHeight;
	// m033: header first, browser, search at the bottom of the pane. The search
	// used to share the header row and covered the 名字 caption.

	// Edge-to-edge: the header band, rows and search fill the frame interior;
	// only the first row keeps a white 8px breathing room below the header.
	const int topGap = 8;
	const int contentW = w - 2;
	// The browser paints its vertical scrollbar over the last 10px of the
	// content. Keep the table columns out of that strip even when the bar is
	// currently hidden, otherwise long lists cover the hotkey/menu cells.
	const int scrollbarW = 10;
	const int columnW = contentW - scrollbarW;
	const int fixed = mColumns[0] + mColumns[1] + mColumns[3];
	mColumns[2] = columnW - fixed;
	if (mColumns[2] < 60)
		mColumns[2] = 60;

	const int textX = x + 3;
	const int headerY = y + 2;
	const int frameH = (y + h) - headerY;
	mListFrame->resize(x, headerY, w, frameH > 0 ? frameH : 0);
	mListBorder->resize(x, headerY, w, frameH > 0 ? frameH : 0);
	mHeaderBar->resize(x + 1, headerY + 1, contentW, headerH);
	mCaptionStatus->resize(textX, headerY + 1, mColumns[0], headerH);
	mCaptionMenu->resize(textX + mColumns[0], headerY + 1, mColumns[1], headerH);
	mCaptionName->resize(textX + mColumns[0] + mColumns[1], headerY + 1, mColumns[2] - 4, headerH);
	mCaptionHotkey->resize(textX + mColumns[0] + mColumns[1] + mColumns[2], headerY + 1, mColumns[3] - 4, headerH);

	const int listY = headerY + 1 + headerH;
	const int browserY = listY + topGap;
	const int searchY = y + h - rowH;
	const int listH = (searchY - 1) - browserY;
	mList->resize(x + 1, browserY, contentW, listH > 0 ? listH : 0);
	mFilterRule->resize(x + 1, searchY - 1, contentW, 1);
	mFilter->resize(x + 1, searchY, contentW, rowH);
}

void DeckWindow::LayoutEditorPane(int x, int y, int w, int h)
{
	mEditorFrame->resize(x, y + 2, w, h - 2);
	mEditorBorder->resize(x, y + 2, w, h - 2);
	const int cx = x + 1;
	const int cy = y + 3;
	const int cw = w - 2;
	const int ch = h - 4;
	// Find/replace row sits directly under the editor. The three-column editor
	// column is narrower than the old find row: hide it (Find/Replace stay on
	// the 编辑 menu) and let the editor/console use the space.
	mFindInput->hide();
	mFindNext->hide();
	mReplaceInput->hide();
	mReplaceOne->hide();
	mReplaceAll->hide();
	mMatchCase->hide();

	// m048: no top toolbar. Code is on top, the middle band holds the four
	// actions, and the log is a quarter of the column at the bottom - the same
	// shape as the AI pane.
	const int bodyTop = cy;
	const int bodyH = ch;
	int outputH = bodyH * kConsoleOutputPercent / 100;
	if (outputH < kConsoleOutputMinH)
		outputH = kConsoleOutputMinH;
	if (outputH > bodyH - kPaneBarH - kEditorMinH)
		outputH = bodyH - kPaneBarH - kEditorMinH;
	if (outputH < 0)
		outputH = 0;
	const int outputY = bodyTop + bodyH - outputH;
	const int barY = outputY - kPaneBarH;
	const int editorH = barY - bodyTop;
	mEditor->resize(cx, bodyTop, cw, editorH > 0 ? editorH : 0);
	mEditorBar->resize(cx, barY, cw, kPaneBarH);
	mConsole->resize(cx, outputY, cw, outputH);

	// Four actions live in the middle band: 保存 / 关闭 / 清空 / 运行. The font
	// and padding step down so the row still fits the narrow 3-column case.
	int barFont = 16;
	int barPad = kConsoleButtonPadX;
	int barGap = kConsoleButtonGap;
	if (cw < 360)
	{
		barFont = 14;
		barPad = 8;
		barGap = 8;
	}
	if (cw < 220)
	{
		barFont = 12;
		barPad = 6;
		barGap = 6;
	}
	FitConsoleButtonRow({mConsoleSave, mConsoleClose, mConsoleClear, mConsoleRun},
		cx + cw, barY + kPaneBarH / 2, 8, barGap, barPad, 0, barFont);
}


namespace
{
// m027: a long-running deck must not grow without bound. Keep the last ~512 KB
// and cut on a line boundary; this is the console, not an archive (the JSONL on
// disk already is).
void TrimTextBuffer(Fl_Text_Buffer *aBuffer)
{
	const int maxBytes = 512 * 1024;
	if (!aBuffer || aBuffer->length() <= maxBytes)
		return;
	const int target = aBuffer->length() - maxBytes / 2;
	char *text = aBuffer->text();
	if (!text)
		return;
	int cut = 0;
	for (int i = target; i < aBuffer->length(); ++i)
	{
		if (text[i] == '\n')
		{
			cut = i + 1;
			break;
		}
	}
	std::free(text);
	if (cut > 0)
		aBuffer->remove(0, cut);
}
} // namespace

void DeckWindow::Append(const std::string &aLine)
{
	// m067: the old gate here was "a file must be open in the editor", which meant
	// a script started straight from the list printed NOTHING - no started/exited
	// receipt, no kernel error, not even the engine's own lines. That contradicted
	// requirement 3 (报错进控制台) for the most common gesture.
	// The console is a column: when it is shown it carries the deck's own lines
	// plus the lines of whichever script owns it (AppendIfTest/AppendRecord do
	// that routing). When only the list is shown there is no console to write to;
	// the session JSONL still holds everything.
	if (!mShowEditor)
		return;
	mBuffer->append(aLine.c_str());
	mBuffer->append("\n");
	TrimTextBuffer(mBuffer);
	mConsole->insert_position(mBuffer->length());
	mConsole->show_insert_position();
}

void DeckWindow::AppendIfTest(const std::wstring &aPath, const std::string &aLine)
{
	if (!mConsolePath.empty() && SamePathI(mConsolePath, aPath))
		Append(aLine);
}

std::wstring DeckWindow::ActiveScriptPath() const
{
	// The editor's file is the ONLY owner of the console (m076). Row focus and
	// selection are navigation; running some other script from the list must
	// never switch it - that script's output stays in its own session JSONL.
	return mEditPath;
}

void DeckWindow::SyncConsoleSelection()
{
	const std::wstring path = ActiveScriptPath();
	// Only a *change* of the editor's file re-claims the console. Comparing
	// against the current owner every tick would silently take it back from a
	// script the user just ran by hand.
	if (mConsoleEditorPath.empty() ? path.empty() : SamePathI(mConsoleEditorPath, path))
		return;
	mConsoleEditorPath = path;
	if (mConsolePath.empty() ? path.empty() : SamePathI(mConsolePath, path))
		return;
	mConsolePath = path;
	// One console, one test script: changing the selected script starts a
	// fresh view. Previous output stays in the session JSONL.
	mBuffer->text("");
}

void DeckWindow::OpenSessionLog(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;
	const ScriptRow &row = mScripts[(size_t)aIndex];
	Session session;
	if (!row.session || !mEngine.GetSession(row.session, session) || session.logPath.empty())
	{
		Append("[ui] " + row.name + " 还没有运行日志");
		return;
	}
	const std::wstring root = mEngine.LogRoot();
	if (root.empty())
	{
		Append("[ui] 日志目录尚未建立（引擎未加载）");
		return;
	}
	std::string file = session.logPath;
	const size_t slash = file.find_last_of("\\/");
	if (slash != std::string::npos)
		file = file.substr(slash + 1);
	const std::wstring full = root + L"\\" + Wide(file);
	const std::wstring args = L"\"" + full + L"\"";
	ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void DeckWindow::AppendRecord(const ErrorRecord &aRecord)
{
	// m075: one short line per record. The old shape repeated the session id,
	// the English severity/phase, the FULL path and then a 35-space-indented
	// "Specifically:" line - three lines of chrome for a one-line error.
	// Now: "[time] 出错（第 1 行）：<kernel text>", with the file named only when
	// the record comes from another file (#Include), never a full path.
	const std::wstring path = [&]() {
		auto it = mSessionPath.find(aRecord.session);
		return it == mSessionPath.end() ? std::wstring() : it->second;
	}();
	const bool sameOwner = !path.empty() && !mConsolePath.empty() && SamePathI(path, mConsolePath);
	const std::string name = NameOfSession(aRecord.session);

	std::string file = aRecord.file;
	const size_t slash = file.find_last_of("\\/");
	if (slash != std::string::npos)
		file = file.substr(slash + 1);
	const std::string ownerLeaf = path.empty() ? std::string() : Utf8(LeafOf(path));
	const bool foreignFile = !file.empty() && file != ownerLeaf;

	std::string where;
	if (aRecord.line > 0)
	{
		where = foreignFile ? file + ":" + std::to_string(aRecord.line)
			: "第 " + std::to_string(aRecord.line) + " 行";
	}
	else if (foreignFile)
	{
		where = file;
	}

	std::string line = "[" + ShortTime(aRecord.ts) + "] ";
	if (!sameOwner && !name.empty())
		line += name + " ";
	line += (aRecord.kind == "warn") ? "警告" : "出错";
	if (!where.empty())
		line += "（" + where + "）";
	line += "：" + aRecord.message;
	if (!aRecord.extra.empty())
		line += " Specifically: " + aRecord.extra;

	// m076: the console shows the edited script's records and nothing else.
	// (m067 had made an unclaimed console swallow every record; the user's rule
	// is that this pane is for correcting the script open in the editor, so a
	// record from another script belongs to its JSONL, not here.)
	if (!mConsolePath.empty() && !path.empty() && SamePathI(path, mConsolePath))
		Append(line);
}

void DeckWindow::ScanScripts()
{
	// m051: the list is exactly exe\Scripts\*.ahk. Rebuild from disk so a file
	// added/removed outside the app disappears on the next scan, but keep the
	// live session ids for scripts that are still running.
	std::map<std::wstring, DWORD> sessions;
	for (const auto &row : mScripts)
	{
		if (row.session)
			sessions[row.path] = row.session;
	}
	mScripts.clear();
	mLastLabels.clear();

	WIN32_FIND_DATAW fd;
	const std::wstring pattern = ExeDirectory() + L"\\Scripts\\*.ahk";
	HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		AddScript(ExeDirectory() + L"\\Scripts\\" + fd.cFileName);
	} while (FindNextFileW(find, &fd));
	FindClose(find);

	for (auto &row : mScripts)
	{
		const auto it = sessions.find(row.path);
		if (it != sessions.end())
			row.session = it->second;
	}
}

bool DeckWindow::AddScript(const std::wstring &aPath)
{
	if (FindScript(aPath) >= 0)
		return false;

	ScriptRow row;
	row.path = aPath;
	row.name = Utf8(LeafOf(aPath));
	mScripts.push_back(std::move(row));

	std::sort(mScripts.begin(), mScripts.end(),
		[](const ScriptRow &a, const ScriptRow &b)
		{
			return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
		});

	// A sorted insert invalidates the previous selection index; the browser is
	// refreshed by the caller, which re-selects by remembering the id.
	return true;
}

int DeckWindow::FindScript(const std::wstring &aPath) const
{
	for (size_t i = 0; i < mScripts.size(); ++i)
	{
		if (SamePathI(mScripts[i].path, aPath))
			return (int)i;
	}
	return -1;
}

int DeckWindow::SelectedIndex() const
{
	// m037: the browser no longer selects rows; the last name click owns the
	// focus. The fallback keeps keyboard-only/menu paths working.
	if (mBrowser)
	{
		const int line = mBrowser->FocusLine();
		if (line > 0)
			return IndexOfLine(line);
	}
	return IndexOfLine(mList ? mList->value() : 0);
}

int DeckWindow::IndexOfLine(int aLine) const
{
	if (aLine < 1 || (size_t)aLine > mVisible.size())
		return -1;
	return mVisible[(size_t)aLine - 1];
}

int DeckWindow::LineOfIndex(int aIndex) const
{
	for (size_t i = 0; i < mVisible.size(); ++i)
	{
		if (mVisible[i] == aIndex)
			return (int)i + 1;
	}
	return 0; // filtered out: there is no line to point at
}

bool DeckWindow::MatchesFilter(size_t aIndex) const
{
	const char *text = mFilter ? mFilter->value() : nullptr;
	if (!text || !*text)
		return true;
	// Case- and charset-folded substring on the wide form: the list names are
	// UTF-8 and Chinese script names are the common case, so byte-wise ASCII
	// folding would silently miss them.
	std::wstring needle = Wide(text);
	std::wstring hay = Wide(mScripts[aIndex].name);
	if (needle.empty() || hay.empty())
		return needle.empty();
	CharLowerW(&needle[0]);
	CharLowerW(&hay[0]);
	return hay.find(needle) != std::wstring::npos;
}

DeckWindow::RowLight DeckWindow::LightOf(int aIndex) const
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return RowLight::Idle;
	const ScriptRow &row = mScripts[(size_t)aIndex];
	if (!row.session)
		return RowLight::Idle;
	Session session;
	if (!mEngine.GetSession(row.session, session))
		return RowLight::Idle;
	if (session.state != SessionState::Finished)
		return RowLight::Running;
	// m077: a finished session is not a state of this button. The grey tick made
	// a row look like it was in some special mode, so users stopped clicking it;
	// the result moved to the row's left edge (LastRunFailed) and the cell went
	// back to exactly two glyphs.
	return RowLight::Idle;
}

// The last run of this row ended badly: non-zero exit code or a fatal kernel
// record. Drawn as a 2px red strip on the row's left edge - a row-level mark,
// not a glyph inside the clickable cell (m077).
bool DeckWindow::LastRunFailed(int aIndex) const
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return false;
	const ScriptRow &row = mScripts[(size_t)aIndex];
	if (!row.session)
		return false;
	Session session;
	if (!mEngine.GetSession(row.session, session) || session.state != SessionState::Finished)
		return false;
	return session.fatal || OfficialRc(session) != 0;
}

bool DeckWindow::IsRunning(const ScriptRow &aRow) const
{
	if (!aRow.session)
		return false;
	Session session;
	return mEngine.GetSession(aRow.session, session) && session.state != SessionState::Finished;
}

// The view is the store with the rows that fail the filter taken out. The store
// is kept in name order (AddScript sorts it), so there is one order and no mode
// to remember.

void DeckWindow::RebuildView()
{
	std::vector<int> indices;
	indices.reserve(mScripts.size());
	for (size_t i = 0; i < mScripts.size(); ++i)
	{
		if (MatchesFilter(i))
			indices.push_back((int)i);
	}

	mVisible = std::move(indices);
}

std::string DeckWindow::NameOfSession(DWORD aSessionId) const
{
	auto it = mSessionName.find(aSessionId);
	if (it != mSessionName.end())
		return it->second;
	if (aSessionId)
		return "#" + std::to_string(aSessionId);
	return "unattributed";
}


bool DeckWindow::SaveSettings(std::string *aError)
{
	if (mSettingsPath.empty())
	{
		if (aError)
			*aError = "settings path is empty";
		return false;
	}
	if (!mConfigWritable)
	{
		if (aError)
			*aError = "配置文件解析失败，未覆盖原文件";
		return false;
	}

	Json ui = JsonObjectMember(mConfigRoot, "ui");
	ui.Set("x", Json((double)x()));
	ui.Set("y", Json((double)y()));
	ui.Set("w", Json((double)ClientWidth()));
	ui.Set("h", Json((double)ClientHeight()));
	ui.Set("rounded", Json(mRoundedCorners));
	ui.Set("shadow", Json(mWindowShadow));
	ui.Set("list", Json(mShowList));
	ui.Set("editor", Json(mShowEditor));
	mConfigRoot.Set("ui", ui);

	std::string error;
	if (!WriteJsonConfig(mSettingsPath, mConfigRoot, error))
	{
		if (aError)
			*aError = error;
		return false;
	}
	if (aError)
		aError->clear();
	return true;
}

// One place builds the status line, so a filter never leaves "N script(s)"
// claiming something the list is not showing.
void DeckWindow::UpdateStatus()
{
	size_t running = 0;
	for (const auto &session : CachedSessions())
	{
		if (session.state != SessionState::Finished)
			++running;
	}

	std::string engine = mKernelSummary.empty()
		? (mReady ? std::string("就绪") : std::string("未载入"))
		: (mKernelSummary + (mReady ? std::string() : std::string(" (未载入)")));
	std::string text = "引擎: " + engine;
	text += "   |   脚本: " + std::to_string((int)mScripts.size());
	if (mVisible.size() != mScripts.size())
		text += " (显示 " + std::to_string((int)mVisible.size()) + ")";
	text += "   |   运行中: " + std::to_string((int)running);
	text += "   |   " + (mEditPath.empty() ? std::string("文件: 未打开") : std::string("文件: 已打开"));
	text += "   |   日志";
	if (text == mLastStatus)
		return;
	mLastStatus = text;
	mStatus->copy_label(text.c_str());
}

void DeckWindow::RefreshList()
{
	RebuildView();

	std::vector<std::string> labels;
	labels.reserve(mVisible.size());

	for (const int index : mVisible)
	{
		const ScriptRow &row = mScripts[(size_t)index];
		// Four columns, separated by the browser's column character: the state
		// Status and gear are drawn, not written: an empty first field for the
		// traffic light, an empty second for the gear button.
		std::string label = "\t\t";
		label += row.name;
		label += "\t";
		const std::string binding = BindingOf(row);
		label += HotkeyDisplayText(binding);
		labels.push_back(std::move(label));
	}

	// A change of state can leave the text identical (two idle rows), so the view
	// itself is part of the "nothing changed" test. The glyphs/status still need
	// a repaint: starting or stopping a script changes no label text.
	if (labels == mLastLabels && mVisible == mLastVisible)
	{
		if (mList) mList->redraw();
		UpdateStatus();
		SyncEditorRunButton();
		return;
	}

	const int selected = SelectedIndex();
	int top = mList->topline();
	mUpdatingList = true;
	mList->clear();
	for (const auto &label : labels)
		mList->add(label.c_str());
	if (mBrowser)
		mBrowser->SetFocusLine(selected >= 0 ? LineOfIndex(selected) : 0);
	if (top > mList->size())
		top = 1;
	if (top >= 1)
		mList->topline(top);
	mUpdatingList = false;
	mLastLabels = std::move(labels);
	mLastVisible = mVisible;
	UpdateStatus();
	SyncEditorRunButton();
}

void DeckWindow::DrainRecords()
{
	ErrorRecord rec;
	while (mEngine.PopRecord(rec, 0))
		AppendRecord(rec);
}

const std::vector<Session> &DeckWindow::CachedSessions() const
{
	if (!mSessionsCacheValid)
	{
		mSessionsCache = mEngine.Sessions();
		mSessionsCacheValid = true;
	}
	return mSessionsCache;
}

void DeckWindow::TrackSessions()
{
	SyncConsoleSelection();
	bool changed = false;
	for (const auto &session : CachedSessions())
	{
		mSessionPath[session.id] = session.path;
		auto seen = mSeenState.find(session.id);
		if (seen == mSeenState.end())
		{
			mSeenState[session.id] = session.state;
			mSessionName[session.id] = session.name;
			AppendIfTest(session.path, "[" + UiClock() + "] " + mSessionName[session.id] + " 运行");
			changed = true;
		}
		else if (seen->second != session.state)
		{
			seen->second = session.state;
			changed = true;
			if (session.state == SessionState::Stopping)
				AppendIfTest(session.path, "[" + UiClock() + "] 停止中");
		}

		// The engine has no lifecycle event stream (only kernel records), so the
		// UI's own state table is what notices a reaped thread. Print that once.
		if (session.state == SessionState::Finished && !mExitReported.count(session.id))
		{
			mExitReported[session.id] = true;
			changed = true;
			const DWORD rc = OfficialRc(session);
			AppendIfTest(session.path, "[" + UiClock() + "] 结束"
				+ (rc != 0 ? " rc=" + std::to_string((unsigned long)rc) : std::string()));
		}
	}
	if (changed)
	{
		if (mList) mList->redraw();
		UpdateStatus();
	}
	// The editor's run/stop button follows the same session state as the list.
	SyncEditorRunButton();
}

void DeckWindow::RefreshFromDisk()
{
	ScanScripts();
	// hotkeys.txt may have been edited by hand since the last scan: re-read it,
	// then re-register so a changed spelling takes effect without a restart.
	LoadHotkeyFile();
	RefreshList();
	RebindHotkeys();
	UpdateTrayTip();
	Append("[" + UiClock() + "] 重新扫描（" + std::to_string(mScripts.size()) + " 个脚本）");
}

DWORD DeckWindow::StartPath(const std::wstring &aPath)
{
	if (!mReady)
	{
		Append("[ui] 引擎未就绪，无法运行");
		return 0;
	}

	// m076: the console belongs to the script open in the editor and to nothing
	// else. Running another script from the list must not steal it - that other
	// script's receipts and kernel records still go to its session JSONL, and the
	// row light shows its result.
	SyncConsoleSelection();
	const bool testScript = !mConsolePath.empty() && SamePathI(aPath, mConsolePath);
	if (testScript)
		mBuffer->text("");

	for (const auto &session : mEngine.Sessions())
	{
		if (session.state != SessionState::Finished && SamePathI(session.path, aPath))
		{
			Append("[ui] " + Utf8(LeafOf(aPath)) + " 已在运行");
			return 0;
		}
	}

	// The editor is only a view of the on-disk script: never run a version the
	// user has edited but not saved. This covers every entry point (list play
	// glyph, hotkeys, restart, console 运行), not just SaveAndRun.
	if (mEditDirty && !mEditPath.empty() && SamePathI(mEditPath, aPath))
	{
		if (!SaveEditorTo(mEditPath))
		{
			Append("[ui] 保存失败，未运行 " + Utf8(LeafOf(aPath)));
			return 0;
		}
	}

	std::string error;
	DWORD id = mEngine.StartSession(aPath, std::vector<std::wstring>(), error);
	InvalidateSessions();
	if (!id)
	{
		Append("[ui] 启动失败: " + error);
		return 0;
	}
	if (!error.empty())
		Append("[ui] " + error);

	if (FindScript(aPath) < 0)
	{
		AddScript(aPath);
		RefreshList();
	}
	const int index = FindScript(aPath);
	if (index >= 0)
		mScripts[(size_t)index].session = id;

	mSessionName[id] = Utf8(LeafOf(aPath));
	mSessionPath[id] = aPath;
	mSeenState[id] = SessionState::Running;
	mExitReported.erase(id);
	AppendIfTest(aPath, "[" + UiClock() + "] " + Utf8(LeafOf(aPath)) + " 运行");
	RefreshList();
	return id;
}

void DeckWindow::RunSelected()
{
	int index = SelectedIndex();
	if (index < 0)
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	StartPath(mScripts[(size_t)index].path);
}

void DeckWindow::StopSelected()
{
	int index = SelectedIndex();
	if (index < 0)
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	ScriptRow &row = mScripts[(size_t)index];
	if (!row.session)
	{
		Append("[ui] " + row.name + " 还没有运行过");
		return;
	}
	if (!mEngine.StopSession(row.session))
	{
		Append("[ui] " + row.name + " 已经结束或无法停止");
		return;
	}
	InvalidateSessions();
	AppendIfTest(row.path, "[" + UiClock() + "] 停止中");
	RefreshList();
}

void DeckWindow::StopEverything()
{
	size_t signalled = mEngine.StopAll();
	InvalidateSessions();
	Append("[" + UiClock() + "] 全部停止（" + std::to_string(signalled) + " 个）");
	RefreshList();
}

void DeckWindow::RunEverything()
{
	if (!mReady)
	{
		Append("[ui] 引擎未就绪，无法运行");
		return;
	}
	std::vector<std::wstring> paths;
	for (const auto &row : mScripts)
	{
		if (!IsRunning(row))
			paths.push_back(row.path);
	}
	size_t started = 0;
	for (const auto &path : paths)
	{
		if (StartPath(path))
			++started;
	}
	Append("[" + UiClock() + "] 全部运行（" + std::to_string(started) + "/"
		+ std::to_string(paths.size()) + "）");
}

void DeckWindow::OpenScriptsFolder()
{
	const std::wstring directory = ExeDirectory() + L"\\Scripts";
	HINSTANCE result = ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr,
		SW_SHOWNORMAL);
	if ((INT_PTR)result <= 32)
		Append("[ui] 无法打开脚本文件夹");
	else
		Append("[ui] 打开脚本文件夹: " + Utf8(directory));
}

void DeckWindow::EditSelected()
{
	const int index = SelectedIndex();
	if (index < 0)
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	EditRow(index);
}

bool DeckWindow::EditingScriptRunning(DWORD *aSession) const
{
	if (mEditPath.empty())
		return false;
	const int index = FindScript(mEditPath);
	if (index < 0)
		return false;
	const ScriptRow &row = mScripts[(size_t)index];
	if (!row.session)
		return false;
	for (const auto &session : CachedSessions())
	{
		if (session.id == row.session && session.state != SessionState::Finished)
		{
			if (aSession)
				*aSession = session.id;
			return true;
		}
	}
	return false;
}

void DeckWindow::RunOrStopEditing()
{
	DWORD session = 0;
	if (EditingScriptRunning(&session))
	{
		if (!mEngine.StopSession(session))
		{
			Append("[ui] 当前脚本已经结束或无法停止");
			return;
		}
		InvalidateSessions();
		AppendIfTest(mEditPath, "[" + UiClock() + "] 停止中");
		RefreshList();
		return;
	}
	SaveAndRun();
}

void DeckWindow::SyncEditorRunButton()
{
	if (!mConsoleRun)
		return;
	const bool running = EditingScriptRunning();
	mConsoleRun->copy_label(running ? "停止" : "运行");
	mConsoleRun->tooltip(running ? "停止当前编辑器中的脚本"
								 : "先保存到本地再运行；未命名会提示命名");
	mConsoleRun->activate();
	mConsoleRun->labelcolor(DeckColors().text1);
	mConsoleRun->redraw();
}

void DeckWindow::OpenLogs()
{
	std::wstring root = mEngine.LogRoot();
	if (root.empty())
	{
		Append("[ui] 日志目录尚未建立（引擎未加载）");
		return;
	}
	HINSTANCE result = ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if ((INT_PTR)result <= 32)
		Append("[ui] 无法打开日志目录");
	else
		Append("[ui] 打开日志目录: " + Utf8(root));
}

void DeckWindow::EditSelectedExternally()
{
	int index = SelectedIndex();
	if (index < 0)
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	const ScriptRow &row = mScripts[(size_t)index];
	HINSTANCE r = ShellExecuteW(nullptr, L"open", L"notepad.exe", row.path.c_str(), nullptr, SW_SHOWNORMAL);
	if ((INT_PTR)r <= 32)
		Append("[ui] 无法打开记事本");
}

// ---------------------------------------------------------------------------
// editor (m015)
// ---------------------------------------------------------------------------

void DeckWindow::OnBufferModified(int, int aInserted, int aDeleted, int, const char *, void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	// Selection changes also run this callback (FLTK uses it to restyle), so a
	// zero-length change is not a modification - marking it dirty would put a
	// stale '*' on a file nobody edited (m016 caught this via find).
	if (aInserted == 0 && aDeleted == 0)
		return;
	self->UpdateLineNumberWidth();
	if (self->mEditLoading || self->mEditDirty)
		return;
	self->mEditDirty = true;
	if (self->mList)
		self->mList->redraw();
}

void DeckWindow::SyncEditorActions()
{
	// m044/m045: 保存/关闭 are permanent residents of the editor toolbar. With
	// no script open they stay visible but disabled; they never vanish or shift
	// the row.
	const bool hasScript = !mEditPath.empty() || mEditIsNew;
	Fl_Button *scriptActions[] = {mConsoleSave, mConsoleClose};
	for (Fl_Button *button : scriptActions)
	{
		if (!button)
			continue;
		if (hasScript)
		{
			button->activate();
			button->labelcolor(DeckColors().text1);
		}
		else
		{
			button->deactivate();
			button->labelcolor(DeckColors().text2);
		}
		button->redraw();
	}
	SyncEditorRunButton();
}

bool DeckWindow::LoadIntoEditor(const std::wstring &aPath)
{
	if (mEditIsNew && mEditDirty)
	{
		Append("[editor] 当前新建脚本还没命名，先保存后再切换");
		PromptSaveNewScript(false);
		return false;
	}
	// Editing another script first persists the one that is open. If the save
	// fails, stay put: switching away would silently lose the edit.
	if (!mEditPath.empty() && !SamePathI(mEditPath, aPath) && mEditDirty)
	{
		if (!SaveEditorTo(mEditPath))
		{
			Append("[editor] 保存失败，未切换到 " + Utf8(LeafOf(aPath)));
			return false;
		}
	}

	TextDocument document;
	std::string error;
	if (!ReadTextDocument(aPath, document, error))
	{
		Append("[editor] 打开失败: " + Utf8(LeafOf(aPath)) + " - " + error);
		return false;
	}

	mEditLoading = true;
	mEditBuffer->text(document.utf8.c_str());
	mEditBuffer->select(0, 0);
	mEditLoading = false;
	UpdateLineNumberWidth();

	mEditPath = aPath;
	mEditDirty = false;
	mEditIsNew = false;
	// One format for every file the deck opens: UTF-8 without BOM, CRLF. If the
	// source was UTF-16, UTF-8 BOM, LF or mixed, rewrite it now so the editor and
	// the disk never disagree about the canonical form.
	const bool canonical = document.encoding == TextEncoding::Utf8NoBom && document.crlfOnly;
	if (!canonical)
	{
		std::string convertError;
		if (!WriteTextDocument(aPath, document.utf8, TextEncoding::Utf8NoBom, true,
			nullptr, convertError))
			Append("[editor] 格式统一失败: " + Utf8(LeafOf(aPath)) + " - " + convertError);
		else
			Append("[editor] 已统一格式: " + Utf8(LeafOf(aPath)));
	}
	mEditor->insert_position(0);
	mEditor->scroll(0, 0);
	SyncEditorActions();
	SyncConsoleSelection();
	if (mList)
		mList->redraw();
	mLastAutoSave = GetTickCount64();

	Append("[editor] 已载入 " + Utf8(LeafOf(aPath)));
	return true;
}

bool DeckWindow::SaveEditorTo(const std::wstring &aPath)
{
	std::string error;
	const char *text = mEditBuffer->text();
	if (!WriteTextDocument(aPath, text, TextEncoding::Utf8NoBom, true, nullptr, error))
	{
		Append("[editor] 保存失败: " + Utf8(LeafOf(aPath)) + " - " + error);
		return false;
	}

	mEditPath = aPath;
	mEditDirty = false;
	mEditIsNew = false;
	mLastAutoSave = GetTickCount64();
	SyncEditorActions();
	if (mList)
		mList->redraw();
	Append("[editor] 已保存 " + Utf8(LeafOf(aPath)));

	if (FindScript(aPath) < 0)
	{
		AddScript(aPath);
		RefreshList();
	}
	return true;
}

bool DeckWindow::SaveEditor()
{
	if (mEditPath.empty())
	{
		PromptSaveNewScript(false);
		return false;
	}
	return SaveEditorTo(mEditPath);
}

void DeckWindow::SaveAndRun()
{
	if (mEditPath.empty())
	{
		PromptSaveNewScript(true);
		return;
	}
	if (!SaveEditor())
		return;
	StartPath(mEditPath);
}

std::string DeckWindow::ScriptTargetPath(const std::string &aLeaf) const
{
	std::string leaf = aLeaf;
	if (leaf.size() < 4 || _stricmp(leaf.c_str() + leaf.size() - 4, ".ahk") != 0)
		leaf += ".ahk";
	return Utf8(ExeDirectory() + L"\\Scripts\\" + Wide(leaf));
}

std::string DeckWindow::ValidateScriptLeaf(const std::string &aLeaf,
	const std::wstring &aExcept) const
{
	if (aLeaf.empty())
		return "名称不能为空";
	if (aLeaf == "." || aLeaf == "..")
		return "名称不能是 . 或 ..";
	if (aLeaf.find_first_of("\\/:*?\"<>|") != std::string::npos)
		return "名称不能包含 \\ / : * ? \" < > |";
	if (aLeaf.back() == ' ' || aLeaf.back() == '.')
		return "名称不能以空格或点结尾";
	if (aLeaf.size() > 180)
		return "名称太长";
	const std::wstring target = Wide(ScriptTargetPath(aLeaf));
	if (!aExcept.empty() && SamePathI(target, aExcept))
		return std::string();
	if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
		return "同名脚本已存在";
	return std::string();
}

void DeckWindow::ShowNameDialog(const char *aTitle, const std::string &aInitial,
	std::function<std::string(const std::string &)> aValidate,
	std::function<void(const std::string &)> aAccept)
{
	delete mNameDialog;
	NameWindow *dialog = new NameWindow(aTitle, aInitial.c_str(),
		std::move(aValidate), std::move(aAccept));
	mNameDialog = dialog;
	dialog->SetFrameOptions(mRoundedCorners, mWindowShadow);
	dialog->ShowCentered();
	dialog->focusInput();
}

void DeckWindow::PromptSaveNewScript(bool aRun)
{
	ShowNameDialog("新建脚本", "新脚本.ahk",
		[this](const std::string &aName) { return ValidateScriptLeaf(aName, std::wstring()); },
		[this, aRun](const std::string &aName)
		{
			const std::wstring target = Wide(ScriptTargetPath(aName));
			CreateDirectoryW((ExeDirectory() + L"\\Scripts").c_str(), nullptr);
			if (!SaveEditorTo(target))
				return;
			mEditIsNew = false;
			if (aRun)
				StartPath(target);
		});
}

void DeckWindow::NewScript()
{
	EnsureEditorVisible();
	if (!mEditPath.empty() && mEditDirty && !SaveEditor())
		return;
	if (mEditIsNew && mEditDirty)
	{
		Append("[editor] 先保存当前新建脚本，再新建另一个");
		PromptSaveNewScript(false);
		return;
	}

	mEditLoading = true;
	mEditBuffer->text("");
	mEditBuffer->select(0, 0);
	mEditLoading = false;
	mEditPath.clear();
	mEditDirty = false;
	mEditIsNew = true;
	mConsolePath.clear();
	mBuffer->text("");
	UpdateLineNumberWidth();
	mEditor->insert_position(0);
	mEditor->scroll(0, 0);
	SyncEditorActions();
	RefreshList();
	mEditor->take_focus();
}

void DeckWindow::RenameSelected()
{
	const int index = SelectedIndex();
	if (index < 0)
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	const ScriptRow row = mScripts[(size_t)index];
	if (IsRunning(row))
	{
		Append("[ui] 先停止脚本，再修改名称");
		return;
	}
	if (!mEditPath.empty() && SamePathI(mEditPath, row.path) && mEditDirty && !SaveEditor())
		return;

	ShowNameDialog("重命名脚本", row.name.c_str(),
		[this, row](const std::string &aName)
		{
			return ValidateScriptLeaf(aName, row.path);
		},
		[this, row](const std::string &aName)
		{
			const std::wstring target = Wide(ScriptTargetPath(aName));
			if (SamePathI(target, row.path))
				return;
			if (!MoveFileW(row.path.c_str(), target.c_str()))
			{
				Append("[ui] 重命名失败（" + std::to_string((unsigned long)GetLastError()) + "）");
				return;
			}
			const std::string leaf = Utf8(LeafOf(target));
			const std::string binding = HotkeyOf(mHotkeys, row.name);
			if (!binding.empty())
			{
				SetHotkeyEntry(mHotkeys, row.name, std::string());
				SetHotkeyEntry(mHotkeys, leaf, binding);
				std::string error;
				if (!SaveHotkeys(mHotkeyPath, mHotkeys, error))
					Append("[hotkey] 重命名后保存失败：" + error);
			}
			if (!mEditPath.empty() && SamePathI(mEditPath, row.path))
				mEditPath = target;
			if (!mConsolePath.empty() && SamePathI(mConsolePath, row.path))
				mConsolePath = target;
			ScanScripts();
			RefreshList();
			RebindHotkeys();
			Append("[ui] 已重命名为 " + leaf);
		});
}

// m044: close the current script but keep the editor column. Unsaved changes
// are written first; if that save fails, stay in the script.
void DeckWindow::CloseScript()
{
	if (mEditPath.empty() && !mEditIsNew)
		return;
	if (mEditIsNew)
	{
		if (mEditDirty)
		{
			Append("[editor] 新建脚本还没命名，先保存再关闭");
			PromptSaveNewScript(false);
			return;
		}
	}
	else if (mEditDirty && !SaveEditor())
	{
		return;
	}

	mEditLoading = true;
	mEditBuffer->text("");
	mEditBuffer->select(0, 0);
	mEditLoading = false;
	mEditPath.clear();
	mEditDirty = false;
	mEditIsNew = false;
	mConsolePath.clear();
	mBuffer->text("");
	UpdateLineNumberWidth();
	mEditor->insert_position(0);
	mEditor->scroll(0, 0);
	SyncEditorActions();
	RefreshList();
}

void DeckWindow::ReloadEditor()
{
	if (mEditPath.empty())
	{
		Append("[editor] 没有已打开的文件");
		return;
	}
	if (mEditDirty)
		Append("[editor] 丢弃未保存修改，重新载入 " + Utf8(LeafOf(mEditPath)));
	LoadIntoEditor(mEditPath);
}

void DeckWindow::FocusFind()
{
	EnsureEditorVisible();
	mFindInput->take_focus();
	mFindInput->insert_position(0, (int)std::strlen(mFindInput->value()));
}

void DeckWindow::FocusReplace()
{
	EnsureEditorVisible();
	mReplaceInput->take_focus();
	mReplaceInput->insert_position(0, (int)std::strlen(mReplaceInput->value()));
}

void DeckWindow::FindNext()
{
	const char *needle = mFindInput->value();
	if (!needle || !*needle)
	{
		Append("[find] 先输入要查找的文本");
		FocusFind();
		return;
	}

	int start = mEditor->insert_position();
	int selStart = 0;
	int selEnd = 0;
	if (mEditBuffer->selection_position(&selStart, &selEnd) && selEnd > selStart)
		start = selEnd;

	const int matchCase = mMatchCase->value() ? 1 : 0;
	int found = 0;
	bool wrapped = false;
	if (!mEditBuffer->search_forward(start, needle, &found, matchCase))
	{
		if (start > 0 && mEditBuffer->search_forward(0, needle, &found, matchCase))
			wrapped = true; // one wrap-around, then stop: no silent infinite loop
		else
		{
			Append(std::string("[find] 未找到: ") + needle);
			return;
		}
	}

	const int length = (int)std::strlen(needle);
	mEditBuffer->select(found, found + length);
	mEditor->insert_position(found + length);
	mEditor->show_insert_position();
	mEditor->take_focus();
	Append(std::string("[find] ") + (wrapped ? "已回到开头，选中 1 处: " : "选中 1 处: ") + needle);
}

void DeckWindow::ReplaceOne()
{
	const char *needle = mFindInput->value();
	if (!needle || !*needle)
	{
		Append("[find] 先输入要查找的文本");
		FocusFind();
		return;
	}
	const char *replacement = mReplaceInput->value();
	if (!replacement)
		replacement = "";

	int selStart = 0;
	int selEnd = 0;
	bool replaced = false;
	if (mEditBuffer->selection_position(&selStart, &selEnd) && selEnd > selStart)
	{
		char *selected = mEditBuffer->text_range(selStart, selEnd);
		const bool matches = selected != nullptr && std::strcmp(selected, needle) == 0;
		if (selected)
			std::free(selected);
		if (matches)
		{
			mEditBuffer->replace(selStart, selEnd, replacement);
			mEditor->insert_position(selStart + (int)std::strlen(replacement));
			Append("[find] 已替换 1 处");
			replaced = true;
		}
	}
	if (!replaced)
		Append("[find] 当前没有选中要替换的文本，先查找");
	FindNext();
}

void DeckWindow::ReplaceAll()
{
	const char *needle = mFindInput->value();
	if (!needle || !*needle)
	{
		Append("[find] 先输入要查找的文本");
		FocusFind();
		return;
	}
	const char *replacement = mReplaceInput->value();
	if (!replacement)
		replacement = "";

	const int matchCase = mMatchCase->value() ? 1 : 0;
	const int needleLength = (int)std::strlen(needle);
	const int replacementLength = (int)std::strlen(replacement);
	int start = 0;
	int found = 0;
	int count = 0;
	while (mEditBuffer->search_forward(start, needle, &found, matchCase))
	{
		mEditBuffer->replace(found, found + needleLength, replacement);
		// Advance past the replacement: a replacement that contains the needle
		// must not be re-scanned forever.
		start = found + replacementLength;
		++count;
	}
	mEditBuffer->unselect();
	Append("[find] 全部替换: " + std::to_string(count) + " 处");
}

void DeckWindow::EditRow(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;
	if (const int line = LineOfIndex(aIndex))
		mBrowser->SetFocusLine(line);
	const std::wstring path = mScripts[(size_t)aIndex].path;
	EnsureEditorVisible();
	if (!mEditPath.empty() && SamePathI(mEditPath, path))
		return; // already editing this file; a row click must not reload it
	LoadIntoEditor(path); // saves the previous file first if it was dirty
}

void DeckWindow::ShowContextMenu(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;

	const int line = LineOfIndex(aIndex);
	const ScriptRow &row = mScripts[(size_t)aIndex];
	const bool running = IsRunning(row);

	// m050: the row context is exactly the five per-script actions. The script's
	// own tray menu stays on the row's 菜单 (gear) cell.
	mContextLabels.clear();
	mContextItems.clear();
	mContextLabels.reserve(8);
	mContextItems.reserve(8);
	auto add = [this](const char *aLabel, int aId) {
		mContextLabels.push_back(aLabel);
		Fl_Menu_Item item = { mContextLabels.back().c_str(), 0, nullptr, (void *)(intptr_t)aId, 0 };
		mContextItems.push_back(item);
	};
	add(running ? "停止" : "运行", 1);
	add("重启", 2);
	add("编辑", 3);
	add("快捷键…", 4);
	add("重命名", 5);
	const Fl_Menu_Item end = { nullptr };
	mContextItems.push_back(end);

	// Fl_Menu_Item::popup() takes WIDGET-RELATIVE coordinates and adds the event's
	// root offset itself (Fl_Menu.cxx pulldown(): X += event_x_root()-event_x()).
	// Passing event_x_root() here double-added the window origin, so the menu
	// landed offset by the window position - invisible while the window sat near
	// (0,0), obvious as soon as the user moved it.
	// Pass the theme's menu bar as the menubutton (Pecia does the same): the
	// popup then uses the same font, height, colour and hover style as the bar.
	const Fl_Menu_Item *picked = mContextItems.data()[0].popup(
		Fl::event_x(), Fl::event_y(), nullptr, nullptr, mMenu);
	if (!picked)
		return;

	const int id = (int)(intptr_t)picked->user_data();
	if (mBrowser)
		mBrowser->SetFocusLine(line);

	switch (id)
	{
	case 1: // 运行 / 停止
		ToggleRow(aIndex);
		break;
	case 2: // 重启
		RestartRow(aIndex);
		break;
	case 3: // 编辑
		EditRow(aIndex);
		break;
	case 4: // 快捷键…
		BeginHotkeyEdit(aIndex);
		break;
	case 5: // 重命名
		RenameSelected();
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// m023/m024: start/stop switch, restart, the script's own tray menu, bindings, tray
// ---------------------------------------------------------------------------

std::string DeckWindow::BindingOf(const ScriptRow &aRow) const
{
	return HotkeyOf(mHotkeys, aRow.name);
}

void DeckWindow::ToggleRow(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;
	ScriptRow &row = mScripts[(size_t)aIndex];

	if (!IsRunning(row))
	{
		StartPath(row.path);
		return;
	}
	if (!mEngine.StopSession(row.session))
	{
		Append("[ui] " + row.name + " 已经结束或无法停止");
		return;
	}
	InvalidateSessions();
	AppendIfTest(row.path, "[" + UiClock() + "] 停止中");
	RefreshList();
}

void DeckWindow::RestartRow(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;
	ScriptRow &row = mScripts[(size_t)aIndex];

	if (!IsRunning(row))
	{
		StartPath(row.path); // v1.3's Restart on a stopped script is just a start
		return;
	}
	if (!mPendingRestart.empty())
	{
		Append("[ui] 上一次重启还在等旧会话退出：" + Utf8(LeafOf(mPendingRestart)));
		return;
	}
	// Waiting is not done with Sleep(): a script that ignores WM_QUIT for a while
	// would freeze the window. The tick that already tracks sessions starts the
	// new session the moment this one is reaped.
	if (!mEngine.StopSession(row.session))
	{
		Append("[ui] " + row.name + " 已经结束或无法停止");
		return;
	}
	InvalidateSessions();
	mPendingRestart = row.path;
	RefreshList();
	AppendIfTest(row.path, "[" + UiClock() + "] 重启中");
}

void DeckWindow::OpenScriptMenu(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
	{
		Append("[ui] 先在左侧选一个在跑的脚本");
		return;
	}
	const ScriptRow &row = mScripts[(size_t)aIndex];

	HWND window = mEngine.FindScriptWindow(row.session);
	if (!window)
	{
		Append("[ui] " + row.name + " 没在运行，或它的主窗口已经没了");
		return;
	}

	// AHK_NOTIFYICON (WM_USER+4) with lParam = WM_RBUTTONUP is the kernel's own
	// "the user right-clicked my tray icon" path: MainWindowProc calls
	// mTrayMenu->Display() with no coordinates, and Display() then reads
	// GetCursorPos itself (.3rd\AutoHotkey_H-v2.0.25\source\script2.cpp:164-192,
	// util.cpp:1809-1830). That is why no point is passed here: A_TrayMenu.Show(x, y)
	// interprets x/y by the script's Menu coord mode, whose default is *Client*
	// (g.CoordMode = 0), so a screen point handed to it is silently offset by the
	// foreground window's origin - measured delta (428,331) with a window at
	// (420,300). The native message needs no cooperation from the script either,
	// so it covers scripts that never included the bridge.
	// UserMenu::Display() calls SetForegroundWindowEx(g_hWnd) before
	// TrackPopupMenuEx. When the deck owns the foreground, Windows may refuse
	// that hand-off; the menu then paints but its first click only activates/
	// dismisses it instead of running 退出. Hand the foreground over first.
	// The shell/taskbar foreground lets AHK's UserMenu::Display take the
	// foreground before TrackPopupMenuEx, exactly like a real tray right-click.
	if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr))
		SetForegroundWindow(tray);
	if (!PostMessageW(window, 0x0404 /*AHK_NOTIFYICON*/, (WPARAM)1, (LPARAM)WM_RBUTTONUP))
		Append("[ui] 投递菜单请求失败：" + row.name);
	else
		Append("[" + UiClock() + "] " + row.name + " 已请求脚本自己的菜单");
}

void DeckWindow::BeginHotkeyEdit(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	const int line = LineOfIndex(aIndex);
	if (line > 0 && mBrowser)
		mBrowser->BeginHotkeyRecord(line);
}

void DeckWindow::CommitInlineHotkey(int aIndex, const std::string &aText)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;
	const ScriptRow &row = mScripts[(size_t)aIndex];
	const std::string typed = aText;

	unsigned int mods = 0;
	unsigned int virtualKey = 0;
	std::string canonical;
	std::string error;
	if (!typed.empty() && !ParseAhkHotkey(typed, mods, virtualKey, canonical, error))
	{
		Append("[hotkey] " + row.name + "：" + error);
		return;
	}
	if (!typed.empty())
	{
		const std::string owner = OwnerOfHotkey(mHotkeys, row.name, typed);
		if (!owner.empty())
		{
			Append("[hotkey] " + canonical + " 已属于 " + owner + "；先给它换一个");
			return;
		}
	}
	SetHotkeyEntry(mHotkeys, row.name, typed.empty() ? std::string() : canonical);
	std::string saveError;
	if (!SaveHotkeys(mHotkeyPath, mHotkeys, saveError))
	{
		Append("[hotkey] 保存失败：" + saveError);
		return;
	}
	RebindHotkeys();
	RefreshList();
	Append(typed.empty()
		? "[" + UiClock() + "] 已解除 " + row.name + " 的快捷键"
		: "[" + UiClock() + "] " + row.name + " 的快捷键 = " + canonical);
}

// Settings > 快捷键 (OK): the dialog keeps its own table until the user agrees,
// then writes every row here. One save + one rebind for the whole table, so a
// half-applied dialog can never leave the hotkey file ahead of the hooks.
bool DeckWindow::ApplyShortcutRows(const std::vector<std::pair<std::string, std::string>> &aRows)
{
	for (const auto &row : aRows)
		SetHotkeyEntry(mHotkeys, row.first, row.second);
	std::string error;
	if (!SaveHotkeys(mHotkeyPath, mHotkeys, error))
	{
		Append("[hotkey] 保存失败：" + error);
		return false;
	}
	RebindHotkeys();
	RefreshList();
	int bound = 0;
	for (const auto &row : aRows)
	{
		if (!row.second.empty())
			++bound;
	}
	Append("[" + UiClock() + "] 快捷键已保存（" + std::to_string(bound) + " 条）");
	return true;
}

std::string DeckWindow::ScriptNameAt(int aIndex) const
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return std::string();
	return mScripts[(size_t)aIndex].name;
}

std::string DeckWindow::ScriptBindingAt(int aIndex) const
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return std::string();
	return BindingOf(mScripts[(size_t)aIndex]);
}

void DeckWindow::ShowShortcutDialog()
{
	// Rebuild each time: a rescan while the dialog was closed must not leave a
	// stale script list in it.
	delete mShortcutDialog;
	mShortcutDialog = new ShortcutDialogWindow(this);
	mShortcutDialog->SetFrameOptions(mRoundedCorners, mWindowShadow);
	mShortcutDialog->ShowCentered();
}


void DeckWindow::LoadHotkeyFile()
{
	mHotkeyPath = AppHotkeyPath();
	if (mHotkeyPath.empty())
		return;
	std::string error;
	if (!LoadHotkeys(mHotkeyPath, mHotkeys, error))
		Append("[hotkey] 读取失败: " + error);
}

void DeckWindow::RebindHotkeys()
{
	mHotkeyTargets.clear();
	if (!mHooks.Window())
		return; // no control window: the hotkey half of the deck is unavailable
	mHooks.UnregisterHotkeys();

	int bound = 0;
	for (size_t i = 0; i < mScripts.size(); ++i)
	{
		const ScriptRow &row = mScripts[i];
		const std::string text = BindingOf(row);
		if (text.empty())
			continue;

		unsigned int mods = 0;
		unsigned int virtualKey = 0;
		std::string canonical;
		std::string error;
		if (!ParseAhkHotkey(text, mods, virtualKey, canonical, error))
		{
			Append("[hotkey] " + row.name + " " + text + "：" + error);
			continue;
		}

		const int id = (int)i + 1; // the row index is the hotkey id for this table
		std::string registerError;
		if (!mHooks.RegisterHotkey(id, mods, virtualKey, registerError))
		{
			Append("[hotkey] " + row.name + " " + canonical + " 未生效：" + registerError);
			continue;
		}
		mHotkeyTargets[id] = row.path;
		++bound;
	}
	if (bound)
		Append("[" + UiClock() + "] 全局快捷键已绑定 " + std::to_string(bound) + " 个");
}

void DeckWindow::HandleIntent(const NativeHooks::Intent &aIntent)
{
	switch (aIntent.kind)
	{
	case NativeHooks::Intent::Kind::Hotkey:
	{
		auto target = mHotkeyTargets.find(aIntent.hotkeyId);
		if (target == mHotkeyTargets.end())
			return;
		const int index = FindScript(target->second);
		if (index < 0)
			return;
		// A global hotkey is not a gesture on this window: it must not take the
		// console away from whatever the user is testing (m067).
		ToggleRow(index);
		break;
	}
	case NativeHooks::Intent::Kind::TrayShow:
		show();
		// The tray click is a real user gesture, so the deck is allowed to take the
		// foreground back; SW_RESTORE also covers the minimised case.
		if (HWND hwnd = (HWND)fl_xid(this))
		{
			ShowWindow(hwnd, SW_RESTORE);
			SetForegroundWindow(hwnd);
		}
		// The tick skipped rendering while hidden; catch up immediately.
		InvalidateSessions();
		RefreshList();
		UpdateStatus();
		SyncDeckFrame(this);
		break;
	case NativeHooks::Intent::Kind::Quit:
		RequestQuit();
		hide();
		break;
	default:
		break;
	}
}

void DeckWindow::HideToTray()
{
	hide();
	UpdateTrayTip();
	Append("[" + UiClock() + "] 已隐藏到托盘：脚本继续运行；双击托盘图标回到窗口，"
		"托盘菜单「退出」才会停掉所有脚本");
}

void DeckWindow::RequestQuit()
{
	mQuitRequested = true;
}

void DeckWindow::UpdateTrayTip()
{
	if (!mHooks.HasTrayIcon())
		return;
	size_t running = 0;
	for (const auto &session : CachedSessions())
	{
		if (session.state != SessionState::Finished)
			++running;
	}
	// NIM_MODIFY is an IPC to the shell: only send it when the number changed.
	const std::string tip = "AHKDeck  运行中 " + std::to_string((unsigned long)running);
	if (tip == mLastTrayTip)
		return;
	mLastTrayTip = tip;
	mHooks.UpdateTrayTip(tip);
}

void DeckWindow::UpdateKernelSummary()
{
	mKernelSummary = KernelVersionOf(ExeDirectory() + L"\\AutoHotkey64.dll");
}


void DeckWindow::OnSelect(Fl_Widget *, void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	if (self->mUpdatingList)
		return;
	// Selecting a row only selects it (run / stop / remove target). The source
	// area and console are opened explicitly: right-click -> 编辑, or the View
	// menu. m022 requirement: browsing scripts must not pull them into the editor.
}


void DeckWindow::OnClear(Fl_Widget *, void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	self->mBuffer->text("");
}

void DeckWindow::OnCopy(Fl_Widget *, void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	const char *text = self->mBuffer->text();
	Fl::copy(text, (int)std::strlen(text), 1);
	self->Append("[ui] 控制台文本已复制");
}




void DeckWindow::ApplyWidgetTheme()
{
	const ThemeColors &colors = DeckColors();

	mMenu->box(FL_FLAT_BOX);
	mMenu->color(colors.background3);
	mMenu->textcolor(colors.text1);
	mMenu->selection_color(colors.highlight1);
	mMenu->check_color(colors.highlight2);
	mMenu->labelsize(16);
	mMenu->textsize(16);
	mMenu->setPressColor(colors.highlight2);

	// m031: only the contextual bars still have buttons.
	Fl_Button *buttons[] = {mFindNext, mReplaceOne, mReplaceAll};
	for (Fl_Button *button : buttons)
		StyleButton(button);

	StyleInput(mFilter);
	if (auto *search = dynamic_cast<SearchInput *>(mFilter))
		search->applyBox(); // StyleInput's FL_BORDER_BOX would undo the 24px inset
	StyleInput(mFindInput);
	StyleInput(mReplaceInput);
	StyleTextDisplay(mEditor);
	StyleTextDisplay(mConsole);

	mMatchCase->box(FL_BORDER_BOX);
	mMatchCase->color(colors.background1);
	mMatchCase->selection_color(colors.highlight2);
	mMatchCase->labelcolor(colors.text1);
	mMatchCase->labelsize(16);

	mList->box(FL_FLAT_BOX);
	mList->color(colors.background1);
	mList->textcolor(colors.text1);
	// m037: the browser still tracks a row for keyboard commands, but it must
	// not paint a selected band - the row is a set of buttons, not a mode.
	mList->selection_color(colors.background1);
	mList->textsize(16);
	mList->scrollbar.type(FL_VERT_SLIDER);
	mList->scrollbar.box(FL_FLAT_BOX);
	mList->scrollbar.color(colors.background1);
	mList->scrollbar.selection_color(colors.background2);

	mStatus->box(FL_FLAT_BOX);
	mStatus->color(colors.background3);
	mStatus->labelcolor(colors.text2);
	mStatus->labelsize(14);

	mEditorBar->box(FL_FLAT_BOX);
	mEditorBar->color(colors.background3);

	// Pecia's tool-window buttons: background2 fill on the grey bar, 1px themed
	// border, hover/press through the shared HoverButton palette.
	Fl_Button *consoleButtons[] = {mConsoleSave, mConsoleClose, mConsoleClear, mConsoleRun};
	for (Fl_Button *button : consoleButtons)
	{
		if (!button)
			continue;
		button->box(FL_BORDER_BOX);
		button->color(colors.background2);
		button->selection_color(colors.highlight1);
		button->labelcolor(colors.text1);
		button->labelsize(16);
	}



	mEditor->textfont(FL_COURIER);
	mEditor->textsize(16);
	// Pecia: the gutter shares the editor background and the text colour; the
	// only divider is the 1px dashed rule drawn by EditorBox.
	mEditor->linenumber_font(FL_COURIER);
	mEditor->linenumber_size(16);
	mEditor->linenumber_fgcolor(colors.text1);
	mEditor->linenumber_bgcolor(colors.background1);
	UpdateLineNumberWidth();
	mConsole->textfont(FL_HELVETICA);
	mConsole->textsize(16);

	if (auto *editor = dynamic_cast<EditorBox *>(mEditor))
		editor->StyleScrollbars();
	if (auto *console = dynamic_cast<DeckDisplay *>(mConsole))
		console->StyleScrollbars();
}

void DeckWindow::UpdateLineNumberWidth()
{
	if (!mEditor || !mEditBuffer)
		return;
	int lines = mEditBuffer->count_lines(0, mEditBuffer->length());
	if (lines < 1)
		lines = 1;
	int digits = 1;
	for (int n = lines; n >= 10; n /= 10)
		++digits;
	char sample[16];
	for (int i = 0; i < digits; ++i)
		sample[i] = '9';
	sample[digits] = 0;

	// Same measurement as Pecia's MainWindow::updateLinenumberWidth: the
	// widest digit in the gutter font, plus 3px padding on each side and 2px.
	fl_font(mEditor->linenumber_font(), mEditor->linenumber_size());
	const int width = (int)fl_width(sample) + 8;
	if (mEditor->linenumber_width() != width)
	{
		mEditor->linenumber_width(width);
		mEditor->redraw();
	}
}

void DeckWindow::ApplyView()
{
	for (Fl_Widget *widget : mListWidgets)
	{
		if (mShowList)
			widget->show();
		else
			widget->hide();
	}
	for (Fl_Widget *widget : mEditorWidgets)
	{
		if (mShowEditor)
			widget->show();
		else
			widget->hide();
	}
	Layout();
	// The 保存/关闭 activation depends on whether a script is open; ApplyView
	// just showed every pane child, so re-sync the toolbar state here.
	SyncEditorActions();

	// FLTK does not repaint a Fl_Text_Display that was hidden when the pane is
	// shown again; ask all panes for a redraw so the first show is never blank.
	mList->redraw();
	mEditor->redraw();
	mConsole->redraw();
}

void DeckWindow::SyncViewMenu()
{
	// FLTK only repaints the menu when it is opened; without this the bar can
	// keep an old check state after a shortcut or programmatic change.
	auto setFlag = [this](const char *aPath, bool aOn)
	{
		Fl_Menu_Item *item = const_cast<Fl_Menu_Item *>(mMenu->find_item(aPath));
		if (!item)
			return;
		if (aOn)
			item->flags |= FL_MENU_VALUE;
		else
			item->flags &= ~FL_MENU_VALUE;
	};
	setFlag("视图/脚本列表", mShowList);
	setFlag("视图/编辑器与控制台", mShowEditor);
	setFlag("视图/窗口圆角", mRoundedCorners);
	setFlag("视图/窗口阴影", mWindowShadow);
	mMenu->redraw();
}

void DeckWindow::EnsureEditorVisible()
{
	if (mShowEditor)
		return;
	mShowEditor = true;
	SyncViewMenu();
	ApplyView();
}


void DeckWindow::OnMenu(Fl_Widget *aWidget, void *aData)
{
	auto *self = static_cast<DeckWindow *>(aWidget ? aWidget->window() : nullptr);
	if (self)
		self->HandleMenu((int)(intptr_t)aData);
}

void DeckWindow::HandleMenu(int aId)
{
	switch (aId)
	{
	case IdNew: NewScript(); break;
	case IdRename: RenameSelected(); break;
	case IdSave: SaveEditor(); break;
	case IdSaveRun: SaveAndRun(); break;
	case IdReloadEditor: ReloadEditor(); break;
	case IdLogs: OpenLogs(); break;
	case IdQuit: RequestQuit(); hide(); break;
	case IdHideTray:
		if (mHooks.HasTrayIcon())
			HideToTray();
		else
			Append("[ui] 没有托盘可用（控制台开头的 [deck] 行说明了原因），关闭即退出");
		break;
	case IdFind: EnsureEditorVisible(); FocusFind(); break;
	case IdReplace: EnsureEditorVisible(); FocusReplace(); break;
	case IdClearConsole: mBuffer->text(""); break;
	case IdCopyConsole: OnCopy(nullptr, this); break;
	case IdToggleRounded:
	case IdToggleShadow:
	{
		// Toggle from our state, not FLTK's mvalue(): the menu item is only a
		// mirror of the setting, and a click can leave mvalue on another item.
		const bool on = (aId == IdToggleRounded) ? !mRoundedCorners : !mWindowShadow;
		if (aId == IdToggleRounded)
			mRoundedCorners = on;
		else
			mWindowShadow = on;
		SyncViewMenu();
		ApplyFrameOptions();
		SaveSettings();
		break;
	}
	case IdViewList:
	case IdViewEditor:
	{
		// Toggle from our state, not FLTK's mvalue(): a keyboard shortcut or a
		// menu click can leave mvalue pointing at a different item, which made
		// the item only ever close.
		bool list = mShowList;
		bool editor = mShowEditor;
		if (aId == IdViewList)
			list = !list;
		else
			editor = !editor;
		if (!list && !editor)
		{
			Append("[ui] 至少保留一个面板");
			SyncViewMenu();
			return;
		}
		mShowList = list;
		mShowEditor = editor;
		ApplyView();
		SyncViewMenu();
		SaveSettings();
		break;
	}
	case IdRun: RunSelected(); break;
	case IdStop: StopSelected(); break;
	case IdRestart: RestartRow(SelectedIndex()); break;
	case IdScriptMenu: OpenScriptMenu(SelectedIndex()); break;
	case IdHotkeyEdit: ShowShortcutDialog(); break;
	case IdNotepad: OnNotepad(nullptr, this); break;
	case IdStopAll: StopEverything(); break;
	case IdRunAll: RunEverything(); break;
	case IdEditSelected: EditSelected(); break;
	case IdRescan: RefreshFromDisk(); break;
	case IdOpenScriptsFolder: OpenScriptsFolder(); break;
	case IdOpenConfigFile:
	{
		std::string error;
		if (GetFileAttributesW(mSettingsPath.c_str()) == INVALID_FILE_ATTRIBUTES)
			SaveSettings(&error);
		if (!error.empty())
			Append("[settings] " + error);
		ShellExecuteW(nullptr, L"open", mSettingsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		break;
	}
	case IdAbout:
		Append(std::string("[ui] AHKDeck ") + VersionString()
			+ " — 更新检查：暂未配置更新源（高级用户可在 settings.json 的 update.url 添加）");
		break;
	default:
		break;
	}
}


void DeckWindow::OnScriptWritten(const std::string &aUtf8Path)
{
	const std::wstring path = Wide(aUtf8Path);
	if (path.empty())
		return;
	if (FindScript(path) < 0)
	{
		AddScript(path);
		RefreshList();
	}
	if (!mEditPath.empty() && SamePathI(mEditPath, path))
	{
		if (mEditDirty)
			Append("[脚本] 外部改写了当前脚本；你有未保存修改，未自动重载");
		else
			LoadIntoEditor(path);
	}
}

void DeckWindow::ApplyToolEditorContent(const std::string &aText)
{
	EnsureEditorVisible();
	const bool wasUnsavedNew = mEditPath.empty();
	mEditLoading = true;
	mEditBuffer->text(aText.c_str());
	mEditBuffer->select(0, 0);
	mEditLoading = false;
	if (wasUnsavedNew)
		mEditIsNew = true;
	mEditDirty = true;
	UpdateLineNumberWidth();
	mEditor->insert_position(0);
	mEditor->scroll(0, 0);
	mLastAutoSave = GetTickCount64();
	SyncEditorActions();
	if (mList)
		mList->redraw();
}

void DeckWindow::OnTick(void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	// One engine snapshot serves this whole tick (m027).
	self->InvalidateSessions();
	self->DrainRecords();
	self->TrackSessions();

	// Timed save: the edited file is persisted every 30 s while dirty. The
	// explicit save-before-switch path uses the same SaveEditorTo.
	const ULONGLONG now = GetTickCount64();
	if (now - self->mLastTrayCheck >= 2000)
	{
		self->mLastTrayCheck = now;
		self->mHooks.EnsureTrayIcon();
	}
	if (self->mEditDirty && !self->mEditPath.empty() &&
		now - self->mLastAutoSave >= 30000)
	{
		self->mLastAutoSave = now;
		self->SaveEditorTo(self->mEditPath);
	}

	// Win32 raised these (hotkey / tray / --quit). Acting here rather than in the
	// window procedure means no FLTK widget is ever mutated inside a message
	// dispatch - the same ground the AI worker events already stand on.
	NativeHooks::Intent intent;
	while (self->mHooks.PopIntent(intent))
		self->HandleIntent(intent);

	// A restart waits for its own session to be reaped, so a script that ignores
	// WM_QUIT for a moment cannot freeze the window behind a Sleep() loop.
	if (!self->mPendingRestart.empty())
	{
		const int index = self->FindScript(self->mPendingRestart);
		bool stillRunning = false;
		if (index >= 0 && self->mScripts[(size_t)index].session)
		{
			Session pending;
			stillRunning = self->mEngine.GetSession(self->mScripts[(size_t)index].session, pending)
				&& pending.state != SessionState::Finished;
		}
		if (!stillRunning)
		{
			const std::wstring path = self->mPendingRestart;
			self->mPendingRestart.clear();
			self->Append("[" + UiClock() + "] " + Utf8(LeafOf(path)) + " 旧会话已退出，重启");
			self->StartPath(path);
		}
	}

	self->UpdateTrayTip();

	// m063/m064: what the tool threads are allowed to read, and what they asked
	// the window to do. Both are cheap and must happen before the UI goes idle.
	self->RefreshEditorSnapshot();
	self->DrainToolJobs();


	// Hidden to the tray or minimized: there is nothing to render, so skip the
	// browser rebuild and the status line. Sessions still advance (track + tray
	// tip run above), and the tray-show path below refreshes on the way back in.
	const HWND hwnd = (HWND)fl_xid(self);
	const bool shown = self->visible() && !(hwnd && IsIconic(hwnd));
	if (shown)
	{
		self->RefreshList();
		self->UpdateStatus();
	}

	// m027 idle downgrade: 10 Hz is only worth it while the user is looking or
	// an AI run is streaming. Hidden/minimized drops to 2 Hz, which is still
	// well below any script's own timer resolution the deck needs to notice.
	// m027 idle downgrade: 10 Hz is only worth it while the user is looking or an
	// MCP client can send a request at any moment - a tool call waiting 0.5 s for
	// the tick to publish the editor snapshot would be a stall the user cannot
	// explain.
	const double interval = (shown || self->mMcpRunning.load()) ? 0.1 : 0.5;
	Fl::repeat_timeout(interval, OnTick, aSelf);
}

} // namespace

void DeckWindow::InstallFrame()
{
	mFrameInstalled = true;
	InstallDeckFrame(this, mTitleBar, (unsigned int)FrameBorderColour(),
		mRoundedCorners, mWindowShadow);
	// The outer rect just grew by the 1px non-client strip; lay the client out
	// against the real client size Win32 now reports.
	Layout();
}

// View > 圆角窗口 / 窗口阴影. Both switches are live: the region is re-applied
// and the layered companion is rebuilt in place, so no restart is needed.
void DeckWindow::ApplyFrameOptions()
{
	const unsigned int border = (unsigned int)FrameBorderColour();
	windowChrome::setRoundedCornersEnabled(mRoundedCorners);
	windowChrome::applyRoundedCorners(this, mRoundedCorners, border);
	windowChrome::refreshFrameBand(this);
	windowShadow::setEnabled(mWindowShadow);
	windowShadow::applyAll();
	windowShadow::sync(this);
	// An open dialog wears the same frame and must follow the same switch.
	if (mNameDialog)
		mNameDialog->SetFrameOptions(mRoundedCorners, mWindowShadow);
	if (mShortcutDialog)
		mShortcutDialog->SetFrameOptions(mRoundedCorners, mWindowShadow);
	redraw();
}

// ---------------------------------------------------------------------------
// m063/m064 - what the tool threads may read, and what they asked for.
// ---------------------------------------------------------------------------

std::string DeckWindow::EditorSnapshot()
{
	std::lock_guard<std::mutex> lock(mSnapshotMutex);
	return mEditorSnapshot;
}

void DeckWindow::RefreshEditorSnapshot()
{
	if (!mEditBuffer)
		return;
	// Identity check first. Copying the whole editing area at 10 Hz is exactly
	// the idle cost m027 removed. Dirty is the truth signal for content edits;
	// length + path catch the loads and closes that clear it.
	const long chars = (long)mEditBuffer->length();
	const std::string key = mEditPath.empty() ? std::string("<new>") : Utf8(LeafOf(mEditPath));
	if (!mEditDirty && chars == mSnapshotChars && key == mSnapshotKey)
		return;
	char *raw = mEditBuffer->text();
	if (!raw)
		return;
	std::string text = raw;
	std::free(raw);
	{
		std::lock_guard<std::mutex> lock(mSnapshotMutex);
		mEditorSnapshot.swap(text);
	}
	mSnapshotChars = chars;
	mSnapshotKey = key;
}

void DeckWindow::DrainToolJobs()
{
	std::deque<ToolJob> jobs;
	{
		std::lock_guard<std::mutex> lock(mToolJobMutex);
		jobs.swap(mToolJobs);
	}
	for (const auto &job : jobs)
	{
		switch (job.kind)
		{
		case ToolJob::Kind::Editor:
			Append("[mcp] " + job.path);
			ApplyToolEditorContent(job.text);
			break;
		case ToolJob::Kind::Note:
			Append(job.text);
			break;
		case ToolJob::Kind::Write:
			OnScriptWritten(job.path);
			break;
		}
	}
}

void DeckWindow::StartMcp()
{
	if (mMcpStarted)
		return;
	mMcpStarted = true;

	AiScriptPolicy policy;
	policy.roots.push_back(ExeDirectory() + L"\\Scripts");
	mMcpTools.SetPolicy(policy);
	mMcpTools.SetEditorSupplier([this] { return EditorSnapshot(); });
	mMcpTools.SetEditorSink([this](const std::string &aContent, const std::string &aNote) {
		ToolJob job;
		job.kind = ToolJob::Kind::Editor;
		job.text = aContent;
		job.path = aNote;
		std::lock_guard<std::mutex> lock(mToolJobMutex);
		mToolJobs.push_back(std::move(job));
	});
	mMcpTools.SetNoteSink([this](const std::string &aText) {
		ToolJob job;
		job.kind = ToolJob::Kind::Note;
		job.text = "[mcp] " + aText;
		std::lock_guard<std::mutex> lock(mToolJobMutex);
		mToolJobs.push_back(std::move(job));
	});
	mMcpTools.SetWriteSink([this](const std::string &aPath) {
		ToolJob job;
		job.kind = ToolJob::Kind::Write;
		job.path = aPath;
		std::lock_guard<std::mutex> lock(mToolJobMutex);
		mToolJobs.push_back(std::move(job));
	});

	mMcpRunning = true;
	mMcpThread = std::thread([this] {
		agent::mcp::ServeStdio(mMcpTools, mMcpRunning, [this](const std::string &aLine) {
			std::string text = aLine;
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.pop_back();
			ToolJob job;
			job.kind = ToolJob::Kind::Note;
			job.text = "[mcp] " + text;
			std::lock_guard<std::mutex> lock(mToolJobMutex);
			mToolJobs.push_back(std::move(job));
		});
	});
}

void DeckWindow::StopMcp()
{
	mMcpRunning = false;
	// Detach, never join: the thread may be blocked in ReadFile on a stdin the
	// client forgot to close, and a blocked reader must not keep the deck on
	// screen after the user quit. Run()'s MCP branch exits the process right
	// after this, so the thread dies with the image - no window outlives it.
	if (mMcpThread.joinable())
		mMcpThread.detach();
}

int Run(HINSTANCE, bool aServeMcpStdio)
{
	Engine &engine = Engine::Instance();
	DeckWindow *window = new DeckWindow(engine);
	window->show();
	window->InstallFrame();

	// m064: the MCP client owns this process's stdin/stdout, so the endpoint
	// starts here - the window is already up, so a tool call arriving in the
	// first second still has somewhere to apply its editor content.
	if (aServeMcpStdio)
		window->StartMcp();

	// Test/screenshot hook only: the shortcut table is still opened from 设置.
	{
		char openShortcuts[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_SHORTCUTS", openShortcuts, sizeof(openShortcuts)) > 0)
			window->ShowShortcutDialog();
	}

	// Not Fl::run(): that one returns as soon as no window is left in FLTK's list,
	// and hiding to the tray removes the window from it (Fl_Window_Driver::hide_common,
	// .3rd\fltk-1.4.5\src\Fl_Window_Driver.cxx:129). The deck keeps pumping until an
	// explicit quit - Fl::wait() still runs the 0.1 s tick and every timeout.
	while (!window->ShouldQuit())
	{
		if (Fl::wait(0.2) < 0)
			break; // a broken event loop must not spin the CPU
	}
	const int rc = 0;

	// Persist geometry and the list view after the event loop ends, before the
	// window is torn down.
	window->SaveSettings();

	// A worker stuck inside WinHTTP (up to timeout_ms) must not keep the app on
	// screen after the user closed the window: the process is exiting, scripts
	// die with it, and the request dies with the process.

	// m064: with an MCP endpoint the window must not be freed underneath a
	// detached reader that can still wake up and call its sinks. Exiting instead
	// keeps the same promise the user gets by closing the window: every script
	// thread dies with this process image.
	window->StopMcp();
	if (window->McpStarted())
	{
		engine.StopAll();
		ExitProcess((UINT)rc);
	}

	delete window;
	engine.StopAll();

	// Give the kernel threads a moment to unwind through TerminateApp before
	// the process image (and the kernel DLL mapped into it) goes away.
	const DWORD start = GetTickCount();
	while (GetTickCount() - start < 3000)
	{
		bool running = false;
		for (const auto &session : engine.Sessions())
		{
			if (session.state != SessionState::Finished)
			{
				running = true;
				break;
			}
		}
		if (!running)
			break;
		Sleep(20);
	}
	return rc;
}

} // namespace ui
} // namespace ahkdeck

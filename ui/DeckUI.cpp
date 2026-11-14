#include "ui/DeckUI.h"
#include "ui/AboutDialog.h"
#include "ui/BlinkInput.h"
#include "ui/CaretBlink.h"
#include "ui/DeckDialog.h"
#include "ui/DeckFrame.h"
#include "ui/DeckTitleBar.h"
#include "ui/FltkErrorChannel.h"
#include "ui/HotkeyCapture.h"
#include "ui/HoverMenuBar.h"
#include "ui/ImeInPlace.h"
#include "ui/OptionsDialog.h"
#include "ui/ScriptDownloadDialog.h"
#include "ui/SubscribeDialog.h"
#include "ui/ScriptInfoDialog.h"
#include "ui/UpdateDialog.h"
#include "ui/WindowFrame.h"
#include "ui/WindowShadow.h"

#include "agent/McpServer.h"
#include "agent/Toolbox.h"
#include "core/AppPaths.h"
#include "core/ExeCompiler.h"
#include "core/Housekeeping.h"
#include "core/Hotkeys.h"
#include "core/Json.h"
#include "core/ResourceStats.h"
#include "core/ScriptCatalog.h"
#include "core/ScriptHeader.h"
#include "core/Settings.h"
#include "core/TextFile.h"
#include "core/Version.h"
#include "engine/DeckEngine.h"
#include "ui/NativeHooks.h"
#include "ui/ThemeWidgets.h"

#include <windows.h>
#include <commdlg.h> // m173: file dialogs for the 编译为 EXE target picker (IFileSaveDialog)
#include <shobjidl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <winver.h>

#include <cstdint>
#include <cstdio>

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

// m132: the list and the tray menu show a script by its name without the ".ahk"
// suffix. The row's name keeps the suffix - hotkeys.txt is keyed by it and the
// file itself is still a file - so this is a display-only transform.
std::string DisplayNameOf(const std::string &aName)
{
	static const char kSuffix[] = ".ahk";
	const size_t n = sizeof(kSuffix) - 1;
	if (aName.size() > n && _stricmp(aName.c_str() + aName.size() - n, kSuffix) == 0)
		return aName.substr(0, aName.size() - n);
	return aName;
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
// m182: aliased to the shared control height so the dialogs and the main
// window can never disagree about what "one row" is.
constexpr int kRowHeight = kUiControl;

// m046: the log takes a quarter of the space below the editor toolbar, and the
// divider is a visible 3px line rather than the old 1px hairline.
// m087: the deck's three sizes, and only three.
//   * a ROW (a list row, a caption row) is 26px;
//   * a CONTROL (a button, an input, a check box) is 26px as well - the same
//     number, so nothing in the deck is a size of its own;
//   * a BAR (the find bar, the action band that holds 保存/关闭/清空/运行)
//     is 32px: the 26px control plus 3px of air above and below, so a
//     control's frame line never touches the edge of the band it sits in.
// The font stays 16px everywhere. This bar is 32px too - only its control
// is smaller, because its font is 11px.
constexpr int kConsoleButtonH = 26;
constexpr int kConsoleButtonGap = 10;
constexpr int kConsoleButtonPadX = 12;
constexpr int kConsoleButtonMinW = 48;
constexpr int kPaneBarH = 32; // editor/AI middle band that holds the actions
// m052: AI defaults (endpoint is the full completions URL).
constexpr int kConsoleOutputPercent = 25; // default height share of the console
constexpr int kConsoleOutputMinH = 60;
// m112: the grab band between panes. It reuses the 6px the layout already
// leaves empty, and every pane keeps at least a fifth of the window so a drag
// can never squeeze one out of sight.
constexpr int kSplitterSize = 6;
constexpr int kPaneMinSharePct = 20;

// m084: the find/replace bar layout rules.
//   * the inputs have a FIXED width (120px) - they are search fields, not
//     text areas, and a field that stretches over the whole column is what
//     made the old bar look coarse;
//   * the hint ("查找" / "替换为") is drawn INSIDE the field;
//   * every button is as wide as its own label (the deck's one width rule,
//     HoverButton::fitWidth), never wider;
//   * the match counter fills the rest of the first row, centred.
constexpr int kFindInputW = 120;
constexpr int kFindGap = 6;
// m086: the deck's own rhythm - a control is 26px tall (the console bar's
// buttons) and a row is 26px (the list rows and the list's header row).
// m085 used 28/30, which made the find bar look like a foreign control.
// m087: the find bar is a BAR, not a ROW - 32px per line, 26px controls, so
// the border line of "下一个" is 3px away from the line's edge instead of
// touching it.
constexpr int kFindFieldH = 26;
constexpr int kFindRowH = 32;
// m087: the collapse button at the right end of the find bar (the deck's "X").
constexpr int kFindCloseW = 22;
// m086: the check box's own width rule: the 14px box plus the gap in front
// of the text (DeckCheckButton::kLabelGap).
constexpr int kFindChkPad = 20;
// The counter keeps this much room in the first row, so the controls never
// squeeze "k/N" out of the row.
constexpr int kFindCountMinW = 34;
// The counter stops counting here; a file with more matches than this would
// make the scan slower than the search it counts.
constexpr int kFindMaxMatches = 2000;

// m084: the deck's ONE width rule for a button, in the shape every other
// button uses (HoverButton::fitWidth: the label plus 12px on each side, never
// narrower than 48px). The find bar uses it for every control it owns, so the
// bar's buttons match the console bar, the dialogs and the list rows - and a
// toggle like "Aa" is not the odd one out with a width of its own.
int FitButtonWidth(Fl_Button *aButton)
{
	if (!aButton)
		return 0;
	int textW = 0;
	int textH = 0;
	aButton->measure_label(textW, textH);
	const int width = textW + 2 * HoverButton::kPadX;
	return width < HoverButton::kMinW ? HoverButton::kMinW : width;
}

// m085: checkbox width - the little square plus the gap in front of
// the text (CHK_PAD = 24). The text is measured in the widget's own font, so
// the long "区分大小写" and the short "Aa" form both get the right width.
// m086: the rule now belongs to DeckCheckButton (a 14px box + a 6px gap).
int FitCheckWidth(Fl_Button *aCheck, const char *aLabel)
{
	if (!aCheck || !aLabel)
		return 0;
	fl_font(aCheck->labelfont(), aCheck->labelsize());
	return (int)fl_width(aLabel) + kFindChkPad;
}

// m088: the shortcut spelling. One file, one syntax: the AHK spelling the
// script bindings already use ("^n", "+F3", "^!a"). These two helpers turn it
// into FLTK's own form (the key in the low bits, the modifiers in the high
// bits) and back.
//
// FLTK matches a lower-case letter against the key event and an upper-case
// one against Shift+letter, so the case follows the Shift modifier.
int FltkShortcutFromAhk(const std::string &aText)
{
	if (aText.empty())
		return 0;
	unsigned int mods = 0;
	unsigned int key = 0;
	std::string canonical;
	std::string error;
	if (!ParseMenuShortcut(aText, mods, key, canonical, error))
		return 0;
	int out = 0;
	if (mods & kHotkeyControl)
		out |= FL_CTRL;
	if (mods & kHotkeyAlt)
		out |= FL_ALT;
	if (mods & kHotkeyShift)
		out |= FL_SHIFT;
	if (mods & kHotkeyWin)
		out |= FL_META;
	if (key >= 'A' && key <= 'Z')
		out |= (mods & kHotkeyShift) ? (int)key : (int)(key - 'A' + 'a');
	else if (key >= 0x70 && key <= 0x7B) // VK_F1 .. VK_F12
		out |= (int)(FL_F + 1 + (key - 0x70));
	else if (key == 0x2E) // VK_DELETE
		out |= FL_Delete;
	else
		out |= (int)key;
	return out;
}

// The other direction: FLTK's shortcut form in the AHK spelling the file and
// the dialog use.
std::string AhkFromFltkShortcut(int aShortcut)
{
	if (!aShortcut)
		return std::string();
	unsigned int mods = 0;
	if (aShortcut & FL_CTRL)
		mods |= kHotkeyControl;
	if (aShortcut & FL_ALT)
		mods |= kHotkeyAlt;
	if (aShortcut & FL_SHIFT)
		mods |= kHotkeyShift;
	if (aShortcut & FL_META)
		mods |= kHotkeyWin;
	const unsigned int key = (unsigned int)(aShortcut & 0xffff);
	std::string token;
	if (key >= (unsigned int)(FL_F + 1) && key <= (unsigned int)(FL_F + 12))
		token = "F" + std::to_string(key - (unsigned int)FL_F);
	else if (key == (unsigned int)FL_Delete)
		token = "Del";
	else if (key >= 'a' && key <= 'z')
		token = std::string(1, (char)key);
	else if (key >= 'A' && key <= 'Z')
		token = std::string(1, (char)key);
	else if (key >= '0' && key <= '9')
		token = std::string(1, (char)key);
	else
		return std::string();
	std::string text;
	if (mods & kHotkeyControl)
		text += "^";
	if (mods & kHotkeyAlt)
		text += "!";
	if (mods & kHotkeyShift)
		text += "+";
	if (mods & kHotkeyWin)
		text += "#";
	return text + token;
}

// "文件/新建" -> "文件 > 新建": the shortcut dialog's name for a menu item.
std::string PathLabel(const char *aPath)
{
	std::string out;
	const char *at = aPath;
	while (at && *at)
	{
		const char *slash = std::strchr(at, '/');
		const size_t length = slash ? (size_t)(slash - at) : std::strlen(at);
		if (!out.empty())
			out += " > ";
		out.append(at, length);
		if (!slash)
			break;
		at = slash + 1;
	}
	return out;
}

// m089: whitelist of edit actions for the right-click menu (the
// IsContextEditItem filter): the everyday editing actions, judged by the item's own
// shortcut. Find / Replace carry keys of their own and stay in the menu bar.
bool IsContextEditItem(const Fl_Menu_Item &aItem)
{
	if (!aItem.text)
		return false;
	const int s = aItem.shortcut();
	if (s == 0)
		return true;
	return s == (FL_CTRL + 'z') || s == (FL_CTRL + 'y') ||
		s == (FL_CTRL + 'x') || s == (FL_CTRL + 'c') ||
		s == (FL_CTRL + 'v') || s == FL_Delete ||
		s == (FL_CTRL + 'a');
}

// Right-align a row of console actions inside the bar (fit-to-label rule):
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

// Filter box with a left magnifier (m031). The custom boxtype insets the
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
	// draw a second border of its own (dialog search outside the pane keeps
	// its border).
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

// m077: the one alert red now lives in Theme.cpp (m071), next to its siblings.


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

// m112: the 6px band between two panes is a grab handle. It is exactly the gap
// the panes already leave, so the drag target costs no layout space; a hover
// or drag tints it, the cursor flips to the matching double arrow. The widget
// never decides the split itself: it reports the dragged edge in window
// coordinates and the owner clamps it, recomputes the share and calls Layout.
class DeckSplitter : public Fl_Widget
{
public:
	enum Axis
	{
		Column, // a vertical band; drags left/right and splits widths
		Row     // a horizontal band; drags up/down and splits heights
	};

	explicit DeckSplitter(Axis aAxis)
		: Fl_Widget(0, 0, 0, 0, nullptr), mAxis(aAxis)
	{
		box(FL_NO_BOX);
	}

	// Grip limits in the widget's own coordinate axis (window/client coords).
	void bounds(int aMin, int aMax)
	{
		mMin = aMin;
		mMax = aMax;
	}

	std::function<void(int)> onMove;   // clamped edge coordinate
	std::function<void()> onRelease;   // persist the resulting share

	void draw() override
	{
		// m114: the band owns no colour of its own. It sits in the gap the two
		// panes already leave empty, so the window background must show through
		// exactly as it did before the gap was draggable. Filling it with a theme
		// colour painted a bright stripe between the panes, and the green hover
		// fill left a trail of stale bands while the split moved. Only the cursor
		// signals the grab now.
	}

	int handle(int aEvent) override
	{
		switch (aEvent)
		{
		case FL_ENTER:
			fl_cursor(mAxis == Column ? FL_CURSOR_WE : FL_CURSOR_NS);
			return 1;
		case FL_LEAVE:
			fl_cursor(FL_CURSOR_DEFAULT);
			return 1;
		case FL_PUSH:
			if (Fl::event_button() != FL_LEFT_MOUSE)
				return 0;
			mDrag = true;
			// Root coordinates: Layout() moves this widget under the pointer on
			// every step, so widget-relative coordinates would feed back.
			mGrab = mAxis == Column ? Fl::event_x_root() : Fl::event_y_root();
			mEdgeStart = mAxis == Column ? x() : y();
			return 1;
		case FL_DRAG:
		{
			if (!mDrag)
				return 0;
			const int cur = mAxis == Column ? Fl::event_x_root() : Fl::event_y_root();
			int edge = mEdgeStart + (cur - mGrab);
			if (edge < mMin)
				edge = mMin;
			if (edge > mMax)
				edge = mMax;
			if (onMove)
				onMove(edge);
			return 1;
		}
		case FL_RELEASE:
			if (!mDrag)
				return 0;
			mDrag = false;
			if (onRelease)
				onRelease();
			return 1;
		default:
			return Fl_Widget::handle(aEvent);
		}
	}

private:
	Axis mAxis;
	bool mDrag = false;
	int mGrab = 0;
	int mEdgeStart = 0;
	int mMin = 0;
	int mMax = 1 << 30;
};

class DeckWindow : public Fl_Double_Window
{
public:
	explicit DeckWindow(Engine &aEngine);
	~DeckWindow() override;

	void resize(int x, int y, int w, int h) override;
	int handle(int aEvent) override;
	void draw() override;

	// The client area is what the layout speaks; with the deck frame the
	// native window is 1px wider and taller than it (the 1px border lives in
	// the non-client strip). Ask Win32 rather than guessing what FLTK did with
	// the outer size it was given.
	int ClientWidth() const;
	int ClientHeight() const;

	DeckTitleBar *TitleBarWidget() const { return mTitleBar; }
	// The custom caption buttons route here: close keeps v1.3's hide-to-tray
	// meaning, never DestroyWindow.
	void TitleClose();
	// Called once after show(): installs the deck frame + shadow.
	void InstallFrame();

	// m084: FLTK's own error channel writes here (ui/FltkErrorChannel.h). The
	// messages join the console column instead of opening a modal dialog.
	void ReportLibraryError(const std::string &aMessage)
	{
		Append("[fltk] " + aMessage);
	}

	// Every method below names a script by its index in mScripts, not by the line
	// the user sees: filtering and sorting make those two differ (m024). Use
	// IndexOfLine()/SelectedIndex() at the widget boundary and LineOfIndex() to
	// drive the browser itself.
	void ShowContextMenu(int aIndex);
	// m089: the editor's right-click menu, assembled from the live menu bar
	// (Rule: no second list - a new menu item shows up by itself).
	void ShowEditorContextMenu();
	void EditRow(int aIndex);
	// The per-row 菜单 cell calls this, so it is not a private detail.
	void OpenScriptMenu(int aIndex); // ask the script to pop its own tray menu
	// Called by the browser when its inline hotkey cell records a combo.
	void CommitInlineHotkey(int aIndex, const std::string &aText);

	// Settings > 快捷键: write the dialog's whole table in one go (OK).
	bool ApplyShortcutRows(const std::vector<std::pair<std::string, std::string>> &aRows);

	// m033: Settings > Shortcuts dialog. It lists every script and records a
	// combo in a cell, the way this shortcut settings page does.
	int ScriptCount() const { return (int)mScripts.size(); }
	std::string ScriptNameAt(int aIndex) const;
	std::string ScriptBindingAt(int aIndex) const;
	void ShowShortcutDialog();
	// m092: Help > About / Check for Updates.
	void ShowUpdateDialog();
	// m146: File > 获取脚本… - the official script downloader.
	void ShowScriptDownloadDialog();
	// m151: the row menu's 信息 window - read-only, built from the script's own
	// header block plus the facts only the disk knows. Public for the same
	// reason the two above are: the screenshot hook opens it without a gesture.
	void ShowScriptInfo(int aIndex);
	// m177: 设置 > 选项… - public for the screenshot hook, like the three above.
	void ShowOptions();
	// m178: 帮助 > 关于 - same reason.
	void ShowAbout();
	// m180: 获取脚本 > 订阅 - the subscription list on its own. Public for the
	// screenshot hook, like the four above.
	void ShowSubscribeManager();
	// m097/m098: Help > MCP 接入 - opens docs\MCP接入.md next to the exe.
	// A dialog would only be a second copy of what that file already says.
	void OpenMcpDocs();
	bool ConfirmRestartForUpdate();
	// m088: the command table, read by the shortcut dialog (the rows it
	// lists) and by the key dispatcher (the editor asks first, the window
	// second). The table is the only place a command is named.
	int MenuCommandCount() const { return (int)Commands().size(); }
	std::string MenuCommandName(int aIndex) const;     // "菜单 > 文件 > 新建"
	std::string MenuCommandKey(int aIndex) const;      // "new"
	std::string MenuCommandBinding(int aIndex) const;  // the binding in force
	std::string MenuCommandDefault(int aIndex) const;  // the built-in binding
	// m108: the "show the main window" shortcut (the tray menu's 主窗口 action).
	// "" = unbound; there is no built-in default.
	std::string ShowWindowBinding() const;
	// Runs the command whose binding matches the key that is being pressed.
	int DispatchShortcut();
	// m082 gate hook: the only way a tool-driven gate can put a Scripts file
	// into the editing area without a user gesture (every other path to
	// LoadIntoEditor is a UI action, and that one stays private on purpose).
	void OpenScriptForGate(const std::string &aLeaf);

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
	// m087: fold / unfold the find bar. The editor takes the bar's room
	// back when it is folded, and every way in (Ctrl+F, Ctrl+H, F3, the
	// 编辑 menu) unfolds it again.
	void SetFindBarVisible(bool aVisible);
	// m089: the find bar is a toggle - the same menu item (or key) brings it
	// up and takes it away again. The find/replace callbacks do exactly this.
	void ToggleFindBar(bool aFocusReplace);
	// The menu items carry a check mark while the bar is on screen.
	void SyncFindMenuState();
	void FindNextFromShortcut() { FindNext(); }
	// m085: Shift+F3 walks back through the matches.
	void FindPrevFromShortcut() { FindPrev(); }
	// Double-click on a row is a start/stop switch, exactly like v1.3's ListView
	// double-click and like the bound hotkey: one gesture, one meaning (m023).
	void RunOrStopRow(int aIndex) { ToggleRow(aIndex); }

	// m064: the MCP endpoint. It runs on its own thread inside this process, so
	// the client that spawned us (WorkBuddy / Qoder, both configured through
	// {"mcpServers":{"<name>":{"command","args","env"}}} - stdio, no url) drives
	// the very window the user is looking at: same editing area, same sessions,
	// same "宿主关闭 = 脚本全关". Only "AHKDeck.exe --mcp-stdio" starts it.
	void StartMcp();
	// m117: --mcp-stdio starts with the window hidden to the tray. An agent
	// client calls tools frequently, and a deck window in front of the user for
	// every call is exactly what should not happen; the tray icon stays the way
	// back in. Without a tray icon the window stays visible - a hidden window
	// with no way back is worse than a visible one.
	void StartMcpBackground();
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
		// m172: 脚本/编译为 EXE - the official compiler ships under tools\
		// and runs as a short-lived child; see core/ExeCompiler.h.
		IdCompileExe,
		IdHideTray,
		IdRunAll,
		IdOpenScriptsFolder,
		IdOpenConfigFile,
		IdEditSelected,
		IdAbout,
		// m092: Help > Check for Updates (About moved into the same group).
		IdCheckUpdate,
		// m097: MCP access lives next to 检查更新 - both are "help me use this
		// from outside the deck".
		IdMcpAccess,
		// m088: the 编辑 menu and the commands that own a
		// button but no menu entry (the middle band, the editor's own reload).
		IdEditUndo,
		IdEditRedo,
		IdEditCut,
		IdEditCopy,
		IdEditPaste,
		IdEditDelete,
		IdEditSelectAll,
		IdFindNext,
		IdFindPrev,
		IdCloseScriptEdit,
		IdRunOrStopEdit,
		// m146: appended last so every existing command id keeps its value.
		IdScriptDownload,
		// m177: 脚本/删除 (row menu + menu bar) and 设置/选项….
		IdDeleteScript,
		IdOptions
	};

	void HandleMenu(int aId);
	void ApplyFrameOptions();

	// m088: ONE table for every command the deck has.
	//
	// The menu bar is built from it, the shortcut dialog lists it, the keys
	// are dispatched from it, and the user's rebinding is stored against its
	// stable key. Keep the same three things apart: the menu table
	// (MenuTable.cpp), the shortcut table (ShortcutCore.h) and the config.
	struct CommandSpec
	{
		int id;            // MenuId
		const char *key;   // stable id in the hotkey file ("menu.new")
		const char *path;  // menu path ("文件/新建"); "" = no menu entry
		const char *group; // the shortcut dialog's group ("菜单", "窗口 > 中部条带")
		const char *label; // the shortcut dialog's label (the item's own text)
		int shortcut;      // the default binding (an FLTK shortcut)
		int flags;         // menu flags (FL_MENU_TOGGLE ...)
		bool editorOnly;   // acts on the editor's text, not on the deck
	};
	static const std::vector<CommandSpec> &Commands();

	// m088: the shortcut dialog and the key dispatcher read the table
	// through these, so the table stays the only place a command is named.
	int ShortcutOf(int aId) const;
	// Writes the user's rebinding into the menu items, so the menu shows it.
	void ApplyMenuShortcuts();

	// m088: the 编辑 menu's commands, all on the editor's text.
	void EditUndo();
	void EditRedo();
	void EditCut();
	void EditCopy();
	void EditPaste();
	void EditDelete();
	void EditSelectAll();

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

	void OpenTempFolder();
	void EditSelectedExternally();
	void RefreshFromDisk();

	// --- m023: the v1.3 workbench actions -----------------------------------
	void ToggleRow(int aIndex); // run if stopped, stop if running
	void RestartRow(int aIndex);     // stop, then start once it really finished
	void BeginHotkeyEdit(int aIndex);
	void RebindHotkeys();
	void LoadHotkeyFile();
	void HandleIntent(const NativeHooks::Intent &aIntent);
	// m109: one startup pass over temp\ (设置/选项… picks the window, m177).
	void RunHousekeeping();
	// m108: the tray menu's 主窗口 action, shared by the tray click and the
	// window's global hotkey so both entries behave the same.
	void ShowFromTray();
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
	// m177: moves the selected script into temp\deleted (no dialog: the move is
	// recoverable, and the console names the file).
	void DeleteSelected();
	// m179: the official mirror and the subscribed catalogs, read once per
	// window that needs them. `download.source` is the current key; the two
	// per-dialog keys it replaced are still honoured when present.
	int OfficialDownloadSource() const;
	std::vector<CustomCatalogSource> CustomCatalogSources() const;
	// m180: one writer for scripts.custom. Both entry points (获取脚本 passes it
	// down to 订阅管理, and the screenshot hook opens 订阅管理 directly) must
	// produce the same JSON, so the shape is written here once.
	void SaveCustomSources(const std::vector<CustomCatalogSource> &aSources);
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
	// m172: row-index based (context menu hands the exact row, the menu item
	// passes SelectedIndex()). Spawns the official compiler on a worker thread
	// and reports through the console; aIndex < 0 = "pick a script first".
	void CompileSelected(int aIndex);
	// m173: arm the post-dialog working-set trim (pecila scheduleTrimSoon:
	// dialog open/close is a page fault the product just paid for).
	void ScheduleSaveTrim();
	static void SaveTrimCb(void *aSelf);
	void CompileExeOnWorker(std::wstring scriptPath, std::wstring outPath, std::string name);
	void ApplyCompileResult(const ExeCompileOutcome &aOutcome, const std::string &aName);
	bool EditingScriptRunning(DWORD *aSession = nullptr) const;
	void RunOrStopEditing();
	void SyncEditorRunButton();
	void ReloadEditor();
	void SyncEditorActions();
	void FindNext();
	// m085: the other direction, and the shared "select match #n" step that
	// keeps the "k/N" counter honest.
	void FindPrev();
	void SelectMatchAt(int aIndex);
	void ReplaceOne();
	void ReplaceAll();
	// m084: the live match counter behind the find bar.
	void UpdateFindMatches();
	void UpdateFindCount();
	// m086: the whole-word switch. FLTK's search_forward has no such option,
	// so the boundary check is ours.
	bool IsWholeWordMatch(int aPos, int aLength) const;
	int SearchForward(int aStart, const char *aNeedle, int aMatchCase,
		bool aWholeWord, int *aFound) const;
	void OnScriptWritten(const std::string &aUtf8Path);
	void ApplyToolEditorContent(const std::string &aText);
	void ApplyWidgetTheme();
	void ApplyView();
	void SyncViewMenu();
	void UpdateLineNumberWidth();
	void EnsureEditorVisible();
	void LayoutEditorPane(int x, int y, int w, int h);
	void LayoutListPane(int x, int y, int w, int h);
	// m083: the find/replace bar under the editor - two rows, because the
	// editor column is too narrow for six controls in one line.
	void LayoutFindBar(int x, int y, int w, int rowH);

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

	static void OnLogs(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->OpenTempFolder(); }
	static void OnClear(Fl_Widget *, void *aSelf);
	static void OnCopy(Fl_Widget *, void *aSelf);
	static void OnSelect(Fl_Widget *, void *aSelf);
	static void OnSave(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->SaveEditor(); }

	static void OnSaveRun(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->SaveAndRun(); }
	static void OnRunOrStop(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->RunOrStopEditing(); }
	static void OnCloseScript(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->CloseScript(); }
	static void OnEditorReload(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->ReloadEditor(); }
	static void OnFindNext(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->FindNext(); }
	// m085: the backward search.
	static void OnFindPrev(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->FindPrev(); }
	// m087: the find bar's switch - fold the bar away.
	static void OnFindClose(Fl_Widget *, void *aSelf)
	{
		static_cast<DeckWindow *>(aSelf)->SetFindBarVisible(false);
	}
	static void OnReplaceOne(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->ReplaceOne(); }
	static void OnReplaceAll(Fl_Widget *, void *aSelf) { static_cast<DeckWindow *>(aSelf)->ReplaceAll(); }
	// m084: the find field reports both "the text changed" and "Enter was
	// pressed". The counter follows the first, the search follows the second.
	static void OnFindInput(Fl_Widget *aWidget, void *aSelf);
	static void OnMatchCase(Fl_Widget *, void *aSelf)
	{
		static_cast<DeckWindow *>(aSelf)->UpdateFindMatches();
	}
	static void OnMenu(Fl_Widget *aWidget, void *aData);
	static void OnTick(void *aSelf);
	static void TrimWorkingSetCb(void *aSelf);
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

	BlinkInput *mFindInput = nullptr;
	BlinkInput *mReplaceInput = nullptr;
	Fl_Button *mFindNext = nullptr;
	// m085: the bar has both directions. "上一个" walks back through the
	// matches, "下一个" walks forward.
	Fl_Button *mFindPrev = nullptr;
	Fl_Button *mReplaceOne = nullptr;
	Fl_Button *mReplaceAll = nullptr;
	// m085: the case toggle is a real check box with a text label, not a
	// light button with a corner mark (m084's "Aa" looked broken).
	// m086: the switches are drawn exactly like the menu bar's check mark.
	DeckCheckButton *mMatchCase = nullptr;
	// m085: the wrap switch - without it "下一个" silently jumped back to the
	// top, which is exactly the "循环搜索" the user asked for.
	DeckCheckButton *mWrap = nullptr;
	// m086: "全词匹配" - the match must be a whole word.
	DeckCheckButton *mWholeWord = nullptr;
	// m085: the grey band the bar sits on (FL_FLAT_BOX, 200/200/200).
	Fl_Box *mFindBarBg = nullptr;
	// m084: the centred match counter ("k/N") at the end of the first row.
	Fl_Box *mFindCount = nullptr;
	// m087: the find bar's switch - "X" folds the bar away, Ctrl+F (or the
	// 编辑 menu, or F3) brings it back. The bar is the only part of the
	// editor pane the user may hide, because it is the only part that is
	// not the text.
	Fl_Button *mFindClose = nullptr;
	// m089: the bar starts FOLDED. The 编辑 > 查找… / 替换…
	// entries are switches now: press once and the bar comes up, press
	// again and it goes away. A bar that was always on screen could not be
	// "pressed again to hide" - the first press would hide a bar the user
	// never asked for.
	bool mFindBarVisible = false;
	// Every widget the find bar owns, so the bar can fold and unfold as one.
	std::vector<Fl_Widget *> mFindBarWidgets;
	std::vector<int> mMatchPositions; // where the needle matched, in order
	int mMatchTotal = 0;
	int mMatchIndex = 0; // 1-based index of the selected match, 0 if none
	bool mFindMatchesDirty = true;


	// m063/m064: the MCP side gets its own Toolbox instance on purpose - each
	// client keeps its own script-id table, while the tool list and every tool
	// implementation live once, in agent/Toolbox.cpp.
	agent::Toolbox mMcpTools;
	std::thread mMcpThread;
	std::atomic<bool> mMcpRunning{false};
	bool mMcpStarted = false;
	// m117: set only by the background start; the endpoint's EOF then quits the
	// deck, because a hidden window with no client is an invisible orphan.
	bool mMcpBackground = false;

	// FLTK text buffers may only be read on the UI thread, so tools never touch
	// the editor: the tick copies the editing area into this snapshot and the
	// tool thread reads the copy.
	std::mutex mSnapshotMutex;
	std::string mEditorSnapshot;
	// m062: the backing file travels with the snapshot, so run_editor resolves
	// A_ScriptDir from the same moment the text came from.
	std::wstring mEditorPathSnapshot;
	long mSnapshotChars = -1;
	std::string mSnapshotKey;
	std::string EditorSnapshot();
	std::wstring EditorPathSnapshot();
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
	// m112: the script list is the deck's spine and is always shown; only the
	// editor/console column can be folded away.
	// m176: a fresh install starts folded. Double-click on a row runs/stops the
	// script (m023) and never touches the editor, so the fold costs nothing
	// there; EnsureEditorVisible() still pulls the column out for the six
	// actions that genuinely need it (new / open / find / replace / tool write).
	// A user's saved settings.json wins over this default either way.
	bool mShowEditor = false;
	// m112: the two draggable shares (percent). 50/25 reproduce the old fixed
	// equal-columns / quarter-console layout exactly, so nothing moves on a
	// fresh install.
	int mListPct = 50;
	int mConsolePct = kConsoleOutputPercent;
	DeckSplitter *mSplitV = nullptr;
	DeckSplitter *mSplitH = nullptr;
	// The editor column's inner body (top and height) as last laid out, so the
	// row splitter can turn a dragged y into a console share.
	int mBodyTop = 0;
	int mBodyH = 0;
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
	// m107: the host's own footprint, refreshed every 2 s by OnTick and
	// appended to the status line. The sampler keeps its previous reading, so
	// each refresh is a delta, not a lifetime total.
	ResourceSampler mResourceSampler;
	std::string mResourceText;
	ULONGLONG mLastResourceCheck = 0;
	bool mUpdatingList = false;
	std::map<DWORD, SessionState> mSeenState;
	std::map<DWORD, std::string> mSessionName;
	std::map<DWORD, bool> mExitReported;

	// m110: the moment the last script session ends, the kernel's load-time
	// allocations (parser tables, class objects, per-thread stacks) are still
	// resident in the working set. A one-shot trim 2 s later hands those pages
	// back to the OS: the status line's memory number falls back to the idle
	// footprint without touching commit size or any live session. Two guards:
	// the timeout is one-shot and removed on teardown (no UAF), and the callback
	// re-checks that nothing started during the 2 s window before trimming.
	bool mHadRunning = false;
	bool mTrimPending = false;
	bool mSaveTrimPending = false; // m173: the save-dialog one-shot (same shape)

	std::wstring mEditPath;
	std::wstring mSettingsPath;
	Json mConfigRoot; // full settings.json, unknown fields preserved
	bool mEditDirty = false;
	bool mEditLoading = false;
	bool mEditIsNew = false;
	// m082: content a tool (edit_editor) applied and the user has not touched
	// yet. While set, the timed save must not persist it - the tool promised
	// "not saved", and only a keystroke or an explicit save lifts the hold.
	bool mToolEditPending = false;
	// The gutter width has to be recomputed after an edit, but not from inside
	// the buffer's own modified callback: FLTK is halfway through the change
	// there, and a query (count_lines -> position_to_line) trips its
	// consistency check ("FI_Tex_Displayposition_to_line: Consistency check
	// pt failed"). The next tick does it instead (m083).
	bool mLineNumberDirty = false;
	DeckDialog *mNameDialog = nullptr;
	ULONGLONG mLastAutoSave = 0;
	int mAutoSaveMs = 30000; // AHKDECK_AUTOSAVE_MS can shrink this (gate only)
	ULONGLONG mLastTrayCheck = 0;

	// m023: tray + global hotkeys live in Win32, the deck's state lives here.
	NativeHooks mHooks;
	std::vector<HotkeyEntry> mHotkeys;
	std::wstring mHotkeyPath;
	std::map<int, std::wstring> mHotkeyTargets; // hotkey id -> script path
	// m109: days kept by the startup cleanup; 0 = off (settings.json only).
	int mHousekeepingDays = kHousekeepingDefaultDays;
	std::string mLastTrayTip;                   // NIM_MODIFY only on change
	std::vector<Fl_Menu_Item> mContextItems;    // the row menu, rebuilt per popup
	std::vector<std::string> mContextLabels;    // ... and the text it points at
	bool mQuitRequested = false;
	std::string mKernelSummary; // "2.0.25", read once from the loaded kernel
	// m172: single-flight for 编译为 EXE - the child is short-lived but only
	// one compile may run at a time (shared .temp capture names, one console
	// voice). The shared flag outlives the window's queue: a Fl::awake job
	// posted just before 退出 must not hop into a dead sink.
	bool mCompileBusy = false;
	std::shared_ptr<bool> mCompileAlive = std::make_shared<bool>(true);
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
		DeckDrawScrollbar(&scrollbar, false);
		if (size() == 0)
			DrawEmptyHint(mOwner->ScriptCount() == 0);
		fl_pop_clip();
	}

	// m125: the release ships no scripts, so an empty list is the FIRST-RUN
	// state, not a failure. A blank pane never said that Scripts\ is where the
	// files belong; a filter that hides every row is a different sentence, and
	// the browser cannot tell them apart by size() alone.
	void DrawEmptyHint(bool aNoScriptsAtAll)
	{
		const int left = x();
		const int width = w();
		const int midY = y() + h() / 2;

		fl_font(FL_HELVETICA, SubTextSize());
		fl_color(DeckColors().text2);
		if (aNoScriptsAtAll)
		{
			fl_draw("还没有脚本", left, midY - 24, width, 20, FL_ALIGN_CENTER);
			fl_draw("把 .ahk 文件放进 Scripts 文件夹", left, midY, width, 20, FL_ALIGN_CENTER);
		}
		else
			fl_draw("没有匹配的脚本", left, midY - 10, width, 20, FL_ALIGN_CENTER);
	}

	// m032: click a hotkey cell and record the combo right there (the
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
					// The deck commits on the captured key itself; there is no Enter
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
				CancelHotkeyRecord(); // a click elsewhere cancels the recording
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

		// Combo cell: panel-coloured fill (background3) so it reads
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
			// Cover Fl_Browser's own text and redraw with the 8px inset.
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
			const std::string name = DisplayNameOf(mOwner->ScriptNameAt(mOwner->IndexOfLine(line)));
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
			const std::string name = DisplayNameOf(mOwner->ScriptNameAt(mOwner->IndexOfLine(line)));
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
			fl_color(RunningGreen());
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

// clipEllipsis: shorten a name to fit the row and end it
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

// m073: the Settings > Shortcuts dialog.
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
		std::string name;   // the row's name (the dialog's own wording)
		std::string combo;  // canonical AHK spelling, empty = unbound
		std::string key;    // the file's key: "menu.new" or the script's name
		std::string def;    // the built-in binding (menu rows only)
		bool isMenu = false;
		bool conflict = false;
	};

	static std::string Lower(std::string aText);

	// The same three rejection classes as the shared shortcutValidate, plus the
	// rule that comes from OUR scope: a script's binding is a system-wide
	// RegisterHotKey entry, so a bare key (even F5) would swallow that key in
	// every application. The deck can accept bare keys because its shortcuts are
	// in-window menu accelerators.
	// m088: a menu command IS in-window, so there a plain function key is
	// allowed - it only lives in this window.
	static bool ValidateCombo(int aKey, unsigned aMods, std::string &aCanonical,
		std::string &aError, bool aMenu = false);

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
	DeckScroll *mScroll = nullptr;
	SearchInput *mFilter = nullptr;
	Fl_Box *mFilterHint = nullptr;
	Fl_Button *mResetBtn = nullptr;
	Fl_Button *mOkBtn = nullptr;
	Fl_Button *mCancelBtn = nullptr;
	Fl_Box *mTopSpacer = nullptr;
	Fl_Box *mLegend = nullptr;
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
	// m116: a system-wide binding (RegisterHotKey) reads red; a menu command is
	// an in-window accelerator and keeps the normal text colour. m117: the NAME
	// is red for every global row from the start (bound or not), so the scope is
	// visible before any key is recorded; the shortcut text turns red only once
	// a combo exists ("（无）" stays normal).
	void setGlobal(bool aGlobal, bool aBound)
	{
		mGlobal = aGlobal;
		mBound = aBound;
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
	bool mComboHover = false;
	bool mGlobal = false;
	bool mBound = false;
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
	const int pad = 16;      // dialog padding: above and below the rows
	const int searchH = 22;
	const int iconW = 24;
	const int filterTop = top + 560 - 2 * barH;
	const int btnTop = filterTop + barH;

	begin();

	// The list owns the whole pane: flush against the caption above and the
	// filter bar below. Rows are drawn inside it, not as a second frame.
	mScroll = new DeckScroll(0, top, 520, filterTop - top);
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
	mFilter->textsize(TextSize());
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
	mFilterHint->labelsize(TextSize());
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
		button->labelsize(TextSize());
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
	// A click on empty dialog space also cancels the recording; row
	// widgets consume their own clicks and stop it there.
	if (aEvent == FL_PUSH && mRecording)
	{
		StopRecording();
		UpdateHintText();
	}
	// The dialog-wide OK shortcut (the default combo): Ctrl+Enter confirms
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
	std::string &aCanonical, std::string &aError, bool aMenu)
{
	if ((aMods & FL_CTRL) && (aMods & FL_ALT) && aKey == FL_Delete)
	{
		aError = "Ctrl+Alt+Del 已被系统保留";
		return false;
	}
	const unsigned mods = aMods & (FL_CTRL | FL_SHIFT | FL_ALT | FL_META);
	if (mods == 0)
	{
		// m117: a plain function key (F1-F12) is a deliberate press and never
		// something typed into a field, so both scopes may bind it bare. Delete
		// stays menu-only: a system-wide Delete would eat deletion everywhere.
		const bool plainFunctionKey = aKey >= FL_F + 1 && aKey <= FL_F + 12;
		const bool plainDelete = aMenu && aKey == FL_Delete;
		if (!plainFunctionKey && !plainDelete)
		{
			aError = aMenu
				? "菜单快捷键至少要有一个修饰键，或用 F1-F12 / Del 这类功能键"
				: "全局快捷键至少要有一个修饰键（Ctrl / Alt / Shift / Win），或使用功能键 F1-F12";
			return false;
		}
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
	// m088: parse in the scope the row belongs to. m117: both scopes now accept
	// a bare function key, so the system parser is the one that decides.
	return aMenu
		? ParseMenuShortcut(text, parsedMods, parsedKey, aCanonical, aError)
		: ParseAhkHotkey(text, parsedMods, parsedKey, aCanonical, aError);
}

bool ShortcutDialogWindow::SameBinding(const std::string &aLeft, const std::string &aRight)
{
	return SameShortcut(aLeft, aRight);
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
	// m116: one red note above the list, explaining why some rows are red.
	// FL_FLAT_BOX (not FL_NO_BOX) so the text clears what it paints over: a
	// label-only box composites onto the previous frame and darkens.
	mLegend = new Fl_Box(FL_FLAT_BOX, 24, mScroll->y(), 520 - 48, 22,
		"红色为全局快捷键");
	mLegend->color(DeckColors().background1);
	mLegend->labelcolor(DeckColors().highlight3);
	// Same 16px as the row names: one note in a second size would read as the
	// very "inconsistent font" the rows were just fixed for.
	mLegend->labelsize(TextSize());
	mLegend->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	// m108: the window's own action first: it is the one binding that works
	// while the window is hidden. isMenu stays false because this row is a
	// system-wide RegisterHotKey entry, like a script's (m117: a bare F-key is
	// allowed there, a bare letter is not).
	{
		RowState state;
		state.isMenu = false;
		state.key = "window.show";
		state.name = "窗口 > 显示主窗口";
		state.combo = mDeck ? mDeck->ShowWindowBinding() : std::string();
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
		row->setIndex((int)mRows.size() - 1);
		row->setName(state.name);
		row->setComboText(ComboText(state.combo));
		row->setGlobal(true, !state.combo.empty());
		mWidgets.push_back(row);
	}
	// m088: the menu commands first, in the table's order. Their name shows
	// the group, then the path ("菜单 > 文件 > 新建"). The script
	// switches come after them ("开关 > 脚本名").
	const int menuCount = mDeck ? mDeck->MenuCommandCount() : 0;
	for (int i = 0; i < menuCount; ++i)
	{
		RowState state;
		state.isMenu = true;
		state.key = "menu." + mDeck->MenuCommandKey(i);
		state.name = mDeck->MenuCommandName(i);
		state.def = mDeck->MenuCommandDefault(i);
		state.combo = mDeck->MenuCommandBinding(i);
		mRows.push_back(state);
		RowWidget *row = new RowWidget(12, 0, 520 - 24, 26);
		row->setDialog(this);
		row->setIndex((int)mRows.size() - 1);
		row->setName(state.name);
		row->setComboText(ComboText(state.combo));
		mWidgets.push_back(row);
	}
	const int count = mDeck ? mDeck->ScriptCount() : 0;
	for (int i = 0; i < count; ++i)
	{
		RowState state;
		state.isMenu = false;
		state.key = mDeck->ScriptNameAt(i);
		state.name = "开关 > " + state.key;
		state.combo = mDeck->ScriptBindingAt(i);
		// The file is hand-editable ("^!a"), so normalize the spelling: parse
		// and re-serialize on load. Unparseable text is kept
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
		row->setIndex((int)mRows.size() - 1);
		row->setName(state.name);
		row->setComboText(ComboText(state.combo));
		row->setGlobal(true, !state.combo.empty());
		mWidgets.push_back(row);
	}
	mBottomSpacer = new Fl_Box(FL_NO_BOX, 0, 0, 0, aPad, nullptr);
	mScroll->end();
	RepositionRows();
	RecomputeConflicts();
}

// Visible rows stack from the top with the 26px/2px rhythm; the top and
// bottom spacers keep dialog_pad at both ends of the scrollable content.
void ShortcutDialogWindow::RepositionRows()
{
	if (!mScroll)
		return;
	const int pad = 16;
	const int topPad = 8;
	const int noteH = 22;
	if (mTopSpacer)
		mTopSpacer->resize(0, mScroll->y(), 0, topPad);
	int y = mScroll->y() + topPad;
	// m116: the red note keeps its own line above the rows, so the first row
	// starts noteH + 6px lower than it did before.
	if (mLegend)
	{
		mLegend->resize(24, y, w() - 48, noteH);
		y += noteH + 6;
	}
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
		y += RowStep();
		lastRowBottom = y - 2;
	}
	if (mBottomSpacer)
		mBottomSpacer->resize(0, lastRowBottom, 0, pad);
	mScroll->redraw();
}

// Filter: a lowercase substring of the name or of the shortcut text.
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

// Same rule as the find bar: the hint is visible only while the input
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
	{
		mWidgets[i]->setComboText(ComboText(mRows[i].combo));
		// A row that just got (or lost) a combo changes between red and normal.
		mWidgets[i]->setGlobal(!mRows[i].isMenu, !mRows[i].combo.empty());
	}
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
	const int index = aRow->index();
	if (index < 0 || (size_t)index >= mRows.size())
		return;
	// m088: a script's binding is a system-wide RegisterHotKey entry; a menu
	// command only lives in this window. m117: ValidateCombo now allows a bare
	// function key in both scopes, so the difference is only the parser's scope.
	const bool isMenu = mRows[(size_t)index].isMenu;
	std::string canonical;
	std::string error;
	if (!ValidateCombo(aKey, aMods, canonical, error, isMenu))
	{
		SetError(error.c_str());
		return; // keep recording
	}
	// New binding wins: the action that already owned the combo is cleared,
	// exactly as scoped (one combo per action, no silent duplicates).
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
	// m088: a menu command goes back to its built-in binding; a script has no
	// built-in shortcut, so its default is "unbound".
	for (RowState &state : mRows)
		state.combo = state.isMenu ? state.def : std::string();
	RebuildComboLabels();
	RecomputeConflicts();
	SetError(nullptr);
}

void ShortcutDialogWindow::FinishOk()
{
	std::vector<std::pair<std::string, std::string>> rows;
	rows.reserve(mRows.size());
	for (const RowState &state : mRows)
		rows.push_back({ state.key, state.combo });
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
	// Only the shortcut cell reacts to the pointer, the same way the menu row tints
	// its combo box: the row itself stays flat (m093).
	if (aEvent == FL_ENTER || aEvent == FL_MOVE || aEvent == FL_LEAVE)
	{
		bool inCombo = false;
		if (aEvent != FL_LEAVE)
		{
			const int mx = Fl::event_x();
			const int my = Fl::event_y();
			inCombo = mx >= ComboX() && mx < ComboX() + kComboW &&
				my >= y() && my < y() + h();
		}
		if (inCombo != mComboHover)
		{
			mComboHover = inCombo;
			redraw();
		}
		return 1;
	}
	return Fl_Box::handle(aEvent);
}

void ShortcutDialogWindow::RowWidget::draw()
{
	Fl_Box::draw();
	const ThemeColors &colors = DeckColors();

	// m116: the row owns its background. FLTK repaints a child without repainting
	// its parent, and GDI draws antialiased text with a transparent background,
	// so a row redrawn on its own (hover, conflict, a recorded combo) blended the
	// glyphs onto the previous frame - after a few repaints the row looked
	// bolder than its neighbours. Clearing first makes every paint identical.
	fl_color(colors.background1);
	fl_rectf(x(), y(), w(), h());

	// Name: red for every system-wide row, even before a key is bound - the
	// colour is what tells the user "this binding works outside the deck".
	const bool globalBound = mGlobal && mBound;
	fl_color(mGlobal ? colors.highlight3 : colors.text1);
	fl_font(FL_HELVETICA, 16);
	const int avail = w() - kComboW - 32;
	const std::string clipped = ClipEllipsis(mName, avail - 12);
	fl_draw(clipped.c_str(), x() + 12, y(), avail, h(),
		FL_ALIGN_LEFT | FL_ALIGN_CENTER, nullptr, 0);

	// Combo: bordered, panel fill; hover and recording get the accent fill,
	// a conflict turns both border and text red.
	const int cx = ComboX();
	const int cy = y() + 1;
	const int ch = h() - 2;
	fl_color((mRecording || mComboHover) ? colors.highlight1
		: colors.background3);
	fl_rectf(cx, cy, kComboW, ch);
	fl_color(mConflict ? colors.highlight3 : colors.text1);
	fl_rect(cx, cy, kComboW, ch);
	fl_color(mRecording ? colors.text1
		: (mConflict || globalBound ? colors.highlight3 : colors.text1));
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
		label->labelsize(TextSize());
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
		mHint->labelsize(SubTextSize());
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
		DeckDrawScrollbarCorner(mVScrollBar, mHScrollBar);
		DeckDrawScrollbar(mVScrollBar, false);
		DeckDrawScrollbar(mHScrollBar, true);
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
// hosts the in-place IME composition so pinyin input draws ONE caret at
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
		// The horizontal bar is resized to the widget's left edge in draw():
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
		DeckDrawScrollbarCorner(mVScrollBar, mHScrollBar);
		DeckDrawScrollbar(mVScrollBar, false);
		DeckDrawScrollbar(mHScrollBar, true);
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
		if (aEvent == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE)
		{
			// m089: the editor's right-click menu (from the editor and the
			// preview share one menu, assembled from the live menu bar).
			mOwner->ShowEditorContextMenu();
			return 1;
		}
		if (aEvent == FL_PASTE)
		{
			// m088: the 编辑 > 粘贴 command asks FLTK for the clipboard and
			// FLTK hands it over here.
			const char *text = Fl::event_text();
			if (text && *text)
				insert(text);
			return 1;
		}
		// m088: every key the user bound comes from the command table, so a
		// re-bound key behaves exactly like the built-in one. The editor asks
		// first: it is the focus owner while the text is being edited.
		if ((aEvent == FL_KEYDOWN || aEvent == FL_SHORTCUT) && mOwner->DispatchShortcut())
			return 1;
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

// m088: the command table. The order is the menu order (File, Edit, View,
// Script, Settings - Edit sits right after File), and the entries
// without a menu path are the commands that own a button but no menu item
// (the middle band's four buttons, the editor's own reload).
//
// m118: only the keys a user already knows from Windows Notepad stay enabled
// by default - Ctrl+N / Ctrl+S, Ctrl+Z / Ctrl+Y / Ctrl+X / Ctrl+C / Ctrl+V /
// Ctrl+A, Del, Ctrl+F, F3, Shift+F3, Ctrl+H. They cover 文件 and 编辑, the two
// groups whose behaviour matches Notepad exactly, and muscle memory is worth
// more than a longer default list. Every other command starts unbound: the
// tray, the buttons and the menu still reach it, and 设置 > 快捷键 binds it.
const std::vector<DeckWindow::CommandSpec> &DeckWindow::Commands()
{
	static const std::vector<CommandSpec> specs = {
		{IdNew, "new", "文件/新建", "菜单", "新建", FL_CTRL + 'n', 0, false},
		// m090: the two file-related buttons of the middle band are menu items
		// too, so a user can reach them without the button and the dialog lists
		// them under 文件 instead of "窗口 > 中部条带".
		{IdSave, "save", "文件/保存", "菜单", "保存", FL_CTRL + 's', 0, false},
		{IdCloseScriptEdit, "closeScript", "文件/关闭脚本", "菜单", "关闭脚本", 0, 0, false},
		// m149: the sub-menu verb is 打开, so the items are just the places
		// (临时文件夹 / 配置文件) instead of repeating 打开 twice. The script
		// folder is NOT here - its object is the script library, so it sits
		// with the other library actions under 脚本.
		// m177: the folder is temp\ now, not logs\ - it holds every run-time
		// artefact (logs, backups, scratch, deleted scripts).
		{IdLogs, "tempFolder", "文件/打开/临时文件夹", "菜单", "临时文件夹", 0, 0, false},
		{IdOpenConfigFile, "openConfig", "文件/打开/配置文件", "菜单", "配置文件", 0, 0, false},
		{IdQuit, "quit", "文件/退出", "菜单", "退出", 0, 0, false},
		{IdEditUndo, "undo", "编辑/撤销", "菜单", "撤销", FL_CTRL + 'z', 0, true},
		{IdEditRedo, "redo", "编辑/重做", "菜单", "重做", FL_CTRL + 'y', 0, true},
		{IdEditCut, "cut", "编辑/剪切", "菜单", "剪切", FL_CTRL + 'x', 0, true},
		{IdEditCopy, "copy", "编辑/复制", "菜单", "复制", FL_CTRL + 'c', 0, true},
		{IdEditPaste, "paste", "编辑/粘贴", "菜单", "粘贴", FL_CTRL + 'v', 0, true},
		{IdEditDelete, "delete", "编辑/删除", "菜单", "删除", FL_Delete, 0, true},
		{IdEditSelectAll, "selectAll", "编辑/全选", "菜单", "全选", FL_CTRL + 'a', 0, true},
		{IdFind, "find", "编辑/查找…", "菜单", "查找…", FL_CTRL + 'f', 0, false},
		{IdFindNext, "findNext", "编辑/查找下一个", "菜单", "查找下一个", FL_F + 3, 0, false},
		{IdFindPrev, "findPrev", "编辑/查找上一个", "菜单", "查找上一个", FL_F + 3 | FL_SHIFT, 0, false},
		{IdReplace, "replace", "编辑/替换…", "菜单", "替换…", FL_CTRL + 'h', 0, false},
		{IdViewEditor, "viewEditor", "视图/编辑器与控制台", "菜单", "编辑器与控制台", 0, FL_MENU_TOGGLE, false},
		{IdToggleRounded, "rounded", "视图/窗口圆角", "菜单", "窗口圆角", 0, FL_MENU_TOGGLE, false},
		{IdToggleShadow, "shadow", "视图/窗口阴影", "菜单", "窗口阴影", 0, FL_MENU_TOGGLE, false},
		// m149: no menu item. The close button already hides to the tray, and
		// the tray icon brings the window back, so a 视图 row only duplicated
		// that gesture. Keep the command (and menu.hideTray) so existing
		// user-bound hotkeys and the shortcut dialog still work; an empty path
		// is the table's established way to own a key without a menu row.
		{IdHideTray, "hideTray", "", "窗口 > 托盘", "隐藏到托盘", 0, 0, false},
		// m149: three groups, two divider lines. The first rows act on ONE
		// script; 全部停止/全部运行 act on the whole set and get their own band
		// in the middle; the last three maintain the script library.
		// FL_MENU_DIVIDER draws BELOW its item (Fl_Menu_Item.H:36), so the
		// lines belong on the last row of each band, not on the first row of
		// the next one.
		{IdRun, "run", "脚本/运行", "菜单", "运行", 0, 0, false},
		{IdStop, "stop", "脚本/停止", "菜单", "停止", 0, 0, false},
		{IdRestart, "restart", "脚本/重启", "菜单", "重启", 0, 0, false},
		{IdEditSelected, "editSelected", "脚本/编辑", "菜单", "编辑", 0, 0, false},
		{IdNotepad, "notepad", "脚本/用记事本打开", "菜单", "用记事本打开", 0, 0, false},
		// m172: 编译为 EXE ends the destructive half of the per-script band.
		{IdCompileExe, "compileExe", "脚本/编译为 EXE", "菜单", "编译为 EXE", 0, 0, false},
		// m177: 删除 closes the per-script band, so the divider that used to sit
		// under 编译为 EXE moves here (FL_MENU_DIVIDER draws BELOW its item).
		{IdDeleteScript, "deleteScript", "脚本/删除…", "菜单", "删除…", 0, FL_MENU_DIVIDER, false},
		{IdStopAll, "stopAll", "脚本/全部停止", "菜单", "全部停止", 0, 0, false},
		{IdRunAll, "runAll", "脚本/全部运行", "菜单", "全部运行", 0, FL_MENU_DIVIDER, false},
		// m147: grouping is by the object acted upon, not by whether the action
		// happens to write a file. 检查更新 also writes one (the zip replaces
		// the exe) and still belongs to 帮助 by convention, so these three
		// script-library actions belong to 脚本.
		// m148: a second-level 库维护 sub-menu made the three regular actions one
		// extra click away. They are flat again, without hiding them.
		{IdRescan, "rescan", "脚本/重新扫描", "菜单", "重新扫描", 0, 0, false},
		{IdScriptDownload, "scriptDownload", "脚本/获取脚本…", "菜单", "获取脚本…", 0, 0, false},
		{IdOpenScriptsFolder, "openScriptsFolder", "脚本/打开脚本文件夹", "菜单", "打开脚本文件夹", 0, 0, false},
		// m177: 设置 holds the deck's own preferences, and 选项… comes first
		// because it is the one a new user opens; 快捷键… is the advanced one.
		{IdOptions, "options", "设置/选项…", "菜单", "选项…", 0, 0, false},
		{IdHotkeyEdit, "hotkeyEdit", "设置/快捷键…", "菜单", "快捷键…", 0, 0, false},
		// m092: the About box and the updater live in their own 帮助 group,
		// after 设置. The key menu.about is unchanged, so existing rebinds
		// keep working.
		{IdAbout, "about", "帮助/关于", "菜单", "关于", 0, 0, false},
		{IdCheckUpdate, "checkUpdate", "帮助/检查更新", "菜单", "检查更新", 0, 0, false},
		// m097: no default key - it is a one-time setup step, not a daily action.
		{IdMcpAccess, "mcpAccess", "帮助/MCP 接入", "菜单", "MCP 接入", 0, 0, false},
		// m090: what is left of the middle band owns buttons but no menu item -
		// 清空 is a console action, 运行 belongs to 脚本/运行 - so the list keeps
		// those as "Window > ..." rows in the same dialog.
		{IdClearConsole, "clearConsole", "", "窗口 > 中部条带", "清空", 0, 0, false},
		{IdRunOrStopEdit, "runOrStop", "", "窗口 > 中部条带", "运行", 0, 0, false},
		{IdReloadEditor, "reloadEditor", "", "窗口 > 编辑区", "重新载入", 0, 0, false},
		{IdCopyConsole, "copyConsole", "", "窗口 > 控制台", "复制", 0, 0, false},
	};
	return specs;
}

DeckWindow::DeckWindow(Engine &aEngine)
	: Fl_Double_Window(600, 400, "AHKDeck")
	, mEngine(aEngine)
{
	// Deck frame: no OS caption. DeckTitleBar takes over dragging and the
	// three window buttons; InstallDeckFrame() re-adds WS_THICKFRAME for the
	// resize edges after show().
	border(0);
	ApplyDeckTheme();
	color(DeckColors().background3);
	labelsize(TextSize());

	// Optional startup view for automation (and power users): "editor" (or
	// "all") opens the editor column too, "list" keeps it folded. m112: the
	// script list itself is always present, so that is the only choice left.
	{
		char spec[64] = {0};
		if (GetEnvironmentVariableA("AHKDECK_START_VIEW", spec, sizeof(spec)) > 0)
		{
			const std::string value(spec);
			if (value == "all" || value.find("editor") != std::string::npos)
			{
				mShowEditor = true;
				mViewFromEnv = true;
			}
			else if (value.find("list") != std::string::npos)
			{
				mShowEditor = false;
				mViewFromEnv = true;
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
	// m088: the menu is built from the command table, so a command that is
	// not in the table cannot be in the menu, and the table's default
	// shortcut is the shortcut the menu shows.
	for (const CommandSpec &cmd : Commands())
	{
		if (!cmd.path || !*cmd.path)
			continue;
		mMenu->add(cmd.path, cmd.shortcut, OnMenu, (void *)(intptr_t)cmd.id, cmd.flags);
	}

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
	mList->textsize(TextSize());
	mList->linespacing(5); // list pitch is font 16 + 10 = 26
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
		caption->labelsize(TextSize());
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

	// m089: the editor, the console and the pane's border overlay come FIRST,
	// so the find bar's buttons are the topmost children. FLTK hands FL_MOVE
	// to the LAST created child that contains the pointer; the overlay is a
	// full-size box, so created after the bar it became Fl::belowmouse() and
	// swallowed every FL_MOVE - the bar's buttons never saw FL_ENTER and
	// never changed colour (measured: 0 changed pixels on hover, while the
	// console buttons, created after the overlay, changed 1232).
	mEditor = new EditorBox(0, 0, 0, 0, this);
	mEditBuffer = new Fl_Text_Buffer();
	mEditor->buffer(mEditBuffer);
	mEditor->textsize(TextSize());
	// FLTK 1.4.5 ships a line-number margin; no custom gutter needed (m016).
	mEditor->linenumber_width(0);
	mEditor->linenumber_size(TextSize());
	mEditor->linenumber_fgcolor(DeckColors().text1);
	mEditor->linenumber_bgcolor(DeckColors().background1);
	mEditor->linenumber_font(FL_COURIER);
	mEditor->linenumber_align(FL_ALIGN_RIGHT);
	// The editor caret: SIMPLE_CURSOR is the same 2px, cap-less line the
	// inputs draw, and it must blink off the shared 500 ms phase.
	mEditor->cursor_style(Fl_Text_Display::SIMPLE_CURSOR);
	mEditor->cursor_color(DeckColors().text1);
	CaretBlink::attach(mEditor);
	mEditBuffer->add_modify_callback(OnBufferModified, this);

	mConsole = new DeckDisplay(0, 0, 0, 0);
	mBuffer = new Fl_Text_Buffer();
	mConsole->buffer(mBuffer);
	mConsole->textsize(TextSize());
	mConsole->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);

	mEditorBorder = new Fl_Box(0, 0, 0, 0);
	mEditorBorder->box(BorderOnlyBox());

	// Find/replace bar (m016): results go to the console, never to a dialog.
	// m084: the layout - fixed-width fields with the hint drawn inside,
	// buttons sized by their own label, and a centred match counter.
	// m085: the bar proper - a grey band, two rows, a check box for the
	// case, a check box for the wrap, and a button for both directions.
	// The band is created FIRST so it sits UNDER the controls (FLTK draws the
	// children in creation order).
	mFindBarBg = new Fl_Box(0, 0, 0, 0);
	mFindBarBg->box(FL_FLAT_BOX);
	// m086: the same background as the script list's header row, so the find
	// bar belongs to the deck instead of looking like a foreign grey band.
	mFindBarBg->color(DeckColors().background3);

	mFindInput = new BlinkInput(0, 0, 0, 0);
	mFindInput->tooltip("查找（Ctrl+F），回车即查找下一个");
	// FLTK_ENTER_KEY_CHANGED = CHANGED | ENTER_KEY | NOT_CHANGED: the callback
	// fires on every keystroke (to keep the counter live) AND on Enter (to
	// jump to the next match). The reason tells the two apart.
	mFindInput->when(FL_WHEN_ENTER_KEY_CHANGED);
	mFindInput->callback(OnFindInput, this);
	mFindInput->setHint("查找");

	mFindNext = new HoverButton(0, 0, 0, 0, "下一个");
	mFindNext->tooltip("查找下一个（F3）");
	mFindNext->callback(OnFindNext, this);

	mFindPrev = new HoverButton(0, 0, 0, 0, "上一个");
	mFindPrev->tooltip("查找上一个（Shift+F3）");
	mFindPrev->callback(OnFindPrev, this);

	mReplaceInput = new BlinkInput(0, 0, 0, 0);
	mReplaceInput->tooltip("替换为（Ctrl+H 进入，回车即全部替换）");
	// m083: Enter in the replacement field replaces everywhere. The bar has no
	// menu to fall back on (m048 took the 编辑 menu away), so the keyboard has
	// to carry the whole feature: Ctrl+F, Ctrl+H, F3, Enter.
	mReplaceInput->when(FL_WHEN_ENTER_KEY_ALWAYS);
	mReplaceInput->callback(OnReplaceAll, this);
	mReplaceInput->setHint("替换为");

	mReplaceOne = new HoverButton(0, 0, 0, 0, "替换");
	mReplaceOne->tooltip("替换当前选中处，并查找下一个");
	mReplaceOne->callback(OnReplaceOne, this);

	mReplaceAll = new HoverButton(0, 0, 0, 0, "全部替换");
	mReplaceAll->callback(OnReplaceAll, this);

	// m085: a real check box with a text label, the way the menu bar spells it.
	// m086: DeckCheckButton - drawn exactly like the menu bar's check mark.
	mMatchCase = new DeckCheckButton(0, 0, 0, 0, "区分大小写");
	mMatchCase->tooltip("区分大小写");
	mMatchCase->callback(OnMatchCase, this);

	// m085: the wrap switch. On (the default): the search starts over at
	// the top when it runs past the end. Off: it stops and says so.
	mWrap = new DeckCheckButton(0, 0, 0, 0, "循环");
	mWrap->tooltip("查找到底后回到开头继续（循环搜索）");
	mWrap->value(1);
	mWrap->callback(OnMatchCase, this);

	// m086: the whole-word switch (the "全词匹配").
	mWholeWord = new DeckCheckButton(0, 0, 0, 0, "全词匹配");
	mWholeWord->tooltip("只匹配完整的词（前后不是字母、数字或汉字）");
	mWholeWord->callback(OnMatchCase, this);

	mFindCount = new Fl_Box(0, 0, 0, 0);
	mFindCount->box(FL_NO_BOX);
	mFindCount->align(FL_ALIGN_CENTER);
	mFindCount->labelsize(SubTextSize());
	mFindCount->copy_label("0/0");

	// m087: the bar's switch. The find bar closes with a small "X" at
	// the right end of the first row; Ctrl+F brings it back. Ours does the
	// same, and the folded bar gives its 60px back to the editor.
	mFindClose = new HoverButton(0, 0, 0, kFindFieldH, "X");
	mFindClose->tooltip("收起查找栏（Ctrl+F 重新打开）");
	mFindClose->labelsize(SubTextSize());
	mFindClose->callback(OnFindClose, this);

	// m087: the bar's own widget list, so folding and unfolding are one
	// operation instead of twelve.
	mFindBarWidgets = {mFindBarBg, mFindInput, mFindNext, mFindPrev,
		mReplaceInput, mReplaceOne, mReplaceAll, mMatchCase, mWrap,
		mWholeWord, mFindCount, mFindClose};

	// m089: the find bar's widgets are now the LAST children of the window.
	// That is the whole fix for "the bar's buttons never change colour": the
	// pane's border overlay is a full-size box, and FLTK hands FL_MOVE to the
	// last created child that contains the pointer - created after the bar it
	// became Fl::belowmouse() and swallowed every FL_MOVE, so no button ever
	// saw FL_ENTER (measured: 0 changed pixels on hover, while the console
	// buttons - created after the overlay - changed 1232).

	// Tool console header: "输出" on the left, actions right-aligned.
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
	mStatus->labelsize(SubTextSize());


	mEditorWidgets = {mEditorFrame, mEditorBar, mEditorBorder,
		mConsoleSave, mConsoleClose, mConsoleClear, mConsoleRun,
		mFindBarBg, mFindInput, mFindNext, mFindPrev, mReplaceInput, mReplaceOne,
		mReplaceAll, mMatchCase, mWrap, mWholeWord, mFindCount, mFindClose,
		mEditor, mConsole};

	// m112: the two grab bands are created LAST so they sit above the panes and
	// are the first children to see the pointer in the gap. They never take
	// layout space - they occupy the 6px the panes already leave empty.
	mSplitV = new DeckSplitter(DeckSplitter::Column);
	mSplitV->onMove = [this](int aEdge)
	{
		const int usable = (ClientWidth() - 2 * 8) - kSplitterSize;
		if (usable <= 1)
			return;
		const int pct = (int)std::lround((aEdge - 8) * 100.0 / usable);
		mListPct = std::clamp(pct, kPaneMinSharePct, 100 - kPaneMinSharePct);
		Layout();
		// m114: resizing children does not damage the area they left behind, so
		// a drag used to smear copies of the old panes across the window. One
		// full damage per step keeps the double buffer honest.
		redraw();
	};
	mSplitV->onRelease = [this] { SaveSettings(); redraw(); };

	mSplitH = new DeckSplitter(DeckSplitter::Row);
	mSplitH->onMove = [this](int aEdge)
	{
		if (mBodyH <= 1)
			return;
		const int outputH = mBodyTop + mBodyH - (aEdge + kSplitterSize);
		const int pct = (int)std::lround(outputH * 100.0 / mBodyH);
		mConsolePct = std::clamp(pct, kPaneMinSharePct, 100 - kPaneMinSharePct);
		Layout();
		redraw();
	};
	mSplitH->onRelease = [this] { SaveSettings(); redraw(); };

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
		// m107 (side fix): a path ending in .ini is the legacy geometry format
		// (and the UI gates' override), never a corrupt settings.json. m094's
		// move-aside ran before this check, so an .ini override was renamed to
		// .broken on startup and the geometry it held was then lost.
		WindowGeometry geometry;
		if (!IsLegacyIniPath(mSettingsPath))
		{
			Json loaded;
			std::string configError;
			if (!mConfigMissing && ReadJsonConfig(mSettingsPath, loaded, configError))
			{
				mConfigRoot = std::move(loaded);
				mConfigWritable = true;
			}
			else if (!mConfigMissing)
			{
				// m094: a corrupt settings.json used to disable saving for the rest
				// of the session ("未覆盖原文件"), so every later change was lost.
				// Move the damaged file aside for diagnosis and start from defaults.
				const std::wstring backup = mSettingsPath + L".broken";
				MoveFileExW(mSettingsPath.c_str(), backup.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
				mConfigWritable = true;
				std::fprintf(stderr, "AHKDeck: settings.json unreadable (%s); moved to .broken\n",
					configError.c_str());
			}
		}
		else
		{
			// m110: a legacy .ini path that the deck has already migrated to JSON
			// on save must still load. Try the JSON form first; fall back to the
			// ini parser only when the file is absent or not JSON.
			Json loaded;
			std::string configError;
			if (ReadJsonConfig(mSettingsPath, loaded, configError))
			{
				mConfigRoot = std::move(loaded);
				mConfigWritable = true;
			}
			else
			{
				std::string settingsError;
				LoadUiSettings(mSettingsPath, geometry, settingsError);
			}
		}

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
			// m053: the view toggle is part of the config too. The automation
			// env var wins over the saved view. m112: the script list is always
			// shown, so only the editor column can be folded away.
			if (!mViewFromEnv && ui.Has("editor"))
				mShowEditor = ui["editor"].AsBool(mShowEditor);
			// m112: the two pane shares. Clamped on load as well, so a hand-edited
			// file cannot squeeze a pane out of sight.
			mListPct = std::clamp(ui["list_pct"].AsInt(mListPct), kPaneMinSharePct,
				100 - kPaneMinSharePct);
			mConsolePct = std::clamp(ui["console_pct"].AsInt(mConsolePct), kPaneMinSharePct,
				100 - kPaneMinSharePct);
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

	// m109/m177: temp\ is per-machine scratch space, cleaned once per launch with
	// the window 设置/选项… writes. The key still has to exist before the window
	// is ever opened, so a first run writes the default back - that is also how a
	// hand-edited settings.json discovers the name.
	mHousekeepingDays = RetentionDaysFromJson(mConfigRoot);
	{
		const Json &existing = mConfigRoot["housekeeping"];
		if (!existing.IsObject() || !existing.Has("max_age_days"))
		{
			Json table = existing.IsObject() ? existing : Json::MakeObject();
			table.Set("max_age_days", Json((double)mHousekeepingDays));
			mConfigRoot.Set("housekeeping", table);
			mConfigDirty = true;
		}
	}

	std::string error;
	if (!mEngine.Load(error))
	{
		mReady = false;
		Append("[engine] " + error);
	}
	else
	{
		mReady = true;
		// m192 P3: the kernel is a separate EXE now, one child process per
		// session. The old "in-process, no second process" line was a lie.
		Append("[engine] 内核就绪（进程外，每个会话一个子进程）");
	}


	// m064: the MCP tool thread reads the editing area through this snapshot, so
	// run_editor can never see text older than what the user is looking at.
	mMcpTools.SetEditorSupplier([this] { return EditorSnapshot(); });
	mMcpTools.SetEditorPathSupplier([this] { return EditorPathSnapshot(); });
	RefreshEditorSnapshot();


	// m177: temp\ is the one root every run-time artefact lives under;
	// RunHousekeeping (below) creates it and keeps its four subdirectories.
	ScanScripts();
	LoadHotkeyFile();
	RunHousekeeping();
	// m088: the user's re-bound keys go into the menu items, so the menu
	// shows the key that is actually in force.
	ApplyMenuShortcuts();
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
			// m131: the tray menu's 运行中 submenu asks who is running at the
			// moment of each right-click. Same thread as this window and the
			// engine, so IsRunning's GetSession is safe to call in there.
			// m132: it also asks for each script's live tray menu, which the tray
			// menu mirrors in as a real submenu, and for the window that a picked
			// command id has to be posted back to.
			mHooks.SetRunningMenuProvider([this]() -> std::vector<NativeHooks::RunningScript> {
				std::vector<NativeHooks::RunningScript> running;
				for (const ScriptRow &row : mScripts)
				{
					if (!IsRunning(row))
						continue;
					NativeHooks::RunningScript entry;
					entry.path = row.path;
					entry.name = DisplayNameOf(row.name);
					entry.menu = mEngine.TrayMenu(row.session);
					entry.window = mEngine.FindScriptWindow(row.session);
					running.push_back(std::move(entry));
				}
				return running;
			});
			std::string trayError;
			if (!mHooks.AddTrayIcon("AHKDeck", trayError))
				Append("[deck] " + trayError + "：关闭按钮按原样退出，不隐藏");
			RebindHotkeys();
			UpdateTrayTip();
		}
	}

	Fl::add_timeout(0.1, OnTick, this);

	// m082 gate knob (test-only): AHKDECK_AUTOSAVE_MS shrinks the timed save so
	// the MCP gate can watch several periods in seconds. Manual parse - no
	// <cstdlib> dependency here.
	{
		char autosaveMs[16] = {0};
		if (GetEnvironmentVariableA("AHKDECK_AUTOSAVE_MS", autosaveMs, sizeof(autosaveMs)) > 0)
		{
			int parsed = 0;
			for (const char *p = autosaveMs; *p >= '0' && *p <= '9'; ++p)
				parsed = parsed * 10 + (*p - '0');
			if (parsed >= 100 && parsed <= 30000)
				mAutoSaveMs = parsed;
		}
	}
}

DeckWindow::~DeckWindow()
{
	// m094: any destruction path other than Run()'s orderly shutdown used to
	// destroy a joinable std::thread, which is std::terminate. StopMcp is
	// idempotent, so calling it here is both safe and the last line of defence.
	// m172: the same argument for the (detached) compile worker - clear the
	// alive flag FIRST, so any Fl::awake reply still in the queue drops itself
	// instead of hopping into a half-dead window.
	*mCompileAlive = false;
	StopMcp();
	delete mShortcutDialog;
	mShortcutDialog = nullptr;
	delete mNameDialog;
	mNameDialog = nullptr;
	Fl::remove_timeout(OnTick, this);
	Fl::remove_timeout(TrimWorkingSetCb, this);
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

	// m083: the find/replace keys and the save key belong to the WINDOW, not to
	// the editor. The editor answers them first when it holds the focus (it
	// returns 1, so the window never sees them), but when the focus sits in the
	// list, in the console or in the find bar, only the window can catch them.
	// Without this, Ctrl+F after a replace-all was dead: the focus was in the
	// replacement field and the key went nowhere.
	// m088: every bound key comes from the command table now, so the keys
	// the user re-bound work here too. The editor answers them first when it
	// holds the focus.
	if ((aEvent == FL_KEYDOWN || aEvent == FL_SHORTCUT) && DispatchShortcut())
		return 1;

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
			// m088: the console copy is a command now ("窗口 > 控制台 > 复制"),
			// so this block is only the fallback for a key nothing claims.
			OnCopy(nullptr, this);
			return 1;
		}
		if (key == FL_F + 11)
		{
			OpenTempFolder();
			return 1;
		}
		if (ctrl && shift && (key == 'c' || key == 'C'))
		{
			OnCopy(nullptr, this);
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

	// m115: the list and editor frames each paint their own 1px top border, so
	// the 6px splitter gap between them has no top run - a notch at the top
	// right of the list pane that moves with the split. Bridge the gap with one
	// continuous line the same colour as the pane borders.
	if (mShowEditor)
	{
		const int top = DeckTitleBar::kHeight + 24 + 2;
		fl_color(ThemeBorderLine());
		fl_rectf(8, top + 2, ClientWidth() - 16, 1);
	}
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

	// m112: the list is always there. Without the editor column it takes the
	// whole width; with it, the 6px gap becomes the vertical grab band and the
	// split is a drag share clamped to a fifth of the window each.
	int listW = cw - 2 * pad;
	const int fullUsable = (cw - 2 * pad) - kSplitterSize;
	if (mShowEditor)
	{
		int minW = (cw * kPaneMinSharePct) / 100;
		if (minW < 180)
			minW = 180;
		listW = (int)std::lround(fullUsable * (mListPct / 100.0));
		if (listW < minW)
			listW = minW;
		if (listW > fullUsable - minW)
			listW = fullUsable - minW;
	}
	const int editorX = pad + listW + kSplitterSize;
	const int editorW = mShowEditor ? fullUsable - listW : 0;

	LayoutListPane(pad, top, listW, paneH);
	if (mSplitV)
	{
		if (mShowEditor)
		{
			mSplitV->bounds(pad + 180, pad + fullUsable - 180);
			mSplitV->resize(pad + listW, top, kSplitterSize, paneH);
			mSplitV->show();
		}
		else
		{
			mSplitV->hide();
		}
	}
	if (mShowEditor)
		LayoutEditorPane(editorX, top, editorW, paneH);
	else if (mSplitH)
		mSplitH->hide();

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
	// m048: no top toolbar. Code is on top, the middle band holds the four
	// actions, and the log is a quarter of the column at the bottom - the same
	// shape as the AI pane.
	const int bodyTop = cy;
	const int bodyH = ch;
	mBodyTop = bodyTop;
	mBodyH = bodyH;
	// m112: the console share comes from the row splitter. Both sides keep at
	// least a fifth of the column, so a drag can never hide one of them.
	int outputH = bodyH * mConsolePct / 100;
	int minH = (bodyH * kPaneMinSharePct) / 100;
	if (minH < kConsoleOutputMinH)
		minH = kConsoleOutputMinH;
	const int maxOutput = bodyH - kPaneBarH - kSplitterSize - minH;
	if (outputH < minH)
		outputH = minH;
	if (outputH > maxOutput)
		outputH = maxOutput;
	if (outputH < 0)
		outputH = 0;
	const int outputY = bodyTop + bodyH - outputH;
	// The 6px between the action band and the console is the row grab band; the
	// action band stays with the editor above it.
	const int barY = outputY - kSplitterSize - kPaneBarH;
	// m084: the find/replace bar sits ABOVE the editor, the way the deck puts it:
	// the eye goes to the field first and the text second. It used to sit
	// between the editor and the action bar, which pushed the code away from
	// the field that searches it.
	const int findRowH = kFindRowH;
	// m087: a folded bar takes no room at all - the editor takes its two
	// lines back.
	const int findBarH = mFindBarVisible ? findRowH * 2 : 0;
	const int editorTop = bodyTop + findBarH;
	const int editorH = barY - editorTop;
	mEditor->resize(cx, editorTop, cw, editorH > 0 ? editorH : 0);
	LayoutFindBar(cx, bodyTop, cw, findRowH);
	mEditorBar->resize(cx, barY, cw, kPaneBarH);
	mConsole->resize(cx, outputY, cw, outputH);
	// m112: the row grab band sits in the 6px between the action band and the
	// console. Its bounds stop the drag where either side would fall under a
	// fifth of the column.
	if (mSplitH)
	{
		const int splitY = outputY - kSplitterSize;
		mSplitH->bounds(bodyTop + kPaneBarH + minH, bodyTop + bodyH - minH - kSplitterSize);
		mSplitH->resize(cx, splitY, cw, kSplitterSize);
		mSplitH->show();
	}

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

// m085: the find/replace bar.
//
// Row 1: the needle (a FIXED 120px field with the hint drawn inside), the
//         next button, the previous button, the "区分大小写" switch, the
//         "全词匹配" switch, the "循环" switch, and - filling the rest of
//         the row - the centred match counter.
// Row 2: the replacement (same fixed field), replace, replace all.
//
// m086: the bar sits on the script list's header background, its rows are
// the deck's row height, its controls are the deck's button height, and the
// switches are drawn exactly like the menu bar's check mark.
//
// Nothing stretches: the inputs keep the fixed search-field width, the buttons
// are exactly as wide as their own label (the deck's one width rule), and the
// counter takes whatever room is left. When the column is too narrow the
// optional controls drop out one by one instead of being squeezed: the case
// switch shortens its label to "Aa" first, then the wrap switch goes, then
// "全词匹配" goes, then "上一个" goes. (The other order drops them in reverse;
// the deck keeps the two switches the user actually asked for and gives up
// the convenience button first.)
void DeckWindow::LayoutFindBar(int x, int y, int w, int rowH)
{
	const int gap = kFindGap;
	const int fieldH = kFindFieldH;
	const int row2Y = y + rowH;
	const int fieldY = y + (rowH - fieldH) / 2;
	const int fieldY2 = row2Y + (rowH - fieldH) / 2;
	const int right = x + w;

	// m087: a folded bar is not laid out at all. ApplyView() shows every
	// editor widget whenever the editor pane is shown, so the fold has to be
	// applied here - after that - not at the moment the user clicks "X".
	if (!mFindBarVisible)
	{
		for (Fl_Widget *widget : mFindBarWidgets)
			if (widget)
				widget->hide();
		return;
	}
	// An unfolded bar shows all of its widgets again (the narrow-column rule
	// below may hide some of them once more).
	for (Fl_Widget *widget : mFindBarWidgets)
		if (widget)
			widget->show();

	// The band under the two rows. It is created before the controls, so
	// FLTK draws it underneath them.
	mFindBarBg->resize(x, y, w, rowH * 2);

	// m087: the bar's switch sits at the right end of the first row (the deck
	// puts its close button there too). Everything else in the row stops
	// before it, so the counter never runs under it.
	// m089: the rule is a GAP on BOTH sides of the switch
	// (the shared layout rule: the button lands at x + w - closeW - GAP and
	// everything else stops at button->x() - GAP). Ours had no gap on the
	// right, so the switch sat flush against the pane's edge.
	const int closeW = kFindCloseW;
	mFindClose->resize(right - closeW - gap, fieldY, closeW, fieldH);
	const int contentRight = mFindClose->x() - gap;

	// --- Row 1: needle -> next -> prev -> case -> whole word -> wrap -> counter ---
	int cx = x + gap;
	mFindInput->resize(cx, fieldY, kFindInputW, fieldH);
	cx += kFindInputW + gap;

	const int nextW = FitButtonWidth(mFindNext);
	const int prevW = FitButtonWidth(mFindPrev);
	const int caseFullW = FitCheckWidth(mMatchCase, "区分大小写");
	const int caseShortW = FitCheckWidth(mMatchCase, "Aa");
	const int wordW = FitCheckWidth(mWholeWord, "全词匹配");
	const int wrapW = FitCheckWidth(mWrap, "循环");

	// The counter keeps kFindCountMinW; a control only stays if the row still
	// fits beside it. When the column is too narrow the controls drop out one
	// by one - first "上一个" (a convenience), then "全词匹配" (the deck has it,
	// but it is the one nobody asked for), then the wrap switch, then the
	// case box (in its short "Aa" form), and last "下一个".
	// m087: the row ends at the switch, not at the pane's edge.
	auto rowFits = [&](int aExtra) { return cx + aExtra + kFindCountMinW <= contentRight; };
	int extra = nextW + gap + prevW + gap + caseFullW + gap + wordW + gap +
		wrapW + gap;
	bool showNext = true;
	bool showPrev = true;
	bool showCase = true;
	bool showWord = true;
	bool showWrap = true;
	bool shortCase = false;
	if (!rowFits(extra))
	{
		showPrev = false;
		extra -= prevW + gap;
	}
	if (!rowFits(extra))
	{
		showWord = false;
		extra -= wordW + gap;
	}
	if (!rowFits(extra))
	{
		showWrap = false;
		extra -= wrapW + gap;
	}
	if (!rowFits(extra))
	{
		shortCase = true;
		extra -= caseFullW - caseShortW;
	}
	if (!rowFits(extra))
	{
		showCase = false;
		extra -= caseShortW + gap;
	}
	if (!rowFits(extra))
	{
		showNext = false;
	}

	// "下一个" and "上一个" stand next to each other: they are one gesture
	// (walk through the matches), and a pair reads as a pair.
	if (showNext)
	{
		mFindNext->show();
		mFindNext->resize(cx, fieldY, nextW, fieldH);
		cx += nextW + gap;
	}
	else
		mFindNext->hide();

	if (showPrev)
	{
		mFindPrev->show();
		mFindPrev->resize(cx, fieldY, prevW, fieldH);
		cx += prevW + gap;
	}
	else
		mFindPrev->hide();

	// The case box spells itself out when there is room and shortens to the
	// compact "Aa" when there is not - a check box either way, never the
	// odd light button with a corner mark (m084's "Aa").
	if (showCase)
	{
		const int caseW = shortCase ? caseShortW : caseFullW;
		mMatchCase->copy_label(shortCase ? "Aa" : "区分大小写");
		mMatchCase->show();
		mMatchCase->resize(cx, fieldY, caseW, fieldH);
		cx += caseW + gap;
	}
	else
		mMatchCase->hide();

	if (showWord)
	{
		mWholeWord->show();
		mWholeWord->resize(cx, fieldY, wordW, fieldH);
		cx += wordW + gap;
	}
	else
		mWholeWord->hide();

	if (showWrap)
	{
		mWrap->show();
		mWrap->resize(cx, fieldY, wrapW, fieldH);
		cx += wrapW + gap;
	}
	else
		mWrap->hide();

	// The counter fills the rest of the row and centres its text, like the
	// match counters elsewhere. It stops before the switch.
	int countW = contentRight - cx;
	if (countW < 0)
		countW = 0;
	mFindCount->resize(cx, fieldY, countW, fieldH);

	// Row 2: replacement -> replace -> replace all.
	cx = x + gap;
	mReplaceInput->resize(cx, fieldY2, kFindInputW, fieldH);
	cx += kFindInputW + gap;
	const int oneW = FitButtonWidth(mReplaceOne);
	const int allW = FitButtonWidth(mReplaceAll);
	if (cx + oneW + gap + allW <= right)
	{
		mReplaceOne->show();
		mReplaceAll->show();
		mReplaceOne->resize(cx, fieldY2, oneW, fieldH);
		cx += oneW + gap;
		mReplaceAll->resize(cx, fieldY2, allW, fieldH);
	}
	else if (cx + oneW <= right)
	{
		mReplaceOne->show();
		mReplaceAll->hide();
		mReplaceOne->resize(cx, fieldY2, oneW, fieldH);
	}
	else
	{
		mReplaceOne->hide();
		mReplaceAll->hide();
	}
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

	std::string line = "[" + ShortTime(aRecord.ts) + "] ";
	if (!sameOwner && !name.empty())
		line += name + " ";

	// m061: a script's own stdout is not a fault. It prints as plain text with
	// the same time/owner prefix, never with the "出错" label or a line number.
	if (aRecord.kind == "stdout")
	{
		line += aRecord.message;
	}
	else
	{
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

		line += (aRecord.kind == "warn") ? "警告" : "出错";
		if (!where.empty())
			line += "（" + where + "）";
		line += "：" + aRecord.message;
		if (!aRecord.extra.empty())
			line += " Specifically: " + aRecord.extra;
	}

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

	// m125: the release ships an empty Scripts folder, but some extractors drop
	// empty directories and a user may delete it - then the list stayed empty
	// forever and 文件 > 打开脚本文件夹 did nothing. Create it on demand.
	const std::wstring scriptsDir = ExeDirectory() + L"\\Scripts";
	CreateDirectoryW(scriptsDir.c_str(), nullptr);

	WIN32_FIND_DATAW fd;
	const std::wstring pattern = scriptsDir + L"\\*.ahk";
	HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		AddScript(scriptsDir + L"\\" + fd.cFileName);
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
	ui.Set("editor", Json(mShowEditor));
	ui.Set("list_pct", Json((double)mListPct));
	ui.Set("console_pct", Json((double)mConsolePct));
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

	// m192 P3: one official AutoHotkey kernel EXE, spawned once per session -
	// no fork, no in-process DLL. mReady means Engine::Load found a usable
	// kernel path, so the wording is 就绪/不可用 rather than 载入/未载入.
	// The number itself still comes from the PE version resource
	// (KernelVersionOf), never from a constant here.
	std::string engine = "AutoHotkey";
	if (mKernelSummary.empty())
		engine += mReady ? " 就绪" : " 不可用";
	else
		engine += " " + mKernelSummary + (mReady ? std::string() : std::string(" (不可用)"));
	std::string text = "引擎: " + engine;
	text += "   |   脚本: " + std::to_string((int)mScripts.size());
	if (mVisible.size() != mScripts.size())
		text += " (显示 " + std::to_string((int)mVisible.size()) + ")";
	text += "   |   运行中: " + std::to_string((int)running);
	// m107: the host's own CPU/memory/GPU cost. It is appended last on purpose:
	// at a narrow width the tail is what FLTK clips, and the engine/script facts
	// are the ones that must survive.
	if (!mResourceText.empty())
		text += "   |   " + mResourceText;
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
		label += DisplayNameOf(row.name);
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
	ApplyMenuShortcuts();
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

// m173: the compiler child and the save dialog run outside the kernel, so the
// deck thread never holds COM today; a one-shot init/uninit pair around the
// dialog is the whole obligation (recipe: .ex/pecia cbOpen). Fail-closed: if
// the apartment refuses, the picker returns empty and the compile is reported
// as not-started - the user keeps a clean console line, not a half action.
static bool ShowPickSaveExePath(const std::wstring &aFolder, const std::wstring &aFileName,
	std::wstring &aOutPath)
{
	aOutPath.clear();
	const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool ownUninit = SUCCEEDED(hrInit);
	bool ok = false;
	IFileSaveDialog *dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_IFileSaveDialog, reinterpret_cast<void **>(&dlg))) && dlg)
	{
		HRESULT hr = dlg->SetTitle(L"编译为 EXE：选择保存位置");
		static const COMDLG_FILTERSPEC kOne[] = { { L"可执行文件", L"*.exe" } };
		if (SUCCEEDED(hr))
			hr = dlg->SetFileTypes(1, kOne);
		if (SUCCEEDED(hr))
			hr = dlg->SetDefaultExtension(L"exe"); // typed name without extension -> gets .exe
		if (SUCCEEDED(hr) && !aFolder.empty())
		{
			IShellItem *folder = nullptr;
			if (SUCCEEDED(SHCreateItemFromParsingName(aFolder.c_str(), nullptr,
				IID_IShellItem, reinterpret_cast<void **>(&folder))) && folder)
			{
				hr = dlg->SetFolder(folder); // best-effort: a dead folder must not block the dialog
				folder->Release();
			}
		}
		if (SUCCEEDED(hr) && !aFileName.empty())
			dlg->SetFileName(aFileName.c_str()); // default into the edit field
		if (SUCCEEDED(hr))
		{
			hr = dlg->Show(nullptr);
			if (SUCCEEDED(hr))
			{
				IShellItem *item = nullptr;
				if (SUCCEEDED(dlg->GetResult(&item)) && item)
				{
					PWSTR path = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
					{
						aOutPath = path;
						ok = !aOutPath.empty();
						CoTaskMemFree(path);
					}
					item->Release();
				}
			}
			// FAILED(hr) from Show() is the user cancel - not an error.
		}
		dlg->Release();
	}
	if (ownUninit)
		CoUninitialize();
	return ok;
}

// m110 helper: "c:\dir\x" -> "c:\dir" (last segment cut at the final slash).
static std::wstring ParentDirOf(const std::wstring &aPath)
{
	const size_t slash = aPath.find_last_of(L'\\');
	if (slash == std::wstring::npos)
		return std::wstring();
	return aPath.substr(0, slash);
}

// m173: compile one row to a standalone exe. The console is the only report
// surface (no dialog to invent): the picker decides the path, the worker
// decides the verdict. The child can take a second or two, so it runs on a
// worker exactly like the downloader, and the reply hops back with Fl::awake
// guarded by mCompileAlive.
void DeckWindow::CompileSelected(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	if (mCompileBusy)
	{
		Append("[编译] 已有一个编译在进行，等它结束再试");
		return;
	}
	const ScriptRow row = mScripts[(size_t)aIndex]; // m173: copy - the picker re-enters the list path
	std::wstring probeCompiler;
	std::wstring probeBase;
	std::string toolError;
	if (!ResolveCompileTools(ExeDirectory(), probeCompiler, probeBase, toolError))
	{
		Append("[编译] " + toolError);
		return;
	}
	// m173: 「另存为」first (pecila's IFileSaveDialog), with the script's own
	// folder and pre-filled name; cancel stops here and says so. The dialog's
	// working set is handed back when it closes (ScheduleSaveTrim -
	// on every path, the same rule as .ex pecia MainWindow_timers).
	// The pre-fill is "<script base>.exe" - the SAME swap rule as core's
	// DefaultExePathFor (it replaces a trailing .ahk; row.name keeps .ahk, so
	// gluing ".exe" onto it would offer 名字.ahk.exe). The test nails the
	// rule on the core side; the leaf is taken here, one truth, one helper.
	const std::wstring suggested = DefaultExePathFor(row.path);
	std::wstring savePath;
	if (!ShowPickSaveExePath(ParentDirOf(row.path), LeafOf(suggested), savePath))
	{
		ScheduleSaveTrim();
		Append("[编译] 已取消（未选择输出位置）");
		return;
	}
	ScheduleSaveTrim();
	// The filter guarantees .exe; belt: the core contract writes exactly here.
	mCompileBusy = true;
	Append("[编译] 正在编译 " + row.name + " 为 exe…");
	CompileExeOnWorker(row.path, savePath, row.name);
}

void DeckWindow::CompileExeOnWorker(std::wstring scriptPath, std::wstring outPath, std::string name)
{
	struct Reply
	{
		DeckWindow *self;
		std::shared_ptr<bool> alive;
		ExeCompileOutcome outcome;
		std::string name;
	};
	const std::wstring exeDir = ExeDirectory();
	std::shared_ptr<bool> alive = mCompileAlive;
	std::thread([exeDir, scriptPath, outPath, name, alive, this]() {
		ExeCompileOutcome outcome;
		try
		{
			outcome = CompileScriptToExe(exeDir, scriptPath, outPath);
		}
		catch (...)
		{
			outcome.error = "编译线程异常";
		}
		auto *reply = new Reply{ this, alive, std::move(outcome), name };
		Fl::awake([](void *aPayload) {
			auto *r = static_cast<Reply *>(aPayload);
			if (*r->alive)
				r->self->ApplyCompileResult(r->outcome, r->name);
			delete r;
		}, reply);
	}).detach();
}

void DeckWindow::ApplyCompileResult(const ExeCompileOutcome &aOutcome, const std::string &aName)
{
	mCompileBusy = false;
	// m173: the task is over (worker done, child process reaped). The picker's
	// earlier trim was skipped while mCompileBusy held it, so arm once more
	// here - this is the "结束任务后释放内存" point the user named. The dialog
	// pages and the compiler child's shared loads all ride back to the OS.
	ScheduleSaveTrim();
	if (!aOutcome.error.empty())
	{
		Append("[编译] " + aName + " 失败：" + aOutcome.error);
		return;
	}
	if (aOutcome.rc == 0 && aOutcome.produced)
	{
		std::string line = "[编译] " + aName + " 完成：" + Utf8(aOutcome.outPath);
		if (aOutcome.backedUp)
			line += "（旧产物已备份为 .old）";
		Append(line);
		return;
	}
	std::string detail = aOutcome.output;
	// The compiler writes several lines; keep the console readable but never
	// hide the verdict - show up to the last 3 non-empty lines verbatim.
	Append("[编译] " + aName + " 未成功（rc=" + std::to_string(aOutcome.rc) + "）");
	if (!detail.empty())
	{
		std::vector<std::string> lines;
		size_t pos = 0;
		while (pos < detail.size())
		{
			size_t nl = detail.find('\n', pos);
			if (nl == std::string::npos)
				nl = detail.size();
			std::string piece = detail.substr(pos, nl - pos);
			while (!piece.empty() && piece.back() == '\r')
				piece.pop_back();
			if (!piece.empty())
				lines.push_back(piece);
			pos = nl + 1;
		}
		const size_t start = lines.size() > 3 ? lines.size() - 3 : 0;
		for (size_t i = start; i < lines.size(); ++i)
			Append("    " + lines[i]);
	}
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

void DeckWindow::OpenTempFolder()
{
	// m177: the temp root, not logs\. One folder holds the logs, the backups,
	// the compile/validate scratch and the deleted scripts - showing the user
	// logs\ only would hide the other three.
	const std::wstring root = AppTempRoot();
	if (root.empty())
	{
		Append("[ui] 无法确定临时目录（程序路径解析失败）");
		return;
	}
	HINSTANCE result = ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if ((INT_PTR)result <= 32)
		Append("[ui] 无法打开临时目录");
	else
		Append("[ui] 打开临时目录: " + Utf8(root));
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
	// A real content change: the user has now touched the buffer, so text a
	// tool left "awaiting review" (m082) is theirs again - the timed save may
	// own it after the next period.
	if (!self->mEditLoading)
		self->mToolEditPending = false;
	// FLTK is halfway through the change while this callback runs: the buffer
	// must not be touched from here. A query - count_lines() for the gutter
	// width goes straight to position_to_line() - trips the library's
	// consistency check and aborts the process ("FI_Tex_Displayposition_to_
	// line: Consistency check pt failed"). So the gutter is remeasured on the
	// next tick, where the buffer is whole again (m083).
	self->mLineNumberDirty = true;
	// m084: the match counter follows the text, so it has to be counted again.
	self->mFindMatchesDirty = true;
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
	mToolEditPending = false;
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
	// m083: the editor takes the keyboard focus when a script is loaded.
	// Without it the caret sits in the file but the keys still go to the old
	// focus (the filter box), so the user opens a script, types, and nothing
	// happens. The deck puts the caret in the file it just opened.
	mEditor->take_focus();
	SyncEditorActions();
	SyncConsoleSelection();
	if (mList)
		mList->redraw();
	mLastAutoSave = GetTickCount64();

	Append("[editor] 已载入 " + Utf8(LeafOf(aPath)));
	return true;
}

// m082 gate hook (called from Run with AHKDECK_OPEN_SCRIPT): wraps
// LoadIntoEditor for the test environment; missing files are ignored so a
// stale environment variable cannot put an error line into a real session.
void DeckWindow::OpenScriptForGate(const std::string &aLeaf)
{
	const std::wstring path = ExeDirectory() + L"\\Scripts\\" + Wide(aLeaf);
	if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
		LoadIntoEditor(path);
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
	mToolEditPending = false;
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
	mToolEditPending = false;
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

// m177: delete = move into temp\deleted, never a hard delete. The user asked
// for a row action and a menu row that both act on ONE script, and for the
// folder next to the exe to stay clean. A move gives both: the list loses the
// row, the file survives (the temp retention pass eventually retires it), and
// there is no dialog to click through because nothing is destroyed.
void DeckWindow::DeleteSelected()
{
	const int index = SelectedIndex();
	if (index < 0)
	{
		Append("[ui] 先在左侧选一个脚本");
		return;
	}
	const ScriptRow row = mScripts[(size_t)index];

	// The editor holds the same file. Close it first: CloseScript() writes
	// unsaved edits, so the copy that lands in temp\deleted is the text the user
	// last saw. If the write fails the script stays in the list untouched.
	if (!mEditPath.empty() && SamePathI(mEditPath, row.path))
	{
		CloseScript();
		if (!mEditPath.empty() && SamePathI(mEditPath, row.path))
			return;
	}

	// A running script must not be moved out from under its own thread.
	if (IsRunning(row))
	{
		if (!mEngine.StopSession(row.session))
			Append("[ui] " + row.name + " 已经结束或无法停止");
		InvalidateSessions();
	}

	const std::wstring dir = AppTempDir(kTempDeleted);
	if (dir.empty())
	{
		Append("[ui] 无法确定临时目录（程序路径解析失败）");
		return;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);
	char stamp[32];
	std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	// row.name carries the ".ahk" leaf (m132); the stamp goes before the dot so
	// the copy still opens by double-click.
	const size_t dot = row.name.find_last_of('.');
	const std::string stem = dot == std::string::npos ? row.name : row.name.substr(0, dot);
	const std::string ext = dot == std::string::npos ? std::string(".ahk") : row.name.substr(dot);

	std::wstring target;
	std::string leafName;
	for (int attempt = 1; attempt <= 99 && target.empty(); ++attempt)
	{
		std::string candidate = stem + "-" + stamp;
		if (attempt > 1)
			candidate += "-" + std::to_string(attempt);
		candidate += ext;
		const std::wstring path = dir + L"\\" + Wide(candidate);
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			target = path;
			leafName = candidate;
		}
	}
	if (target.empty())
	{
		Append("[ui] 临时目录里同名文件太多，删除已取消");
		return;
	}

	if (!MoveFileW(row.path.c_str(), target.c_str()))
	{
		Append("[ui] 删除失败（" + std::to_string((unsigned long)GetLastError()) +
			"）：脚本可能正在退出，稍后再试");
		return;
	}

	// The binding belonged to the file: dropping it here is what keeps 快捷键
	// from listing a script that no longer exists.
	const std::string binding = HotkeyOf(mHotkeys, row.name);
	if (!binding.empty())
	{
		SetHotkeyEntry(mHotkeys, row.name, std::string());
		std::string error;
		if (!SaveHotkeys(mHotkeyPath, mHotkeys, error))
			Append("[hotkey] 删除后保存失败：" + error);
	}
	if (!mConsolePath.empty() && SamePathI(mConsolePath, row.path))
		mConsolePath.clear();

	ScanScripts();
	RefreshList();
	RebindHotkeys();
	Append("[ui] 已删除 " + row.name + "（移到 " +
		AppTempRelative(kTempDeleted, leafName) + "）");
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
	mToolEditPending = false;
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

// m087: fold / unfold the find bar. Folding gives the editor the bar's two
// lines back; every way into the bar (Ctrl+F, Ctrl+H, F3, the 编辑 menu)
// unfolds it again.
void DeckWindow::SetFindBarVisible(bool aVisible)
{
	if (mFindBarVisible == aVisible)
		return;
	mFindBarVisible = aVisible;
	Layout();
	redraw();
	// The bar is gone, so the keyboard belongs to the editor again.
	if (!mFindBarVisible && mEditor)
		mEditor->take_focus();
	// m089: the menu items have to say whether the bar is up.
	SyncFindMenuState();
}

// m089: the find bar is a toggle. Pressing the menu item (or its key) again
// takes the bar away. The find/replace callbacks do the same: the bar is
// hidden when it is already visible, shown otherwise.
void DeckWindow::ToggleFindBar(bool aFocusReplace)
{
	EnsureEditorVisible();
	if (mFindBarVisible)
	{
		SetFindBarVisible(false);
		return;
	}
	if (aFocusReplace)
		FocusReplace();
	else
		FocusFind();
}

// m089: the two menu items carry a check mark while the bar is on screen, so
// the toggle state is visible in the menu and not only in the layout.
void DeckWindow::SyncFindMenuState()
{
	if (!mMenu)
		return;
	for (const CommandSpec &cmd : Commands())
	{
		if (cmd.id != IdFind && cmd.id != IdReplace)
			continue;
		if (!cmd.path || !*cmd.path)
			continue;
		Fl_Menu_Item *item = const_cast<Fl_Menu_Item *>(mMenu->find_item(cmd.path));
		if (!item)
			continue;
		if (mFindBarVisible)
			item->flags |= FL_MENU_VALUE;
		else
			item->flags &= ~FL_MENU_VALUE;
	}
	mMenu->redraw();
}

void DeckWindow::FocusFind()
{
	EnsureEditorVisible();
	SetFindBarVisible(true);
	mFindInput->take_focus();
	mFindInput->insert_position(0, (int)std::strlen(mFindInput->value()));
}

void DeckWindow::FocusReplace()
{
	EnsureEditorVisible();
	SetFindBarVisible(true);
	mReplaceInput->take_focus();
	mReplaceInput->insert_position(0, (int)std::strlen(mReplaceInput->value()));
}

// m086: the whole-word rule: the match must not touch a word character on
// either side. A word character is a letter, a digit, an underscore, or any
// multi-byte character (Chinese counts as a word, like in every editor).
bool DeckWindow::IsWholeWordMatch(int aPos, int aLength) const
{
	if (!mEditBuffer)
		return true;
	auto isWordByte = [](unsigned char c) {
		if (c & 0x80)
			return true;
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') || c == '_';
	};
	if (aPos > 0)
	{
		const unsigned char before = (unsigned char)mEditBuffer->byte_at(aPos - 1);
		if (isWordByte(before))
			return false;
	}
	const int after = aPos + aLength;
	if (after < mEditBuffer->length())
	{
		const unsigned char next = (unsigned char)mEditBuffer->byte_at(after);
		if (isWordByte(next))
			return false;
	}
	return true;
}

// m086: one search that honours both switches. FLTK's search_forward finds
// the next occurrence of the text; when the whole-word switch is on, an
// occurrence that is part of a bigger word is skipped and the search goes on.
int DeckWindow::SearchForward(int aStart, const char *aNeedle, int aMatchCase,
	bool aWholeWord, int *aFound) const
{
	if (!mEditBuffer || !aNeedle || !*aNeedle)
		return 0;
	const int needleLength = (int)std::strlen(aNeedle);
	int start = aStart;
	int found = 0;
	while (mEditBuffer->search_forward(start, aNeedle, &found, aMatchCase))
	{
		if (!aWholeWord || IsWholeWordMatch(found, needleLength))
		{
			if (aFound)
				*aFound = found;
			return 1;
		}
		// Step over the first character of the rejected match, so an
		// overlapping occurrence is still found.
		start = mEditBuffer->next_char(found);
		if (start <= found)
			break;
	}
	return 0;
}

// m084: count the matches of the needle in the editing area. The counter in
// the find bar shows "current/total" (the same format); the positions are kept
// so that the index of the selected match is a lookup, not another scan.
void DeckWindow::UpdateFindMatches()
{
	mMatchPositions.clear();
	mMatchTotal = 0;
	mMatchIndex = 0;
	mFindMatchesDirty = false;
	const char *needle = mFindInput ? mFindInput->value() : nullptr;
	if (needle && *needle && mEditBuffer)
	{
		const int matchCase = (mMatchCase && mMatchCase->value()) ? 1 : 0;
		const bool wholeWord = mWholeWord && mWholeWord->value();
		const int needleLength = (int)std::strlen(needle);
		int start = 0;
		int found = 0;
		while ((int)mMatchPositions.size() < kFindMaxMatches &&
			SearchForward(start, needle, matchCase, wholeWord, &found))
		{
			mMatchPositions.push_back(found);
			start = found + needleLength;
			if (start > mEditBuffer->length())
				break;
		}
		mMatchTotal = (int)mMatchPositions.size();
	}
	UpdateFindCount();
}

void DeckWindow::UpdateFindCount()
{
	if (!mFindCount)
		return;
	const std::string text = std::to_string(mMatchIndex) + "/" + std::to_string(mMatchTotal);
	mFindCount->copy_label(text.c_str());
}

// m085: select match #aIndex (0-based) of the current needle and move the
// caret there. Both directions go through this one function, so the "k/N"
// counter can never disagree with what is selected.
void DeckWindow::SelectMatchAt(int aIndex)
{
	if (aIndex < 0 || aIndex >= (int)mMatchPositions.size())
		return;
	const int pos = mMatchPositions[aIndex];
	const int length = (int)std::strlen(mFindInput->value());
	mEditBuffer->select(pos, pos + length);
	mEditor->insert_position(pos + length);
	mEditor->show_insert_position();
	mEditor->take_focus();
	mMatchIndex = aIndex + 1;
	UpdateFindCount();
}

// m085: the search walks the list of matches, so "下一个" and "上一个" agree
// with the counter by construction. The wrap switch decides what happens at
// the ends: on (the default) the search starts over, off it stops and
// says so.
void DeckWindow::FindNext()
{
	// m087: F3 on a folded bar unfolds it - the user asked for a search.
	SetFindBarVisible(true);
	const char *needle = mFindInput->value();
	if (!needle || !*needle)
	{
		Append("[find] 先输入要查找的文本");
		FocusFind();
		return;
	}
	if (mFindMatchesDirty)
		UpdateFindMatches();
	if (mMatchPositions.empty())
	{
		Append(std::string("[find] 未找到: ") + needle);
		return;
	}

	const int cursor = mEditor->insert_position();
	int target = -1;
	for (size_t i = 0; i < mMatchPositions.size(); ++i)
	{
		if (mMatchPositions[i] >= cursor)
		{
			target = (int)i;
			break;
		}
	}
	bool wrapped = false;
	if (target < 0)
	{
		if (!mWrap->value())
		{
			Append("[find] 已到末尾（未开启循环）");
			return;
		}
		target = 0;
		wrapped = true;
	}
	SelectMatchAt(target);
	Append(std::string("[find] ") + (wrapped ? "已回到开头，选中 1 处: " : "选中 1 处: ") + needle);
}

// m085: the backward search. The anchor is where the caret (or the
// selection) stands: the search looks for the match BEFORE it, and with the
// wrap switch on it falls back to the last match of the file.
void DeckWindow::FindPrev()
{
	// m087: Shift+F3 on a folded bar unfolds it too.
	SetFindBarVisible(true);
	const char *needle = mFindInput->value();
	if (!needle || !*needle)
	{
		Append("[find] 先输入要查找的文本");
		FocusFind();
		return;
	}
	if (mFindMatchesDirty)
		UpdateFindMatches();
	if (mMatchPositions.empty())
	{
		Append(std::string("[find] 未找到: ") + needle);
		return;
	}

	// The anchor is the start of the selection when there is one (a selected
	// match starts exactly there), otherwise the caret.
	int selStart = 0;
	int selEnd = 0;
	int anchor = mEditor->insert_position();
	if (mEditBuffer->selection_position(&selStart, &selEnd) && selEnd > selStart)
		anchor = selStart;

	// The match the anchor sits on, if any.
	int current = -1;
	for (size_t i = 0; i < mMatchPositions.size(); ++i)
	{
		if (mMatchPositions[i] == anchor)
		{
			current = (int)i;
			break;
		}
	}

	int target = -1;
	bool wrapped = false;
	if (current >= 0)
	{
		// The caret is on a match: the previous one is the one before it.
		target = current - 1;
		if (target < 0)
		{
			if (!mWrap->value())
			{
				Append("[find] 已经是第一处（未开启循环）");
				return;
			}
			target = (int)mMatchPositions.size() - 1;
			wrapped = true;
		}
	}
	else
	{
		// The caret is not on a match: the previous one is the last that
		// starts before it.
		for (size_t i = 0; i < mMatchPositions.size(); ++i)
		{
			if (mMatchPositions[i] < anchor)
				target = (int)i;
		}
		if (target < 0)
		{
			if (!mWrap->value())
			{
				Append("[find] 已经在第一处之前（未开启循环）");
				return;
			}
			target = (int)mMatchPositions.size() - 1;
			wrapped = true;
		}
	}
	SelectMatchAt(target);
	Append(std::string("[find] ") + (wrapped ? "已到最后，选中 1 处: " : "选中 1 处: ") + needle);
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
	// m086: "全部替换" honours the same switches as the search.
	const bool wholeWord = mWholeWord && mWholeWord->value();
	const int needleLength = (int)std::strlen(needle);
	const int replacementLength = (int)std::strlen(replacement);
	int start = 0;
	int found = 0;
	int count = 0;
	while (SearchForward(start, needle, matchCase, wholeWord, &found))
	{
		mEditBuffer->replace(found, found + needleLength, replacement);
		// Advance past the replacement: a replacement that contains the needle
		// must not be re-scanned forever.
		start = found + replacementLength;
		++count;
	}
	mEditBuffer->unselect();
	// m083: the focus goes back to the editor. It used to stay in the
	// replacement field, and then Ctrl+S (which only the editor answered)
	// went nowhere and the work was lost on disk.
	mEditor->show_insert_position();
	mEditor->take_focus();
	// m084: the text changed, so the counter has to be counted again.
	UpdateFindMatches();
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
	{
		// m083: the file is already open, so there is nothing to reload - but the
		// caret still belongs in it. Without this the row click keeps the focus
		// in the list and the keys the user types never reach the editor.
		mEditor->take_focus();
		return;
	}
	LoadIntoEditor(path); // saves the previous file first if it was dirty
}

// m089: the editor's right-click menu. The rule (开发指南 §4): the menu is
// assembled from the LIVE menu bar, so a new command shows up in it by itself
// - no second list to keep in step. The Edit part is the edit whitelist (the
// everyday actions), the 视图 part is the whole submenu, copied with its
// shortcuts and its check marks.
void DeckWindow::ShowEditorContextMenu()
{
	if (!mMenu)
		return;

	// The menu bar's top level: 文件 编辑 视图 脚本 设置.
	const Fl_Menu_Item *edit = nullptr;
	const Fl_Menu_Item *view = nullptr;
	for (const Fl_Menu_Item *t = mMenu->menu(); t && t->text; t = t->next(1))
	{
		if (!t->submenu())
			continue;
		if (std::strcmp(t->text, "编辑") == 0)
			edit = t;
		else if (std::strcmp(t->text, "视图") == 0)
			view = t;
	}
	if (!edit)
		return;

	Fl_Menu_Item popup[64] = {};
	int n = 0;

	// ── Edit 白名单（平铺）──
	for (const Fl_Menu_Item *it = edit + 1; it && it->text; ++it)
	{
		if (!IsContextEditItem(*it))
			continue;
		if (n + 2 >= 64)
			break;
		popup[n] = *it;             // the whole item: shortcut, flags, callback
		popup[n].flags &= ~FL_MENU_INACTIVE;
		++n;
	}
	if (n > 0)
		popup[n - 1].flags |= FL_MENU_DIVIDER;

	// ── 视图 ▶（逐项同款）──
	if (view && view->text && n + 2 < 64)
	{
		popup[n++] = {view->text, 0, nullptr, nullptr, FL_SUBMENU};
		for (const Fl_Menu_Item *it = view + 1; it && it->text; ++it)
		{
			if (n + 2 >= 64)
				break;
			popup[n] = *it;
			++n;
		}
		popup[n++] = {nullptr};      // end of the submenu
	}
	popup[n++] = {nullptr};          // end of the menu

	// popup() takes WIDGET-RELATIVE coordinates and adds the window offset
	// itself; the menu bar is passed as the menu button so the popup wears the
	// same font, colours and hover style (the same style as the menu bar).
	const Fl_Menu_Item *picked = popup[0].popup(Fl::event_x(), Fl::event_y(), nullptr, nullptr, mMenu);
	if (!picked || !picked->callback_)
		return;
	// popup() does not run the callback. The item's user data is the command
	// id, exactly like the menu bar's own items carry it.
	HandleMenu((int)(intptr_t)picked->user_data());
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
	mContextLabels.reserve(10);
	mContextItems.reserve(10);
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
	// m172: 编译为 EXE joins the per-script actions, right before 信息 (which
	// stays last - it is the "properties" row, not an action).
	add("编译为 EXE", 7);
	// m177: 删除 sits with the other per-script actions. No divider above 信息
	// (m153's rule still holds: a rule there split the menu in two halves).
	add("删除…", 8);
	// m153: no divider above 信息 after all. FL_MENU_DIVIDER on 重命名 drew a
	// rule that made a five-item menu read as two unrelated halves; Explorer's
	// 属性 sits in the same list without one.
	add("信息", 6);
	const Fl_Menu_Item end = { nullptr };
	mContextItems.push_back(end);

	// Fl_Menu_Item::popup() takes WIDGET-RELATIVE coordinates and adds the event's
	// root offset itself (Fl_Menu.cxx pulldown(): X += event_x_root()-event_x()).
	// Passing event_x_root() here double-added the window origin, so the menu
	// landed offset by the window position - invisible while the window sat near
	// (0,0), obvious as soon as the user moved it.
	// Pass the theme's menu bar as the menubutton (the editor menu does this): the
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
	case 6: // 信息
		ShowScriptInfo(aIndex);
		break;
	case 7: // 编译为 EXE
		CompileSelected(aIndex);
		break;
	case 8: // 删除…
		DeleteSelected();
		break;
	default:
		break;
	}
}

namespace
{

// 1204 -> "1,204". Local to this file; the info window is the only caller.
std::string GroupedDigits(long long aValue)
{
	const std::string raw = std::to_string(aValue);
	std::string out;
	int count = 0;
	for (size_t i = raw.size(); i-- > 0;)
	{
		out.insert(out.begin(), raw[i]);
		if (++count % 3 == 0 && i > 0)
			out.insert(out.begin(), ',');
	}
	return out;
}

} // namespace

void DeckWindow::ShowScriptInfo(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mScripts.size())
		return;
	const ScriptRow &row = mScripts[(size_t)aIndex];

	ScriptInspection scan;
	InspectScript(row.path, scan);

	// Display order is fixed here, not inherited from the file: three scripts
	// that list their keys in different orders must still read the same way.
	// Keys this build does not know are appended after the known ones, so a
	// future @tag is never silently dropped.
	struct Known
	{
		const char *key;
		const char *label;
		bool wrap;
	};
	static const Known kKnown[] = {
		{ "name", "名称", false },
		{ "version", "版本", false },
		{ "date", "日期", false },
		{ "author", "作者", false },
		{ "license", "许可", false },
		{ "description", "说明", true },
		{ "requires", "运行环境", true },
		{ "host", "宿主要求", true },
		{ "entry", "入口", true },
		{ "homepageURL", "主页", true },
	};
	// @namespace is deliberately not shown: it is the userscript-era identifier
	// and every script in the repository gives it the same value as
	// @homepageURL, so printing both would put the same URL on two rows.

	std::vector<InfoGroup> groups;
	InfoGroup header;
	if (scan.headerFound)
	{
		for (const Known &known : kKnown)
		{
			const std::string value = scan.Get(known.key);
			if (value.empty())
				continue;
			const bool isUrl = std::strcmp(known.key, "homepageURL") == 0;
			header.push_back({ known.label, value, known.wrap, isUrl });
		}
		for (const ScriptHeaderField &field : scan.fields)
		{
			if (_stricmp(field.key.c_str(), "namespace") == 0)
				continue;
			bool known = false;
			for (const Known &entry : kKnown)
			{
				if (_stricmp(field.key.c_str(), entry.key) == 0)
				{
					known = true;
					break;
				}
			}
			if (!known && !field.value.empty())
				header.push_back({ field.key, field.value, true, false });
		}
	}
	else
	{
		header.push_back({ "说明",
			"这个脚本开头没有 ==UserScript== 信息块，下面只有磁盘上的文件信息。",
			true, false });
	}
	groups.push_back(header);

	// The disk layer, which every .ahk has - this is what keeps a header-less
	// script from opening an empty window.
	InfoGroup file;
	file.push_back({ "文件大小", FormatScriptSize(scan.sizeBytes), false, false });
	file.push_back({ "修改时间", scan.modified, false, false });
	if (scan.lineCount >= 0)
		file.push_back({ "行数", GroupedDigits(scan.lineCount) + " 行", false, false });
	// Relative on purpose: the deck never shows a local absolute path. row.name
	// is the leaf FileName, suffix included (the list strips it for display).
	file.push_back({ "位置", "Scripts\\" + row.name, false, true });
	groups.push_back(file);

	ShowInfoDialog("脚本信息", groups, mRoundedCorners, mWindowShadow);
}

// ---------------------------------------------------------------------------
// m023/m024: start/stop switch, restart, the script's own tray menu, bindings, tray
// ---------------------------------------------------------------------------
// m108: the window is not a script, so its "show the main window" action gets a
// reserved entry in hotkeys.txt. Menu commands use "menu.<key>"; this one is
// "window.show" and has no built-in default.
const std::string kWindowShowKey = "window.show";
// Script rows use ids 1..N, so this id cannot collide with one of them.
// (0xC000..0xFFFF are reserved for DLL hotkeys, so 0x7FFF is safe.)
constexpr int kShowWindowHotkeyId = 0x7FFF;

// m108: an entry with no binding and no built-in default is not worth a line in
// hotkeys.txt, so it is removed rather than written empty.
void RemoveHotkeyEntry(std::vector<HotkeyEntry> &aEntries, const std::string &aName)
{
	auto it = std::find_if(aEntries.begin(), aEntries.end(),
		[&aName](const HotkeyEntry &aEntry) { return aEntry.name == aName; });
	if (it != aEntries.end())
		aEntries.erase(it);
}

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

	// m160: a script that emptied its own tray menu (A_TrayMenu.Delete()) turns
	// the message below into an EMPTY popup - to the kernel it is exactly "the
	// user right-clicked my tray icon". Ask the engine for the live HMENU first
	// (m192 P3: rebuilt host-side from the child's JSON snapshot, not exported
	// from an in-process kernel) and fall back to bringing the script's window
	// to the front when there is nothing to show. Every entry lands here - the
	// list context action, the gear cell, the menu-bar command and the tray
	// "running" fallback - so this one guard covers all of them.
	const HMENU scriptMenu = mEngine.TrayMenu(row.session);
	if (!scriptMenu || GetMenuItemCount(scriptMenu) == 0)
	{
		if (IsIconic(window))
			ShowWindowAsync(window, SW_RESTORE);
		// The deck owns the foreground right now (the user just clicked us), so
		// the hand-off is allowed; BringWindowToTop covers the rare refusal.
		if (!SetForegroundWindow(window))
			BringWindowToTop(window);
		Append("[" + UiClock() + "] " + row.name + " 没有托盘菜单，已前置它的窗口");
		return;
	}

	// AHK_NOTIFYICON (WM_USER+4) with lParam = WM_RBUTTONUP is the kernel's own
	// "the user right-clicked my tray icon" path: MainWindowProc calls
	// mTrayMenu->Display() with no coordinates, and Display() then reads
	// GetCursorPos itself (.3rd\AutoHotkey-v2.0.31-src\source\script2.cpp:281-311).
	// m192 P3: AHKDECK_NO_TRAY only suppresses the icon, NOT this handler, and
	// the snapshot hook already forced UserMenu::CreateHandle(), so the child
	// can still display its own menu on this path. That is why no point is
	// passed here: A_TrayMenu.Show(x, y)
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
	// m090: one combo, one action. A new assignment takes the combo over: the
	// action that owned it - a menu command or another script - is cleared
	// instead of the edit being refused, so the table can never hold two rows
	// with the same combo. The menu defaults count as owned even when the file
	// has no entry for them, which is why they are read from the binding in
	// force instead of from mHotkeys alone.
	std::string previousOwner;
	bool clearedMenu = false;
	if (!typed.empty())
	{
		// m108: the window's own action is one of the owners too.
		if (SameShortcut(ShowWindowBinding(), canonical))
		{
			previousOwner = "显示主窗口";
			RemoveHotkeyEntry(mHotkeys, kWindowShowKey);
		}
		for (size_t i = 0; i < Commands().size(); ++i)
		{
			if (!SameShortcut(MenuCommandBinding((int)i), canonical))
				continue;
			previousOwner = Commands()[i].label;
			SetHotkeyEntry(mHotkeys, std::string("menu.") + Commands()[i].key, std::string());
			clearedMenu = true;
		}
		for (size_t i = 0; i < mHotkeys.size(); ++i)
		{
			HotkeyEntry &entry = mHotkeys[i];
			if (entry.name == row.name || entry.name.compare(0, 5, "menu.") == 0 ||
				entry.name == kWindowShowKey)
				continue;
			if (!SameShortcut(entry.hotkey, canonical))
				continue;
			previousOwner = entry.name;
			entry.hotkey.clear();
		}
	}
	SetHotkeyEntry(mHotkeys, row.name, typed.empty() ? std::string() : canonical);
	std::string saveError;
	if (!SaveHotkeys(mHotkeyPath, mHotkeys, saveError))
	{
		Append("[hotkey] 保存失败：" + saveError);
		return;
	}
	if (clearedMenu)
		ApplyMenuShortcuts();
	RebindHotkeys();
	RefreshList();
	std::string message = typed.empty()
		? "[" + UiClock() + "] 已解除 " + row.name + " 的快捷键"
		: "[" + UiClock() + "] " + row.name + " 的快捷键 = " + canonical;
	if (!previousOwner.empty())
		message += "（" + previousOwner + " 已解除）";
	Append(message);
}

// Settings > 快捷键 (OK): the dialog keeps its own table until the user agrees,
// then writes every row here. One save + one rebind for the whole table, so a
// half-applied dialog can never leave the hotkey file ahead of the hooks.
bool DeckWindow::ApplyShortcutRows(const std::vector<std::pair<std::string, std::string>> &aRows)
{
	// m090: shortcuts are unique. The dialog already clears the previous owner
	// while recording, but a hand-edited file can still carry duplicates; the
	// last row keeps the combo and every earlier one is cleared here, so the
	// invariant holds for whatever path wrote the table.
	std::vector<std::pair<std::string, std::string>> rows = aRows;
	MakeShortcutsUnique(rows);

	for (const auto &row : rows)
	{
		// m108: the window's binding has no built-in default, so an unbound row
		// removes its entry - writing "window.show=" would only be litter.
		if (row.first == kWindowShowKey && row.second.empty())
		{
			RemoveHotkeyEntry(mHotkeys, row.first);
			continue;
		}
		// m088: a menu command's built-in binding needs no entry in the file,
		// so a row that is back at its default removes its entry - that is
		// how "恢复默认" works without writing the defaults down.
		if (row.first.compare(0, 5, "menu.") == 0)
		{
			const std::string key = row.first.substr(5);
			std::string def;
			for (size_t i = 0; i < Commands().size(); ++i)
			{
				if (Commands()[i].key == key)
					def = MenuCommandDefault((int)i);
			}
			// m090: an empty entry means "this built-in binding was cleared",
			// so it is only worth writing when there WAS a default. A command
			// that is simply unbound (no default, no combo) leaves no row -
			// otherwise every keyless command would litter hotkeys.txt.
			if ((!def.empty() && row.second == def) || (def.empty() && row.second.empty()))
			{
				for (size_t i = 0; i < mHotkeys.size(); ++i)
				{
					if (mHotkeys[i].name != row.first)
						continue;
					mHotkeys.erase(mHotkeys.begin() + (long)i);
					break;
				}
				continue;
			}
		}
		SetHotkeyEntry(mHotkeys, row.first, row.second);
	}
	std::string error;
	if (!SaveHotkeys(mHotkeyPath, mHotkeys, error))
	{
		Append("[hotkey] 保存失败：" + error);
		return false;
	}
	// m088: the menu shows the binding that is now in force.
	ApplyMenuShortcuts();
	RebindHotkeys();
	RefreshList();
	int bound = 0;
	for (const auto &row : rows)
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

// m092: Help > Check for Updates. The URLs come from settings.json; an absent
// key means the built-in mirror, an explicitly empty string disables a source
// (a mirror the user does not trust must be switchable off, not merely
// deprioritised).
void DeckWindow::ShowUpdateDialog()
{
	Json update = JsonObjectMember(mConfigRoot, "update");
	const std::string gitee = update.Has("urlGitee")
		? update["urlGitee"].AsString() : std::string(kDefaultUpdateUrlGitee);
	const std::string github = update.Has("urlGithub")
		? update["urlGithub"].AsString() : std::string(kDefaultUpdateUrlGithub);
	// m179: the mirror comes from 选项 → 官方下载源; this dialog only reads it.
	const int source = OfficialDownloadSource();

	UpdateDialog dialog(VersionString(), gitee, github, source,
		[this]() { return ConfirmRestartForUpdate(); });
	dialog.SetFrameOptions(mRoundedCorners, mWindowShadow);
	dialog.Run();
}

// m146: File > 获取脚本…. The dialog is deliberately a DeckWindow member so
// it can reach the private rescan/stop abilities without a second public API.
// Everything the dialog needs is injected here as a callback.
void DeckWindow::ShowScriptDownloadDialog()
{
	Json node = JsonObjectMember(mConfigRoot, "scripts");
	const std::string gitee = node.Has("manifestGitee")
		? node["manifestGitee"].AsString() : std::string(kDefaultCatalogUrlGitee);
	const std::string github = node.Has("manifestGithub")
		? node["manifestGithub"].AsString() : std::string(kDefaultCatalogUrlGithub);
	// m179: the mirror is a global preference now (选项 → 官方下载源); this
	// window resolves it and never writes it back.
	const int source = OfficialDownloadSource();

	ScriptDownloadCallbacks callbacks;
	callbacks.saveCustom = [this](const std::vector<CustomCatalogSource> &aSources) {
		SaveCustomSources(aSources);
	};
	callbacks.refreshScripts = [this]() { RefreshFromDisk(); };
	callbacks.stopScript = [this](const std::string &aName) {
		const std::string leaf = aName + ".ahk";
		for (const ScriptRow &row : mScripts)
		{
			if (_stricmp(row.name.c_str(), leaf.c_str()) != 0)
				continue;
			if (!row.session)
				return true; // present but not running: nothing to stop
			if (!mEngine.StopSession(row.session))
				return false;
			InvalidateSessions();
			RefreshList();
			// The in-process engine releases the file asynchronously; give it a
			// moment so the downloader's atomic swap can actually take the name.
			Sleep(300);
			return true;
		}
		return true; // a brand-new download has no running session
	};
	callbacks.log = [this](const std::string &aLine) { Append(aLine); };

	ScriptDownloadDialog dialog(
		source == 1 ? ManifestSource::OfficialGithub : ManifestSource::OfficialGitee,
		source == 1 ? github : gitee, CustomCatalogSources(), callbacks);
	dialog.SetFrameOptions(mRoundedCorners, mWindowShadow);
	dialog.Run();
}

void DeckWindow::OpenMcpDocs()
{
	// m098: 帮助 > MCP 接入 opens the doc itself - it is the only place the
	// client config is written down, so there is nothing for a dialog to keep
	// in sync.
	const std::wstring path = ExeDirectory() + L"\\docs\\MCP接入.md";
	if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		Append("未找到 docs\\MCP接入.md；发行包里应带有 docs 文件夹。");
		return;
	}
	// m099: hand it to the shell so it opens in whatever the user actually reads
	// markdown with - forcing Notepad was a guess about their setup. ShellExecute
	// returning <= 32 means "no handler took it".
	const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
		SW_SHOWNORMAL);
	if ((INT_PTR)result <= 32)
		Append("没有程序能打开 docs\\MCP接入.md（.md 未关联任何编辑器）。");
}

// The update replaces the exe and restarts it, so unsaved edits must not
// disappear with it. An existing file is saved (that is what 保存 does, and
// the update the user just requested implies closing); a never-saved new
// script cannot be saved without a name, so the update is cancelled instead
// of opening the name dialog from underneath a closing window.
bool DeckWindow::ConfirmRestartForUpdate()
{
	if (!mEditDirty)
		return true;
	if (mEditPath.empty())
	{
		Append("[update] 编辑区有未保存的新脚本，已取消更新；请先保存再试");
		return false;
	}
	if (!SaveEditor())
	{
		Append("[update] 保存编辑区失败，已取消更新");
		return false;
	}
	Append("[update] 已保存当前脚本，准备重启以完成更新");
	return true;
}


void DeckWindow::LoadHotkeyFile()
{
	mHotkeyPath = AppHotkeyPath();
	if (mHotkeyPath.empty())
		return;
	std::string error;
	if (!LoadHotkeys(mHotkeyPath, mHotkeys, error))
	{
		Append("[hotkey] 读取失败: " + error);
		return;
	}

	// m106: the dialog prevents these, a hand-edited file does not. Reporting to
	// the console is the whole fix - the loader must not silently rewrite the
	// file or drop a binding (m090 keeps normalization in the save path).
	const std::vector<HotkeyIssue> issues = AuditHotkeys(mHotkeys);
	for (const HotkeyIssue &issue : issues)
		Append("[hotkey] " + issue.name + " " + issue.hotkey + "：" + issue.message);
}

void DeckWindow::RebindHotkeys()
{
	mHotkeyTargets.clear();
	if (!mHooks.Window())
		return; // no control window: the hotkey half of the deck is unavailable
	mHooks.UnregisterHotkeys();

	const std::string showKeyText = HotkeyOf(mHotkeys, kWindowShowKey);
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
	// m108: the window's own shortcut, after the scripts. Its id is reserved, so
	// a removed script row can never shift it onto another action.
	if (!showKeyText.empty())
	{
		unsigned int mods = 0;
		unsigned int virtualKey = 0;
		std::string canonical;
		std::string error;
		if (!ParseAhkHotkey(showKeyText, mods, virtualKey, canonical, error))
		{
			Append("[hotkey] 显示主窗口 " + showKeyText + "：" + error);
		}
		else
		{
			std::string registerError;
			if (!mHooks.RegisterHotkey(kShowWindowHotkeyId, mods, virtualKey, registerError))
			{
				Append("[hotkey] 显示主窗口 " + canonical + " 未生效：" + registerError);
			}
			else
			{
				++bound;
			}
		}
	}
	if (bound)
		Append("[" + UiClock() + "] 全局快捷键已绑定 " + std::to_string(bound) + " 个");
}

void DeckWindow::ShowAbout()
{
	// Modal and self-sizing (it fits its own widest line), so nothing to do
	// afterwards - unlike ShowOptions, which writes settings back.
	ShowAboutDialog(mRoundedCorners, mWindowShadow);
}

void DeckWindow::ShowSubscribeManager()
{
	// The window persists through the callback, exactly as it does when opened
	// from 获取脚本; here the log sink is the only thing that needs the deck.
	SubscribeCallbacks callbacks;
	callbacks.save = [this](const std::vector<CustomCatalogSource> &aSources) {
		SaveCustomSources(aSources);
	};
	callbacks.log = [this](const std::string &aLine) { Append(aLine); };

	std::vector<CustomCatalogSource> updated;
	ShowSubscribeDialog(CustomCatalogSources(), mRoundedCorners, mWindowShadow,
		callbacks, updated);
}

int DeckWindow::OfficialDownloadSource() const
{
	// 0 = Gitee, 1 = GitHub. One value for four consumers: the script catalog
	// URL, which mirror a script downloads from, the update manifest URL, and
	// which release asset the updater takes. Anything unexpected reads as 0.
	const Json &download = mConfigRoot["download"];
	if (download.IsObject() && download.Has("source"))
		return download["source"].AsInt(0) == 1 ? 1 : 0;
	// Pre-m179 settings: the two dialogs each kept their own copy of the same
	// preference. Read whichever survived so an existing install is not reset.
	const Json &update = mConfigRoot["update"];
	if (update.IsObject() && update.Has("source"))
		return update["source"].AsInt(0) == 1 ? 1 : 0;
	const Json &scripts = mConfigRoot["scripts"];
	if (scripts.IsObject() && scripts.Has("source"))
		return scripts["source"].AsInt(0) == 1 ? 1 : 0;
	return 0;
}

std::vector<CustomCatalogSource> DeckWindow::CustomCatalogSources() const
{
	std::vector<CustomCatalogSource> sources;
	const Json &custom = mConfigRoot["scripts"]["custom"];
	if (!custom.IsArray())
		return sources;
	for (size_t i = 0; i < custom.Size(); ++i)
	{
		const Json &item = custom[i];
		CustomCatalogSource source;
		if (item.IsObject())
		{
			// m180: url plus the name the manifest reported for itself. A bare
			// string is the m179 shape - still loadable, just uncached (the
			// 订阅管理 window will show the host until the next fetch).
			source.url = item["url"].AsString();
			source.name = item["name"].AsString();
		}
		else
		{
			source.url = item.AsString();
		}
		if (!source.url.empty())
			sources.push_back(std::move(source));
	}
	return sources;
}

void DeckWindow::SaveCustomSources(const std::vector<CustomCatalogSource> &aSources)
{
	Json scripts = JsonObjectMember(mConfigRoot, "scripts");
	Json list = Json::MakeArray();
	for (const CustomCatalogSource &source : aSources)
	{
		// m180: an object, not a bare URL string - the name the manifest
		// reported has to survive a restart so 订阅管理 can still title a
		// source it cannot reach today.
		Json item = Json::MakeObject();
		item.Set("url", Json(source.url));
		if (!source.name.empty())
			item.Set("name", Json(source.name));
		list.Push(item);
	}
	scripts.Set("custom", list);
	mConfigRoot.Set("scripts", scripts);
	std::string error;
	if (!SaveSettings(&error))
		Append("[settings] 订阅来源未能写入: " + error);
}

void DeckWindow::ShowOptions()
{
	const int oldSource = OfficialDownloadSource();
	OptionsResult chosen;
	if (!ShowOptionsDialog(mHousekeepingDays, oldSource, mRoundedCorners,
		mWindowShadow, chosen))
		return;

	const bool daysChanged = chosen.days != mHousekeepingDays;
	const bool sourceChanged = chosen.source != oldSource;
	mHousekeepingDays = chosen.days;
	if (daysChanged || sourceChanged)
	{
		std::string error;
		if (daysChanged)
		{
			Json housekeeping = JsonObjectMember(mConfigRoot, "housekeeping");
			housekeeping.Set("max_age_days", Json((double)chosen.days));
			mConfigRoot.Set("housekeeping", housekeeping);
		}
		if (sourceChanged)
		{
			// m179: ONE setting for both manifests and both download paths -
			// the script catalog and the updater read it, neither writes it.
			Json download = JsonObjectMember(mConfigRoot, "download");
			download.Set("source", Json((double)chosen.source));
			mConfigRoot.Set("download", download);
		}
		if (!SaveSettings(&error))
			Append("[settings] 保存失败：" + error);
	}
	if (sourceChanged)
		Append("[download] 官方下载源：" +
			std::string(chosen.source == 1 ? "GitHub" : "Gitee"));
	// Run it now: a user who just picked 7 天 should not have to restart to see
	// the folder shrink. The same pass reports how many files went.
	RunHousekeeping();
	Append("[cleanup] 临时文件保留时长：" +
		(chosen.days <= 0 ? std::string("永不") : std::to_string(chosen.days) + " 天"));
}

void DeckWindow::RunHousekeeping()
{
	// The four subdirectories are a FIXED layout, but the pass removes any
	// directory it empties (that is how backups\<timestamp> disappears). So the
	// layout is restored on the way out - including on the 「永不」 path, where
	// nothing is pruned but a hand-deleted temp\ still has to come back.
	const auto restoreLayout = []() { EnsureAppTempLayout(); };
	if (mHousekeepingDays <= 0)
	{
		restoreLayout();
		return;
	}
	FILETIME now = {};
	GetSystemTimeAsFileTime(&now);
	const std::uint64_t nowTicks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
		static_cast<std::uint64_t>(now.dwLowDateTime);
	// m177: ONE root, one pass. logs\, backups\, work\ and deleted\ are cleaned
	// by the same rule; the walk is recursive and skips junctions.
	const int removed = [nowTicks, this]() {
		std::string error;
		const int count = PruneOlderThan(AppTempRoot(false), mHousekeepingDays, nowTicks, error);
		if (!error.empty())
			Append("[cleanup] temp：" + error);
		return count;
	}();
	restoreLayout();
	if (removed > 0)
		Append("[" + UiClock() + "] 已清理 " + std::to_string(removed) + " 个超过 " +
			std::to_string(mHousekeepingDays) + " 天的临时文件");
}

void DeckWindow::ShowFromTray()
{
	show();
	// SW_RESTORE also covers the minimised case; a global hotkey is an explicit
	// user gesture too, so the deck may take the foreground back.
	if (HWND hwnd = (HWND)fl_xid(this))
	{
		ShowWindow(hwnd, SW_RESTORE);
		SetForegroundWindow(hwnd);
	}
	// hide()/show() may have rebuilt the HWND, and every per-handle frame
	// artefact died with the old one: the rounded-corner opt-in prop, the DWM
	// non-client opt-out behind the 1px border, the grown outer rect. Detect a
	// fresh handle and reinstall the whole frame; a plain sync covers restore.
	if (windowChrome::frameInstalled(this))
		SyncDeckFrame(this);
	else
		InstallFrame();
	// The tick skipped rendering while hidden; catch up immediately.
	InvalidateSessions();
	RefreshList();
	UpdateStatus();
	// m114: the pointer that clicked 关闭 never left, so nothing told the caption
	// to drop its hover tint. Repaint it now against the real cursor position
	// instead of waiting for the next mouse move.
	if (mTitleBar)
	{
		mTitleBar->RefreshHoverFromCursor();
		mTitleBar->redraw();
	}
}

void DeckWindow::HandleIntent(const NativeHooks::Intent &aIntent)
{
	switch (aIntent.kind)
	{
	case NativeHooks::Intent::Kind::Hotkey:
	{
		// m108: the window's own shortcut (the tray menu's 主窗口 action). Its id
		// is only registered when the user bound a key, so a Hotkey intent that
		// carries it is a real binding.
		if (aIntent.hotkeyId == kShowWindowHotkeyId)
		{
			ShowFromTray();
			break;
		}
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
		ShowFromTray();
		break;
	case NativeHooks::Intent::Kind::TrayMenu:
	{
		// m131: a script picked in the tray menu's 运行中 submenu. The submenu was
		// built from a live snapshot, but the script may have exited since the
		// click: FindScript misses rows that a rescan removed, and OpenScriptMenu
		// reports its "not running" guard line for rows whose session is gone.
		const int index = FindScript(aIntent.scriptPath);
		if (index >= 0)
			OpenScriptMenu(index);
		break;
	}
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
	// m192 P3: the engine is now out-of-process and the in-process DLL is gone.
	// The status bar must read the very EXE Engine::Load resolves (optionally
	// overridden by AHKDECK_KERNEL), not the retired AutoHotkey64.dll.
	std::wstring kernel = ExeDirectory() + L"\\AutoHotkey64.exe";
	wchar_t override[MAX_PATH] = {0};
	if (GetEnvironmentVariableW(L"AHKDECK_KERNEL", override, MAX_PATH) > 0)
		kernel = override;
	mKernelSummary = KernelVersionOf(kernel);
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
	mMenu->labelsize(TextSize());
	mMenu->textsize(TextSize());
	mMenu->setPressColor(colors.highlight2);

	// m031: only the contextual bars still have buttons.
	Fl_Button *buttons[] = {mFindNext, mFindPrev, mReplaceOne, mReplaceAll, mFindClose};
	for (Fl_Button *button : buttons)
		StyleButton(button);
	// m087: the switch is a button like the others, only its "X" is smaller
	// so it stays quiet in the corner of the row.
	if (mFindClose)
		mFindClose->labelsize(SubTextSize());

	StyleInput(mFilter);
	if (auto *search = dynamic_cast<SearchInput *>(mFilter))
		search->applyBox(); // StyleInput's FL_BORDER_BOX would undo the 24px inset
	StyleInput(mFindInput);
	StyleInput(mReplaceInput);
	StyleTextDisplay(mEditor);
	StyleTextDisplay(mConsole);

	// m086: the switches are drawn by DeckCheckButton, exactly like the menu
	// bar's check mark. All they need from the theme is the text colour and
	// the font size.
	for (DeckCheckButton *check : {mMatchCase, mWrap, mWholeWord})
	{
		if (!check)
			continue;
		check->labelcolor(colors.text1);
		check->labelsize(TextSize());
	}

	// m086: the band the find bar sits on uses the script list's header
	// background, so the bar belongs to the deck.
	mFindBarBg->box(FL_FLAT_BOX);
	mFindBarBg->color(colors.background3);

	// m084: the match counter is a read-only note, not a field: no box, and
	// the quiet text colour.
	mFindCount->box(FL_NO_BOX);
	mFindCount->align(FL_ALIGN_CENTER);
	mFindCount->labelsize(SubTextSize());
	mFindCount->labelcolor(colors.text2);

	mList->box(FL_FLAT_BOX);
	mList->color(colors.background1);
	mList->textcolor(colors.text1);
	// m037: the browser still tracks a row for keyboard commands, but it must
	// not paint a selected band - the row is a set of buttons, not a mode.
	mList->selection_color(colors.background1);
	mList->textsize(TextSize());
	mList->scrollbar.type(FL_VERT_SLIDER);
	mList->scrollbar.box(FL_FLAT_BOX);
	mList->scrollbar.color(colors.background1);
	mList->scrollbar.selection_color(colors.background2);

	mStatus->box(FL_FLAT_BOX);
	mStatus->color(colors.background3);
	mStatus->labelcolor(colors.text2);
	mStatus->labelsize(SubTextSize());

	mEditorBar->box(FL_FLAT_BOX);
	mEditorBar->color(colors.background3);

	// Tool-window buttons: background2 fill on the grey bar, 1px themed
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
		button->labelsize(TextSize());
	}



	mEditor->textfont(FL_COURIER);
	mEditor->textsize(TextSize());
	// The gutter shares the editor background and the text colour; the
	// only divider is the 1px dashed rule drawn by EditorBox.
	mEditor->linenumber_font(FL_COURIER);
	mEditor->linenumber_size(TextSize());
	mEditor->linenumber_fgcolor(colors.text1);
	mEditor->linenumber_bgcolor(colors.background1);
	UpdateLineNumberWidth();
	mConsole->textfont(FL_HELVETICA);
	mConsole->textsize(TextSize());

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

	// Same measurement as the shared updateLinenumberWidth rule: the
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
	// m112: the script list is the deck's spine and always visible, so only the
	// editor column has a visibility state to apply.
	for (Fl_Widget *widget : mListWidgets)
		widget->show();
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

// m084: the find field reports "the text changed" and "Enter was pressed" as
// two different reasons. The counter follows the text, the search follows
// Enter - the old bar only had the second one.
void DeckWindow::OnFindInput(Fl_Widget *, void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	if (!self)
		return;
	if (Fl::callback_reason() == FL_REASON_CHANGED)
		self->UpdateFindMatches();
	else
		self->FindNext();
}

// m088: the shortcut dialog's names. A menu command is "菜单 > 文件 > 新建"
// (shape: the group, then the path), a command that owns a button but
// no menu item is "窗口 > 中部条带 > 保存".
std::string DeckWindow::MenuCommandName(int aIndex) const
{
	const CommandSpec &cmd = Commands()[(size_t)aIndex];
	std::string name = cmd.group;
	name += " > ";
	name += (cmd.path && *cmd.path) ? PathLabel(cmd.path) : cmd.label;
	return name;
}

std::string DeckWindow::MenuCommandKey(int aIndex) const
{
	return Commands()[(size_t)aIndex].key;
}

// The binding in force: the user's own entry if the file has one (an empty
// entry means "no key"), otherwise the built-in default.
std::string DeckWindow::MenuCommandBinding(int aIndex) const
{
	const CommandSpec &cmd = Commands()[(size_t)aIndex];
	const std::string name = std::string("menu.") + cmd.key;
	for (const auto &entry : mHotkeys)
	{
		if (entry.name != name)
			continue;
		return entry.hotkey; // "" = the user cleared it
	}
	return AhkFromFltkShortcut(cmd.shortcut);
}

std::string DeckWindow::MenuCommandDefault(int aIndex) const
{
	return AhkFromFltkShortcut(Commands()[(size_t)aIndex].shortcut);
}

// m108: no built-in default - the binding is whatever the user assigned.
std::string DeckWindow::ShowWindowBinding() const
{
	return HotkeyOf(mHotkeys, kWindowShowKey);
}

int DeckWindow::ShortcutOf(int aId) const
{
	for (size_t i = 0; i < Commands().size(); ++i)
	{
		if (Commands()[i].id == aId)
			return FltkShortcutFromAhk(MenuCommandBinding((int)i));
	}
	return 0;
}

// m088: the key dispatcher. The editor asks first (it owns the keyboard when
// the text is being edited), then the window. Every command goes through the
// table, so a re-bound key works exactly like the built-in one.
int DeckWindow::DispatchShortcut()
{
	// m089: FLTK keeps Fl::event_key() from the LAST key that went down, so a
	// key event that carries a character still reports the PREVIOUS key. The
	// shortcut test alone would eat the character: after one Delete, every
	// typed character came back as another Delete (measured 2026-11-07: the
	// editor stayed empty and the file was saved empty).
	// FLTK's own rule is the one compose() uses: a printable character is
	// text, not a shortcut. The same test here keeps the two in step.
	if (Fl::event_length() > 0 && Fl::event_text())
	{
		const unsigned char c = (unsigned char)Fl::event_text()[0];
		if ((c & ~31) && c != 127)
			return 0;
	}
	Fl_Widget *focus = Fl::focus();
	for (size_t i = 0; i < Commands().size(); ++i)
	{
		const CommandSpec &cmd = Commands()[i];
		const int shortcut = ShortcutOf(cmd.id);
		if (!shortcut)
			continue;
		if (!Fl::test_shortcut((unsigned int)shortcut))
			continue;
		// The text commands belong to the editor. When another widget owns
		// the keyboard (the find field, the list's filter), the key is not
		// ours - pasting into the find field must not paste into the code.
		if (cmd.editorOnly && focus != mEditor)
			continue;
		HandleMenu(cmd.id);
		return 1;
	}
	return 0;
}

// m088: write the user's rebinding into the menu items, so the menu shows
// the key that is actually in force.
void DeckWindow::ApplyMenuShortcuts()
{
	if (!mMenu)
		return;
	for (const CommandSpec &cmd : Commands())
	{
		if (!cmd.path || !*cmd.path)
			continue;
		const int index = mMenu->find_index(cmd.path);
		if (index < 0)
			continue;
		mMenu->shortcut(index, ShortcutOf(cmd.id));
	}
}

// m088: the 编辑 menu's commands. They act on the editor's text, which is
// what the 编辑 menu acts on.
void DeckWindow::EditUndo()
{
	if (!mEditBuffer || !mEditBuffer->can_undo())
		return;
	int pos = 0;
	mEditBuffer->undo(&pos);
	mEditor->insert_position(pos);
	mEditor->show_insert_position();
}

void DeckWindow::EditRedo()
{
	if (!mEditBuffer || !mEditBuffer->can_redo())
		return;
	int pos = 0;
	mEditBuffer->redo(&pos);
	mEditor->insert_position(pos);
	mEditor->show_insert_position();
}

void DeckWindow::EditCopy()
{
	if (!mEditBuffer)
		return;
	int start = 0;
	int end = 0;
	if (!mEditBuffer->selection_position(&start, &end) || end <= start)
		return;
	char *text = mEditBuffer->text_range(start, end);
	if (!text)
		return;
	Fl::copy(text, (int)std::strlen(text), 1);
	std::free(text);
}

void DeckWindow::EditCut()
{
	if (!mEditBuffer)
		return;
	int start = 0;
	int end = 0;
	if (!mEditBuffer->selection_position(&start, &end) || end <= start)
		return;
	EditCopy();
	mEditBuffer->remove(start, end);
	mEditor->insert_position(start);
}

void DeckWindow::EditPaste()
{
	// FLTK delivers the clipboard to the widget as an FL_PASTE event; the
	// editor takes it there (EditorBox::handle).
	if (mEditor)
		Fl::paste(*mEditor, 1);
}

void DeckWindow::EditDelete()
{
	if (!mEditBuffer)
		return;
	int start = 0;
	int end = 0;
	// m089: Delete is this command's default key now (the 编辑 > Delete
	// carries FL_Delete too). The editor asks the command table BEFORE its
	// own key handling, so the command has to do exactly what the editor's
	// Delete does - otherwise the key would swallow character deletion.
	// The delete command is the same two-step rule: with a selection remove
	// it, without one delete the character at the cursor.
	if (mEditBuffer->selection_position(&start, &end) && end > start)
	{
		mEditBuffer->remove(start, end);
		mEditor->insert_position(start);
		return;
	}
	if (mEditor)
		Fl_Text_Editor::kf_delete(0, mEditor);
}

void DeckWindow::EditSelectAll()
{
	if (!mEditBuffer || !mEditor)
		return;
	mEditBuffer->select(0, mEditBuffer->length());
	mEditor->insert_position(mEditBuffer->length());
	mEditor->show_insert_position();
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
	case IdLogs: OpenTempFolder(); break;
	case IdQuit: RequestQuit(); hide(); break;
	case IdHideTray:
		if (mHooks.HasTrayIcon())
			HideToTray();
		else
			Append("[ui] 没有托盘可用（控制台开头的 [deck] 行说明了原因），关闭即退出");
		break;
	// m089: the find bar is a toggle - the menu item (and its key) brings it
	// up and takes it away again.
	case IdFind: ToggleFindBar(false); break;
	case IdReplace: ToggleFindBar(true); break;
	// m088: the 编辑 menu and the band buttons go through the same table as
	// the keys, so a re-bound key and a click do the same thing.
	case IdFindNext: FindNext(); break;
	case IdFindPrev: FindPrev(); break;
	case IdEditUndo: EditUndo(); break;
	case IdEditRedo: EditRedo(); break;
	case IdEditCut: EditCut(); break;
	case IdEditCopy: EditCopy(); break;
	case IdEditPaste: EditPaste(); break;
	case IdEditDelete: EditDelete(); break;
	case IdEditSelectAll: EditSelectAll(); break;
	case IdCloseScriptEdit: CloseScript(); break;
	case IdRunOrStopEdit: RunOrStopEditing(); break;
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
	case IdViewEditor:
	{
		// Toggle from our own state, not FLTK's mvalue(): a keyboard shortcut or
		// a menu click can leave mvalue pointing at a different item, which made
		// the item only ever close. m112: the script list is always shown, so
		// this is a plain on/off for the editor column.
		mShowEditor = !mShowEditor;
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
	case IdCompileExe: CompileSelected(SelectedIndex()); break;
	case IdDeleteScript: DeleteSelected(); break;
	case IdOptions: ShowOptions(); break;
	case IdRescan: RefreshFromDisk(); break;
	case IdScriptDownload: ShowScriptDownloadDialog(); break;
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
		ShowAbout();
		break;
	case IdCheckUpdate:
		ShowUpdateDialog();
		break;
	case IdMcpAccess:
		OpenMcpDocs();
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
	mToolEditPending = true; // m082: not saved until the user acts
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
	// The gutter width is remeasured here rather than in the buffer's modified
	// callback: the buffer must not be queried while FLTK is changing it.
	if (self->mLineNumberDirty)
	{
		self->mLineNumberDirty = false;
		self->UpdateLineNumberWidth();
	}
	// m084: the match counter follows the editing area. It is counted here
	// (not inside the buffer's modified callback) for the same reason the
	// gutter width is: the buffer must not be queried while FLTK changes it.
	if (self->mFindMatchesDirty)
		self->UpdateFindMatches();
	self->TrackSessions();

	// m110: edge-triggered working-set trim. TrackSessions reaps finished
	// sessions; when the count of live ones drops to zero, one 2 s one-shot
	// timer hands the kernel's now-dead allocations back to the OS. Running
	// sessions own the pages being trimmed, so the edge (and the callback's own
	// re-check) is what keeps the trim away from live scripts.
	bool anyRunning = false;
	for (const Session &s : self->CachedSessions())
		if (s.state == SessionState::Running || s.state == SessionState::Stopping)
		{
			anyRunning = true;
			break;
		}
	if (self->mHadRunning && !anyRunning && !self->mTrimPending)
	{
		self->mTrimPending = true;
		Fl::add_timeout(2.0, TrimWorkingSetCb, self);
	}
	self->mHadRunning = anyRunning;

	// Timed save: the edited file is persisted every 30 s while dirty. The
	// explicit save-before-switch path uses the same SaveEditorTo.
	const ULONGLONG now = GetTickCount64();
	if (now - self->mLastTrayCheck >= 2000)
	{
		self->mLastTrayCheck = now;
		self->mHooks.EnsureTrayIcon();
	}
	// m107: the resource numbers move slowly, and the sampler's GPU query wants
	// a second collection between readings - so this runs on the same 2 s beat
	// rather than on every 100 ms tick.
	if (now - self->mLastResourceCheck >= 2000)
	{
		self->mLastResourceCheck = now;
		self->mResourceText = FormatResources(self->mResourceSampler.Sample());
		self->UpdateStatus();
	}
	if (self->mEditDirty && !self->mToolEditPending && !self->mEditPath.empty() &&
		now - self->mLastAutoSave >= (ULONGLONG)self->mAutoSaveMs)
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

// m110: fires 2 s after the last session ended. Re-check first: a script
// started inside the window owns live pages, and trimming under it would page
// its working data out for nothing. Resetting mTrimPending here (not only on
// the success path) keeps the next stop edge able to re-arm the timer.
void DeckWindow::TrimWorkingSetCb(void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	self->mTrimPending = false;
	for (const Session &s : self->CachedSessions())
		if (s.state == SessionState::Running || s.state == SessionState::Stopping)
			return;
	SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

// m173: the save dialog (comdlg32 + shell namespace pages) inflates the
// working set by megabytes that the deck never touches again. Pecila's rule,
// kept verbatim: trim two seconds after EVERY close - open/close is a paid
// fault, and the next open reloads the shell pages regardless. Same guard as
// TrimWorkingSetCb: live sessions own pages; a compile in flight wants its
// own pages too (the worker + console append).
void DeckWindow::ScheduleSaveTrim()
{
	if (mSaveTrimPending)
		Fl::remove_timeout(SaveTrimCb, this);
	mSaveTrimPending = true;
	Fl::add_timeout(2.0, SaveTrimCb, this);
}

void DeckWindow::SaveTrimCb(void *aSelf)
{
	auto *self = static_cast<DeckWindow *>(aSelf);
	self->mSaveTrimPending = false;
	if (self->mCompileBusy)
		return;
	for (const Session &s : self->CachedSessions())
		if (s.state == SessionState::Running || s.state == SessionState::Stopping)
			return;
	SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
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

std::wstring DeckWindow::EditorPathSnapshot()
{
	std::lock_guard<std::mutex> lock(mSnapshotMutex);
	return mEditorPathSnapshot;
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
		mEditorPathSnapshot = mEditPath;
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
	mMcpTools.SetEditorPathSupplier([this] { return EditorPathSnapshot(); });
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
		// m094: an exception escaping a thread entry calls std::terminate and
		// takes the whole host down. MCP is an optional endpoint, so a bad line
		// from a client must end the loop, not the app.
		try
		{
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
		}
		catch (...)
		{
			mMcpRunning = false;
		}
		// m117: the client owns the other end of stdin. When it lets go (EOF or
		// a broken frame stream) the deck has no user left to serve: in background
		// mode it quits instead of lingering invisibly. Fl::awake hops back to the
		// main thread - RequestQuit is UI state.
		if (mMcpBackground)
			Fl::awake([](void *aData) {
				static_cast<DeckWindow *>(aData)->RequestQuit();
			}, this);
	});
}

void DeckWindow::StartMcpBackground()
{
	mMcpBackground = true;
	StartMcp();
	if (!mHooks.HasTrayIcon())
	{
		Append("[mcp] 托盘不可用：窗口保持可见（隐藏后无法再唤回界面）");
		return;
	}
	HideToTray();
}

void DeckWindow::StopMcp()
{
	mMcpRunning = false;
	// m083/B3: join, never detach. The reader thread polls mMcpRunning every
	// 20 ms and leaves on its own; the cancel below releases a tool call that
	// is still waiting out its timeout, so the join cannot hang for 20 s.
	// Detaching (the m064 behaviour) meant the window had to be torn down by
	// ExitProcess - the destructor chain never ran.
	mMcpTools.Cancel();
	if (mMcpThread.joinable())
		mMcpThread.join();
}

int Run(HINSTANCE, bool aServeMcpStdio)
{
	// m084: FLTK's own errors must never open a modal dialog (AGENTS rule 3).
	// The channel is installed before anything else, so even an error during
	// the window's construction lands in the console instead of a popup.
	InstallFltkErrorChannel();

	Engine &engine = Engine::Instance();
	DeckWindow *window = new DeckWindow(engine);
	window->show();
	window->InstallFrame();

	// The console is the deck's error column; FLTK's messages join the kernel
	// errors there (m084).
	SetFltkErrorSink([window](const std::string &aMessage) {
		window->ReportLibraryError(aMessage);
	});

	// m064: the MCP client owns this process's stdin/stdout, so the endpoint
	// starts here. m117: with --mcp-stdio the deck then hides to the tray - the
	// agent drives the same editing area, sessions and log, but no window pops
	// up on every call. The tray icon (or the bound 显示主窗口 hotkey) brings it
	// back, and the event loop keeps ticking while hidden.
	if (aServeMcpStdio)
		window->StartMcpBackground();

	// Test/screenshot hook only: the shortcut table is still opened from 设置.
	{
		char openShortcuts[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_SHORTCUTS", openShortcuts, sizeof(openShortcuts)) > 0)
			window->ShowShortcutDialog();
	}

	// m092 screenshot hook only: open 帮助 > 检查更新 without a gesture, so the
	// updater window can be captured (and its layout checked) by a script.
	{
		char openUpdate[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_UPDATE", openUpdate, sizeof(openUpdate)) > 0)
			window->ShowUpdateDialog();
	}

	// m146 screenshot hook only: open 文件 > 获取脚本… without a gesture.
	{
		char openDownload[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_SCRIPT_DOWNLOAD", openDownload,
			sizeof(openDownload)) > 0)
			window->ShowScriptDownloadDialog();
	}

	// m151 screenshot hook only: open the row menu's 信息 window without a
	// gesture. The value is the row index (0 when the variable is just "1").
	{
		char openInfo[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_SCRIPT_INFO", openInfo,
			sizeof(openInfo)) > 0)
			window->ShowScriptInfo(std::atoi(openInfo));
	}

	// m177 screenshot hook only: open 设置 > 选项… without a gesture.
	{
		char openOptions[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_OPTIONS", openOptions,
			sizeof(openOptions)) > 0)
			window->ShowOptions();
	}

	// m178 screenshot hook only: open 帮助 > 关于 without a gesture.
	{
		char openAbout[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_ABOUT", openAbout,
			sizeof(openAbout)) > 0)
			window->ShowAbout();
	}

	// m180 screenshot hook only: open 获取脚本 without a gesture.
	{
		char openDownload[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_DOWNLOAD", openDownload,
			sizeof(openDownload)) > 0)
			window->ShowScriptDownloadDialog();
	}

	// m180 screenshot hook only: open 订阅管理 without a gesture.
	{
		char openSubscribe[8] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_SUBSCRIBE", openSubscribe,
			sizeof(openSubscribe)) > 0)
			window->ShowSubscribeManager();
	}

	// m082 gate hook (test-only): put a Scripts file into the editing area
	// without any gesture, so the MCP gate can watch editor-owned state.
	{
		char openScript[280] = {0};
		if (GetEnvironmentVariableA("AHKDECK_OPEN_SCRIPT", openScript, sizeof(openScript)) > 0)
			window->OpenScriptForGate(std::string(openScript));
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

	// m083/B3: with an MCP endpoint the window used to be freed by ExitProcess,
	// because a detached reader could still wake up and call its sinks. The
	// endpoint now joins its threads (StopMcp above), so the normal teardown
	// applies to the MCP mode too - the settings are saved, the tray icon is
	// removed, and the shadow window is detached, all through the destructors.
	window->StopMcp();

	// The error sink points at the window; it must not outlive it.
	SetFltkErrorSink(nullptr);

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

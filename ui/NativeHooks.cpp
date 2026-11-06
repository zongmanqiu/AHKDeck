#include "ui/NativeHooks.h"

#include "ui/DeckIcon.h"

#include <shellapi.h>

#include <cstdio>

namespace ahkdeck
{
namespace ui
{
namespace
{

// A window class name the deck owns: the CLI finds running decks by it, and the
// kernel's own classes ("AutoHotkey", "AutoHotkeyGUI") are not involved.
constexpr wchar_t kControlClass[] = L"AHKDeckControl";
constexpr wchar_t kQuitMessageName[] = L"AHKDeckQuit";
constexpr wchar_t kShowMessageName[] = L"AHKDeckShow";
constexpr wchar_t kTrayMessageName[] = L"AHKDeckTrayIcon";
constexpr UINT kTrayIconId = 1;

enum TrayCommand
{
	CmdShow = 1,
	CmdQuit = 2
};

// Registered (not WM_APP) so another process can name the same message: WM_APP
// is private to the process that made it up.
UINT QuitMessage()
{
	static UINT message = RegisterWindowMessageW(kQuitMessageName);
	return message;
}

UINT ShowMessage()
{
	static UINT message = RegisterWindowMessageW(kShowMessageName);
	return message;
}

UINT TrayMessage()
{
	static UINT message = RegisterWindowMessageW(kTrayMessageName);
	return message;
}

// Explorer broadcasts TaskbarCreated after it restarts; every top-level window
// receives it. Without handling it the tray icon stays gone until the deck is
// restarted, because Shell_NotifyIcon's registration died with the old taskbar.
UINT TaskbarCreatedMessage()
{
	static UINT message = RegisterWindowMessageW(L"TaskbarCreated");
	return message;
}

std::wstring LastErrorText(DWORD aCode)
{
	wchar_t buffer[160] = {0};
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, aCode, 0,
		buffer, 160, nullptr);
	return buffer;
}

std::string Utf8Of(const std::wstring &aWide)
{
	if (aWide.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring WideOf(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), out.data(), n);
	return out;
}

// One deck per process, so the WndProc can find its owner without a per-window
// index. Create() sets it, Destroy() clears it.
NativeHooks *gHooks = nullptr;

HICON TrayIconHandle()
{
	// Same AHK "H" as the title bar and taskbar: load the resource by id.
	// Index 0 is not guaranteed to be that icon after future resource edits.
	if (HMODULE module = GetModuleHandleW(nullptr))
	{
		if (HICON icon = LoadIconW(module, MAKEINTRESOURCEW(IDI_AHKDECK)))
			return icon;
	}
	wchar_t exePath[MAX_PATH] = {0};
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
	{
		// Not named "small": rpcndr.h (pulled in by shellapi.h) #defines small char.
		HICON iconLarge = nullptr;
		HICON iconSmall = nullptr;
		if (ExtractIconExW(exePath, 0, &iconLarge, &iconSmall, 1) == 1)
		{
			if (iconSmall)
				return iconSmall;
			if (iconLarge)
				return iconLarge;
		}
	}
	// m030: AHKDeck.exe now carries the AutoHotkey icon as its first icon
	// group, so the extract above finds it. The fallback stays for a stripped
	// or resource-less build. MAKEINTRESOURCEW by name: this target does not
	// define UNICODE, so the IDI_APPLICATION macro would hand the ANSI
	// MAKEINTRESOURCE to a W call.
	return LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /*IDI_APPLICATION*/);
}

struct DeckMessageTarget
{
	DWORD processId = 0;
	UINT message = 0;
	int count = 0;
};

BOOL CALLBACK EnumDeckWindows(HWND aHwnd, LPARAM aParam)
{
	wchar_t className[64] = {0};
	if (GetClassNameW(aHwnd, className, 64) == 0 || wcscmp(className, kControlClass) != 0)
		return TRUE;
	DeckMessageTarget *target = reinterpret_cast<DeckMessageTarget *>(aParam);
	if (target->processId != 0)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(aHwnd, &pid);
		if (pid != target->processId)
			return TRUE;
	}
	if (target->message)
		PostMessageW(aHwnd, target->message, 0, 0);
	++target->count;
	return TRUE;
}

} // namespace

bool NativeHooks::Create(HINSTANCE aInstance, std::string &aError)
{
	aError.clear();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = NativeHooks::WndProc;
	wc.hInstance = aInstance;
	wc.lpszClassName = kControlClass;
	ATOM atom = RegisterClassExW(&wc);
	if (!atom)
	{
		const DWORD code = GetLastError();
		// 1410 = ERROR_CLASS_ALREADY_EXISTS: a deck in this process already
		// registered it, which is fine - only one NativeHooks exists per process.
		if (code != 1410)
		{
			aError = "RegisterClassEx 失败：" + Utf8Of(LastErrorText(code));
			return false;
		}
	}
	else
	{
		mClass = atom;
	}

	// Top-level (no parent) and never shown: a HWND_MESSAGE child would keep the
	// hotkeys and the tray working, but PostQuitToRunningDecks() could not find it
	// - message-only windows are not in the EnumWindows chain.
	mWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kControlClass, L"AHKDeck 控制窗口", 0,
		0, 0, 0, 0, nullptr, nullptr, aInstance, nullptr);
	if (!mWindow)
	{
		aError = "创建 deck 控制窗口失败：" + Utf8Of(LastErrorText(GetLastError()));
		if (mClass)
		{
			UnregisterClassW(kControlClass, aInstance);
			mClass = 0;
		}
		return false;
	}

	gHooks = this;
	return true;
}

void NativeHooks::Destroy()
{
	RemoveTrayIcon();
	UnregisterHotkeys();
	if (mWindow)
		DestroyWindow(mWindow);
	mWindow = nullptr;
	if (gHooks == this)
		gHooks = nullptr;
	if (mClass)
	{
		UnregisterClassW(kControlClass, GetModuleHandleW(nullptr));
		mClass = 0;
	}
}

bool NativeHooks::AddTrayIcon(const std::string &aTipUtf8, std::string &aError)
{
	aError.clear();
	if (!mWindow)
	{
		aError = "控制窗口不存在";
		return false;
	}

	NOTIFYICONDATAW data = {};
	data.cbSize = sizeof(data);
	data.hWnd = mWindow;
	data.uID = kTrayIconId;
	data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	data.uCallbackMessage = TrayMessage();
	data.hIcon = TrayIconHandle();
	mTrayTip = WideOf(aTipUtf8);
	lstrcpynW(data.szTip, mTrayTip.c_str(), (int)_countof(data.szTip));

	if (!Shell_NotifyIconW(NIM_ADD, &data))
	{
		aError = "Shell_NotifyIcon NIM_ADD 失败（托盘可能已满）";
		return false;
	}
	mTrayShown = true;
	return true;
}

void NativeHooks::UpdateTrayTip(const std::string &aTipUtf8)
{
	if (!mTrayShown || !mWindow)
		return;
	NOTIFYICONDATAW data = {};
	data.cbSize = sizeof(data);
	data.hWnd = mWindow;
	data.uID = kTrayIconId;
	data.uFlags = NIF_TIP;
	mTrayTip = WideOf(aTipUtf8);
	lstrcpynW(data.szTip, mTrayTip.c_str(), (int)_countof(data.szTip));
	Shell_NotifyIconW(NIM_MODIFY, &data);
}

void NativeHooks::RemoveTrayIcon()
{
	if (!mTrayShown || !mWindow)
		return;
	NOTIFYICONDATAW data = {};
	data.cbSize = sizeof(data);
	data.hWnd = mWindow;
	data.uID = kTrayIconId;
	Shell_NotifyIconW(NIM_DELETE, &data);
	mTrayShown = false;
}

void NativeHooks::ReAddTrayIcon()
{
	if (!mTrayShown || !mWindow)
		return;
	NOTIFYICONDATAW data = {};
	data.cbSize = sizeof(data);
	data.hWnd = mWindow;
	data.uID = kTrayIconId;
	data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	data.uCallbackMessage = TrayMessage();
	data.hIcon = TrayIconHandle();
	lstrcpynW(data.szTip, mTrayTip.c_str(), (int)_countof(data.szTip));
	Shell_NotifyIconW(NIM_ADD, &data);
}

void NativeHooks::EnsureTrayIcon()
{
	if (!mTrayShown || !mWindow)
		return;
	NOTIFYICONDATAW data = {};
	data.cbSize = sizeof(data);
	data.hWnd = mWindow;
	data.uID = kTrayIconId;
	data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	data.uCallbackMessage = TrayMessage();
	data.hIcon = TrayIconHandle();
	lstrcpynW(data.szTip, mTrayTip.c_str(), (int)_countof(data.szTip));
	// NIM_MODIFY fails when Explorer rebuilt the tray and our icon is gone;
	// NIM_ADD is then the same recovery the TaskbarCreated broadcast performs.
	if (!Shell_NotifyIconW(NIM_MODIFY, &data))
		Shell_NotifyIconW(NIM_ADD, &data);
}

bool NativeHooks::RegisterHotkey(int aId, unsigned int aMods, unsigned int aVirtualKey, std::string &aError)
{
	aError.clear();
	if (!RegisterHotKey(mWindow, aId, aMods, aVirtualKey))
	{
		const DWORD code = GetLastError();
		aError = code == 1409 /*ERROR_HOTKEY_ALREADY_REGISTERED*/
			? "该快捷键已被系统或其它程序占用"
			: "RegisterHotKey 失败：" + Utf8Of(LastErrorText(code));
		return false;
	}
	mHotkeyIds.push_back(aId);
	return true;
}

void NativeHooks::UnregisterHotkeys()
{
	for (int id : mHotkeyIds)
		UnregisterHotKey(mWindow, id);
	mHotkeyIds.clear();
}

bool NativeHooks::PopIntent(Intent &aOut)
{
	if (mIntents.empty())
		return false;
	aOut = mIntents.front();
	mIntents.pop_front();
	return true;
}

void NativeHooks::PushIntent(const Intent &aIntent)
{
	mIntents.push_back(aIntent);
}

int NativeHooks::PostQuitToRunningDecks(DWORD aProcessId)
{
	const UINT quitMessage = QuitMessage();
	if (!quitMessage)
		return 0;
	DeckMessageTarget target;
	target.processId = aProcessId;
	target.message = quitMessage;
	EnumWindows(EnumDeckWindows, reinterpret_cast<LPARAM>(&target));
	return target.count;
}

int NativeHooks::PostShowToRunningDecks(DWORD aProcessId)
{
	const UINT showMessage = ShowMessage();
	if (!showMessage)
		return 0;
	DeckMessageTarget target;
	target.processId = aProcessId;
	target.message = showMessage;
	EnumWindows(EnumDeckWindows, reinterpret_cast<LPARAM>(&target));
	return target.count;
}

LRESULT CALLBACK NativeHooks::WndProc(HWND aHwnd, UINT aMessage, WPARAM aWParam, LPARAM aLParam)
{
	if (gHooks)
	{
		const UINT quitMessage = QuitMessage();
		const UINT trayMessage = TrayMessage();
		const UINT taskbarCreated = TaskbarCreatedMessage();

		if (taskbarCreated && aMessage == taskbarCreated)
		{
			gHooks->ReAddTrayIcon();
			return 0;
		}

		if (aMessage == WM_HOTKEY)
		{
			Intent intent;
			intent.kind = Intent::Kind::Hotkey;
			intent.hotkeyId = (int)aWParam;
			gHooks->PushIntent(intent);
			return 0;
		}
		if (quitMessage && aMessage == quitMessage)
		{
			Intent intent;
			intent.kind = Intent::Kind::Quit;
			gHooks->PushIntent(intent);
			return 0;
		}
		const UINT showMessage = ShowMessage();
		if (showMessage && aMessage == showMessage)
		{
			Intent intent;
			intent.kind = Intent::Kind::TrayShow;
			gHooks->PushIntent(intent);
			return 0;
		}
		if (trayMessage && aMessage == trayMessage)
		{
			const UINT mouse = (UINT)aLParam;
			if (mouse == WM_LBUTTONDBLCLK)
			{
				Intent intent;
				intent.kind = Intent::Kind::TrayShow;
				gHooks->PushIntent(intent);
				return 0;
			}
			if (mouse == WM_RBUTTONUP)
			{
				// The menu is native (CreatePopupMenu/TrackPopupMenu), which keeps
				// it consistent with the native title bar the deck kept in m021 and
				// means no FLTK widget is touched from inside this dispatch.
				HMENU menu = CreatePopupMenu();
				if (menu)
				{
					AppendMenuW(menu, MF_STRING, CmdShow, L"主窗口");
					AppendMenuW(menu, MF_STRING, CmdQuit, L"退出");
					POINT where = {};
					GetCursorPos(&where);
					SetForegroundWindow(aHwnd); // so the menu closes when it loses focus
					const LONG picked = TrackPopupMenuEx(menu,
						TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, where.x, where.y, aHwnd, nullptr);
					DestroyMenu(menu);
					if (picked == CmdShow)
					{
						Intent intent;
						intent.kind = Intent::Kind::TrayShow;
						gHooks->PushIntent(intent);
					}
					else if (picked == CmdQuit)
					{
						Intent intent;
						intent.kind = Intent::Kind::Quit;
						gHooks->PushIntent(intent);
					}
				}
				return 0;
			}
		}
	}
	return DefWindowProcW(aHwnd, aMessage, aWParam, aLParam);
}

} // namespace ui
} // namespace ahkdeck

// deck UI - the three Win32 surfaces FLTK does not offer (m023).
//
// The old workbench was itself an AHK script, so it got the tray icon, the
// global hotkeys and "one instance" for free from the interpreter. As a C++
// host the deck must ask Win32 for them, and all three want a window of their
// own: RegisterHotKey delivers WM_HOTKEY to a hwnd, Shell_NotifyIcon delivers
// the tray callback to a hwnd, and "--quit" has to reach a *running* deck from
// another process, which means a window another process can find.
//
// That is the whole file: one hidden top-level window (hidden = never shown, not
// SW_HIDE - a message-only window is not in FindWindow's chain, so the CLI could
// not find it), its WndProc, and the tray icon. Nothing here touches the deck's
// state; every Win32 event becomes an Intent the window drains on its own tick,
// so no FLTK widget is ever mutated from inside a message dispatch.
//
// Direction: ui depends on engine and core; this class depends on Win32 only.

#pragma once

#include <windows.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace ahkdeck
{
namespace ui
{

class NativeHooks
{
public:
	// m131: one entry of the tray menu's 运行中 submenu. The menu shows the UTF-8
	// leaf name (without the ".ahk" suffix, m132); the full path is the identity
	// the deck resolves a fallback pick by.
	struct RunningScript
	{
		std::wstring path;
		std::string name;  // UTF-8 leaf, display only
		// m132/m192 P3: the host-owned HMENU rebuilt from the child's JSON tray
		// snapshot, copied into a real submenu, and the child window the picked
		// command id is posted back to as WM_COMMAND. Both may be null (script
		// gone, or a busy child that could not answer the snapshot request); the
		// entry then falls back to "open that script's menu in a second popup"
		// like m131 did.
		HMENU menu = nullptr;
		HWND window = nullptr;
	};

	struct Intent
	{
		enum class Kind
		{
			Hotkey,   // a registered start/stop hotkey fired
			TrayShow, // the tray icon was clicked: bring the window back
			TrayMenu, // m131: a script was picked in the tray menu's 运行中 submenu
			Quit      // "--quit" arrived from another process
		} kind = Kind::TrayShow;
		int hotkeyId = 0;
		std::wstring scriptPath; // TrayMenu only: the picked script, by full path
	};

	// Creates the control window. On failure aError says which Win32 call gave up.
	bool Create(HINSTANCE aInstance, std::string &aError);
	void Destroy();

	// m131: the deck answers "who is running right now" when the tray menu is
	// about to open. The control window lives on the UI thread, so the provider
	// may read the deck and the engine directly - asked fresh per right-click,
	// no snapshot to go stale, no lock.
	void SetRunningMenuProvider(std::function<std::vector<RunningScript>()> aProvider);

	HWND Window() const { return mWindow; }

	// Tray icon. AddTrayIcon failing is not fatal: the deck then treats the close
	// box as "exit" again, because an unreachable window is worse than no tray.
	bool AddTrayIcon(const std::string &aTipUtf8, std::string &aError);
	void UpdateTrayTip(const std::string &aTipUtf8);
	void RemoveTrayIcon();
	void EnsureTrayIcon(); // watchdog: re-add after an Explorer restart
	bool HasTrayIcon() const { return mTrayShown; }

	// One global hotkey per script. aId is handed back with the Intent.
	bool RegisterHotkey(int aId, unsigned int aMods, unsigned int aVirtualKey, std::string &aError);
	void UnregisterHotkeys();
	size_t HotkeyCount() const { return mHotkeyIds.size(); }

	// Drains one pending event; false when the queue is empty.
	bool PopIntent(Intent &aOut);

	// Tells a running deck to exit (the UI gates use this instead of WM_CLOSE,
	// which now means "hide to tray"). aProcessId=0 keeps the old "every deck"
	// behaviour; the gates pass their own pid so they cannot kill a user's
	// instance. Returns how many windows were signalled.
	static int PostQuitToRunningDecks(DWORD aProcessId = 0);

	// A second bare launch must not become a second deck: it asks the existing
	// instance to come out of the tray (or to the foreground) and leaves.
	static int PostShowToRunningDecks(DWORD aProcessId = 0);

private:
	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	void PushIntent(const Intent &aIntent);
	void ReAddTrayIcon();

	HWND mWindow = nullptr;
	ATOM mClass = 0;
	bool mTrayShown = false;
	std::wstring mTrayTip;
	std::deque<int> mHotkeyIds;
	std::deque<Intent> mIntents;
	std::function<std::vector<RunningScript>()> mRunningMenuProvider;
};

} // namespace ui
} // namespace ahkdeck

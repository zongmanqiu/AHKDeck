// Per-script start/stop hotkeys (m023), the shape AHKDeck v1.3 gave the user.
//
// Two independent pieces live here because only one of them can be unit-tested
// without a window station: the *parser* turns an AHK hotkey string ("^!c") into
// Win32 modifiers + a virtual key, and the *map* persists one string per script
// in the user's data directory. The UI stores the text the user typed, so the
// row label can show the original spelling and the registration is derived on
// demand - storing the parsed form would be a second truth.
//
// Direction: core has no UI and no engine dependency. The table below is the
// only place Win32 virtual-key codes are named; it must stay free of user32
// calls so the logic stays testable (see test/test_hotkeys.cpp).

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ahkdeck
{

// Win32 RegisterHotKey modifier flags (the values are the ABI, not ours).
inline constexpr unsigned int kHotkeyAlt = 0x0001;
inline constexpr unsigned int kHotkeyControl = 0x0002;
inline constexpr unsigned int kHotkeyShift = 0x0004;
inline constexpr unsigned int kHotkeyWin = 0x0008;
inline constexpr unsigned int kHotkeyNoRepeat = 0x4000;

// Parses one AHK hotkey ("^!c", "Ctrl+Alt+F12", "#z"). An empty string is NOT a
// binding: the caller clears the entry before ever calling here, and the parser
// says so, because "clear" and "typo" must not look the same to the user.
// aOk is false when the syntax is wrong; aError then says why and aText always
// gets the canonical display form (upper-case letters, the modifier order we
// store). aMods carries kHotkeyNoRepeat: v1.3 used Hotkey, "On" without the
// Tilde, so a held key must not repeat the toggle.
bool ParseAhkHotkey(const std::string &aText, unsigned int &aMods, unsigned int &aVirtualKey,
	std::string &aCanonical, std::string &aError);

// m088: the same spelling, but for a *menu command* shortcut. Those live inside
// the deck window, so any named key ("Del", "Enter") is safe there, while the
// system-wide scope still needs a modifier - except a bare function key
// (F1-F24): m117 allows that in both scopes, because nobody types an F-key into
// a text field. A bare letter or digit stays rejected in both: it would eat the
// key while typing.
bool ParseMenuShortcut(const std::string &aText, unsigned int &aMods, unsigned int &aVirtualKey,
	std::string &aCanonical, std::string &aError);

struct HotkeyEntry
{
	std::string name;  // script leaf name, UTF-8 (the key the list shows)
	std::string hotkey; // AHK syntax, empty = unbound
};

// hotkeys.txt next to the exe (portable mode, same rule as Settings/Library).
std::wstring DefaultHotkeyPath();

// Missing file = first run: returns true with an empty list. Later duplicates of
// the same script name win, so a hand-edited file behaves like the save path.
bool LoadHotkeys(const std::wstring &aPath, std::vector<HotkeyEntry> &aOut, std::string &aError);

// Creates the parent directory when needed; writes CRLF/UTF-8 without BOM.
bool SaveHotkeys(const std::wstring &aPath, const std::vector<HotkeyEntry> &aEntries, std::string &aError);

// Sets (or clears, on empty hotkey) one entry in place; keeps the file order.
void SetHotkeyEntry(std::vector<HotkeyEntry> &aEntries, const std::string &aName, const std::string &aHotkey);

// The hotkey bound to aName, or "" when unbound.
std::string HotkeyOf(const std::vector<HotkeyEntry> &aEntries, const std::string &aName);

// The script that already claims aHotkey, or "" when free. Case-insensitive on
// the spelling after parsing, so "^!C" and "^!c" count as one binding.
std::string OwnerOfHotkey(const std::vector<HotkeyEntry> &aEntries, const std::string &aName,
	const std::string &aHotkey);

// m090: two actions must never share one combo. Both sides are parsed in the
// menu scope (the wider one, which also accepts a bare function key), so a menu
// default and a script binding compare the same way. An empty side never
// collides.
bool SameShortcut(const std::string &aLeft, const std::string &aRight);

// m090: one combo, one action. aRows is (name, combo) in table order; when two
// rows carry the same combo the later one keeps it and every earlier one is
// cleared. A hand-edited file is normalized by the save path, not rejected.
void MakeShortcutsUnique(std::vector<std::pair<std::string, std::string>> &aRows);

// m106: what a hand-edited hotkeys.txt can contain that the dialog prevents.
// One issue per entry: the text does not parse, or it equals an earlier entry.
// The loader reports these to the console; it never rewrites the file silently
// (m090: a hand-edited file is normalized by the save path, not by the loader).
struct HotkeyIssue
{
	std::string name;
	std::string hotkey;
	std::string message; // already user-facing: names the reason and the owner
};
std::vector<HotkeyIssue> AuditHotkeys(const std::vector<HotkeyEntry> &aEntries);

} // namespace ahkdeck

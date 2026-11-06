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

struct HotkeyEntry
{
	std::string name;  // script leaf name, UTF-8 (the key the list shows)
	std::string hotkey; // AHK syntax, empty = unbound
};

// %LOCALAPPDATA%\AHKDeck\hotkeys.txt, or empty when the variable is missing.
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

} // namespace ahkdeck

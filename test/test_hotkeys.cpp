// m023: the hotkey layer has two jobs the UI must not redo - turn an AHK string
// into Win32 modifiers + a virtual key, and keep one binding per script across
// runs. Both are pure logic, so both are pinned here (cwd = .temp/hotkey_gate).
//
// The parser fixtures are written against the shapes that broke while writing
// it: "^!c" without any '+' is the spelling v1.3 users already have in their
// AHKDeck.ini, so a splitter that only knows '+' would reject it.

#include "core/Hotkeys.h"
#include "core/TextFile.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int gFailures = 0;

void Check(bool aCondition, const char *aWhat)
{
	if (aCondition)
		std::printf("ok   %s\n", aWhat);
	else
	{
		std::printf("FAIL %s\n", aWhat);
		++gFailures;
	}
}

struct Parsed
{
	bool ok = false;
	unsigned int mods = 0;
	unsigned int key = 0;
	std::string canonical;
	std::string error;
};

Parsed Parse(const char *aText)
{
	Parsed out;
	out.ok = ahkdeck::ParseAhkHotkey(aText, out.mods, out.key, out.canonical, out.error);
	return out;
}

void CheckError(const Parsed &aParsed, const char *aNeedle, const char *aWhat)
{
	// Assert the branch, not a substring of the whole screen: the message must
	// name the reason the user has to act on.
	Check(!aParsed.ok && aParsed.error.find(aNeedle) != std::string::npos, aWhat);
}

} // namespace

int main()
{
	// --- the modifier prefixes, in the two spellings users type ---------------
	{
		const Parsed ctrlAlt = Parse("^!c");
		Check(ctrlAlt.ok, "^!c parses (no '+' anywhere)");
		Check(ctrlAlt.mods == (ahkdeck::kHotkeyControl | ahkdeck::kHotkeyAlt | ahkdeck::kHotkeyNoRepeat),
			"^!c -> CONTROL|ALT|NOREPEAT");
		Check(ctrlAlt.key == 'C', "^!c -> VK 0x43 for the letter, upper-cased");
		Check(ctrlAlt.canonical == "^!C", "^!c canonicalises to ^!C");

		const Parsed spelled = Parse("ctrl + alt + F12");
		Check(spelled.ok, "ctrl + alt + F12 parses with spaces and names");
		Check(spelled.key == 0x7B, "F12 -> VK_F12 (0x7B)");
		Check(spelled.canonical == "^!F12", "spelled form canonicalises to ^!F12");

		const Parsed win = Parse("#z");
		Check(win.ok && win.mods == (ahkdeck::kHotkeyWin | ahkdeck::kHotkeyNoRepeat) && win.key == 'Z',
			"#z -> WIN + Z");

		const Parsed shift = Parse("^+b");
		Check(shift.ok && shift.key == 'B' && shift.canonical == "^+B", "^+b -> CONTROL|SHIFT + B");

		// '+' is both the separator and the Shift symbol; "=" is a real key.
		const Parsed plusEquals = Parse("^+=");
		Check(plusEquals.ok && plusEquals.key == 0xBB /*VK_OEM_PLUS*/, "^+= -> Shift is a modifier, = is the key");
		Check(plusEquals.canonical == "^+=", "^+= keeps its punctuation in the display form");
	}

	// --- named keys keep the kernel's own spelling ----------------------------
	{
		const Parsed enter = Parse("^Enter");
		Check(enter.ok && enter.key == 0x0D && enter.canonical == "^Enter", "^Enter -> VK_RETURN, canonical ^Enter");

		const Parsed pgdn = Parse("^+pgdn");
		Check(pgdn.ok && pgdn.key == 0x22 && pgdn.canonical == "^+PgDn", "pgdn is case-insensitive, canonical is PgDn");

		const Parsed apps = Parse("!AppsKey");
		Check(apps.ok && apps.key == 0x5D, "!AppsKey -> VK_APPS");

		const Parsed unknown = Parse("^Return");
		CheckError(unknown, "无法识别", "Return is not an alias for Enter (the kernel spells it Enter)");
	}

	// --- rejections name the branch, so the console line is actionable -------
	{
		CheckError(Parse("a"), "修饰键", "a bare letter needs a modifier");
		CheckError(Parse(""), "为空", "an empty string is not a binding");
		CheckError(Parse("^+"), "主键", "a trailing modifier has no key");
		CheckError(Parse("~^c"), "~", "the ~ pass-through prefix is refused, not silently dropped");
		CheckError(Parse("^^"), "主键", "two carets are two Ctrl symbols, so no key is left");
		CheckError(Parse("^%"), "无法识别", "a symbol that is not a key names the key it cannot find");
		CheckError(Parse("Enter"), "修饰键", "Enter without a modifier is refused");
		// "Win" without a following '+' is a key press, so it must not be eaten
		// as a prefix and then reported as a missing key.
		const Parsed bareWin = Parse("Win");
		Check(!bareWin.ok && bareWin.error.find("修饰键") != std::string::npos, "bare Win is a key, not a prefix");
		const Parsed winPlus = Parse("Win+F2");
		Check(winPlus.ok && winPlus.key == 0x71, "Win+F2 treats Win as the modifier");
	}

	// --- the stored map ------------------------------------------------------
	const std::wstring dir = L"hotkey_gate";
	CreateDirectoryW(dir.c_str(), nullptr);
	const std::wstring path = dir + L"\\hotkeys.txt";

	{
		std::vector<ahkdeck::HotkeyEntry> entries;
		ahkdeck::SetHotkeyEntry(entries, "mouse-double.ahk", "^!d");
		ahkdeck::SetHotkeyEntry(entries, "心跳.ahk", "^!h");
		ahkdeck::SetHotkeyEntry(entries, "mouse-double.ahk", "^!m"); // rebind, not duplicate
		ahkdeck::SetHotkeyEntry(entries, "示例-报错.ahk", "");       // clear

		std::string error;
		Check(ahkdeck::SaveHotkeys(path, entries, error), "save hotkeys");
		(void)error.clear();

		std::vector<ahkdeck::HotkeyEntry> loaded;
		Check(ahkdeck::LoadHotkeys(path, loaded, error), "load hotkeys");
		Check(loaded.size() == 2, "the cleared binding and the rebind collapse to 2 rows");
		Check(ahkdeck::HotkeyOf(loaded, "mouse-double.ahk") == "^!m", "the rebind is what came back");
		Check(ahkdeck::HotkeyOf(loaded, "示例-报错.ahk").empty(), "an unbound script has no hotkey");

		// Conflict lookup is by parsed key + modifiers, not by spelling.
		Check(ahkdeck::OwnerOfHotkey(loaded, "other.ahk", "^!M") == "mouse-double.ahk",
			"^!M collides with ^!m regardless of case");
		Check(ahkdeck::OwnerOfHotkey(loaded, "mouse-double.ahk", "^!m").empty(),
			"a script changing its own binding does not collide with itself");
		Check(ahkdeck::OwnerOfHotkey(loaded, "other.ahk", "^!d").empty(), "a free hotkey reports no owner");
		Check(ahkdeck::OwnerOfHotkey(loaded, "other.ahk", "^m") != "mouse-double.ahk", "^m (no Alt) is not ^!m");
	}

	// A name containing '=' could never be read back: say so, do not write a row
	// that silently loses a binding.
	{
		std::vector<ahkdeck::HotkeyEntry> entries;
		entries.push_back({"a=b.ahk", "^!x"});
		std::string error;
		Check(!ahkdeck::SaveHotkeys(dir + L"\\eq.txt", entries, error), "save refuses a name containing =");
		Check(error.find("a=b.ahk") != std::string::npos, "the error names the script");
	}

	// Hand-edited file: last row for a name wins, junk lines are skipped.
	{
		std::string error;
		Check(ahkdeck::WriteTextDocument(dir + L"\\hand.txt",
			"one.ahk=^!a\nnot a binding line\n\none.ahk=^!b\ntwo.ahk=#t\n",
			ahkdeck::TextEncoding::Utf8NoBom, true, nullptr, error), "write hand-edited hotkeys");
		std::vector<ahkdeck::HotkeyEntry> loaded;
		Check(ahkdeck::LoadHotkeys(dir + L"\\hand.txt", loaded, error), "load hand-edited hotkeys");
		Check(loaded.size() == 2, "the malformed line and the blank line were skipped");
		Check(ahkdeck::HotkeyOf(loaded, "one.ahk") == "^!b", "the last row for a name wins");
	}

	// A missing file is the first run, not an error.
	{
		std::vector<ahkdeck::HotkeyEntry> loaded;
		loaded.push_back({"sentinel", "^!s"});
		std::string error;
		Check(ahkdeck::LoadHotkeys(dir + L"\\missing.txt", loaded, error), "missing file is not an error");
		Check(loaded.empty(), "missing file yields an empty map");
	}

	// Default path: the override wins so the UI gate never touches real bindings.
	{
		SetEnvironmentVariableW(L"AHKDECK_HOTKEYS", L"Z:\\explicit\\hotkeys.txt");
		Check(ahkdeck::DefaultHotkeyPath() == L"Z:\\explicit\\hotkeys.txt", "AHKDECK_HOTKEYS overrides the default path");
		SetEnvironmentVariableW(L"AHKDECK_HOTKEYS", nullptr);

		std::wstring fallback = ahkdeck::DefaultHotkeyPath();
		wchar_t base[MAX_PATH] = {0};
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n > 0 && n < MAX_PATH)
			Check(fallback.find(L"hotkeys.txt") != std::wstring::npos, "default path is hotkeys.txt under AHKDeck");
		else
			Check(fallback.empty(), "default path is empty without LOCALAPPDATA");
	}

	DeleteFileW(path.c_str());
	DeleteFileW((dir + L"\\hand.txt").c_str());

	if (gFailures)
	{
		std::printf("hotkeys: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("hotkeys: all checks passed\n");
	return 0;
}

#include "core/Hotkeys.h"

#include "core/TextFile.h"

#include <windows.h>

#include <cstring>

namespace ahkdeck
{
namespace
{

// The virtual keys Win32 names identically to their ASCII character: letters and
// digits. Everything else needs the table below.
unsigned int AsciiVirtualKey(char aChar)
{
	if (aChar >= 'a' && aChar <= 'z')
		return (unsigned int)(aChar - 'a' + 'A');
	if (aChar >= 'A' && aChar <= 'Z')
		return (unsigned int)aChar;
	if (aChar >= '0' && aChar <= '9')
		return (unsigned int)aChar;
	return 0;
}

struct NamedKey
{
	const char *name;
	unsigned int virtualKey;
};

// Only the keys that RegisterHotKey can actually deliver, spelled the way the
// kernel spells them (.3rd\AutoHotkey_H-v2.0.25\source\globaldata.cpp g_key_to_vk).
// Numpad and mouse/joystick names are deliberately absent: they need scan codes.
const NamedKey kNamedKeys[] = {
	{"Escape", 0x1B}, {"Esc", 0x1B}, {"Tab", 0x09}, {"Space", 0x20},
	{"Backspace", 0x08}, {"BS", 0x08}, {"Enter", 0x0D},
	{"Up", 0x26}, {"Down", 0x28}, {"Left", 0x25}, {"Right", 0x27},
	{"Home", 0x24}, {"End", 0x23}, {"PgUp", 0x21}, {"PgDn", 0x22},
	{"Insert", 0x2D}, {"Ins", 0x2D}, {"Delete", 0x2E}, {"Del", 0x2E},
	{"PrintScreen", 0x2C}, {"Pause", 0x13}, {"ScrollLock", 0x91},
	{"Numlock", 0x90}, {"CapsLock", 0x14}, {"AppsKey", 0x5D},
	{"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73},
	{"F5", 0x74}, {"F6", 0x75}, {"F7", 0x76}, {"F8", 0x77},
	{"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B},
	{"F13", 0x7C}, {"F14", 0x7D}, {"F15", 0x7E}, {"F16", 0x7F},
	{"F17", 0x80}, {"F18", 0x81}, {"F19", 0x82}, {"F20", 0x83},
	{"F21", 0x84}, {"F22", 0x85}, {"F23", 0x86}, {"F24", 0x87},
};

// The OEM row, US layout (VK_OEM_*). Non-US layouts map the same virtual key to
// another character, which is why the spelling here is the key's *position*.
unsigned int PunctuationVirtualKey(char aChar)
{
	switch (aChar)
	{
	case ',': return 0xBC; // VK_OEM_COMMA
	case '.': return 0xBE; // VK_OEM_PERIOD
	case '/': return 0xBF; // VK_OEM_2
	case ';': return 0xBA; // VK_OEM_1
	case '\'': return 0xDE; // VK_OEM_7
	case '[': return 0xDB; // VK_OEM_4
	case ']': return 0xDD; // VK_OEM_6
	case '\\': return 0xDC; // VK_OEM_5
	case '-': return 0xBD; // VK_OEM_MINUS
	case '=': return 0xBB; // VK_OEM_PLUS
	case '`': return 0xC0; // VK_OEM_3
	default: return 0;
	}
}

char LowerAscii(char aChar)
{
	return (aChar >= 'A' && aChar <= 'Z') ? (char)(aChar - 'A' + 'a') : aChar;
}

bool EqualsIgnoreCase(const std::string &aLeft, const char *aRight)
{
	if (aLeft.size() != std::strlen(aRight))
		return false;
	for (size_t i = 0; i < aLeft.size(); ++i)
	{
		if (LowerAscii(aLeft[i]) != LowerAscii(aRight[i]))
			return false;
	}
	return true;
}

// The four AHK modifier symbols as Win32 modifier flags.
unsigned int SymbolModifier(const std::string &aText, size_t aAt)
{
	if (aAt >= aText.size())
		return 0;
	switch (aText[aAt])
	{
	case '^': return kHotkeyControl;
	case '!': return kHotkeyAlt;
	case '+': return kHotkeyShift;
	case '#': return kHotkeyWin;
	default: return 0;
	}
}

// Where the '+' that follows a spelled-out modifier name sits, or npos when aAt
// does not start one. Requiring the '+' is what keeps "Enter" a key and
// "Control+Enter" a binding; the spaces are allowed because the AHK Hotkey
// control in v1.3 produced "Ctrl + Alt + C" verbatim.
size_t ModifierNameSeparator(const std::string &aText, size_t aAt)
{
	static const char *const kNames[] = {"Ctrl", "Control", "Alt", "Shift", "Win", "LWin", "RWin"};
	if (aAt >= aText.size())
		return std::string::npos;

	const size_t remaining = aText.size() - aAt;
	for (const char *name : kNames)
	{
		const size_t length = std::strlen(name);
		if (length > remaining)
			continue;
		if (!EqualsIgnoreCase(aText.substr(aAt, length), name))
			continue;

		size_t after = aAt + length;
		while (after < aText.size() && (aText[after] == ' ' || aText[after] == '\t'))
			++after;
		if (after >= aText.size() || aText[after] != '+')
			continue; // a bare "Win" is a key press, not a prefix
		return after;
	}
	return std::string::npos;
}

std::string TrimText(const std::string &aText)
{
	size_t begin = 0;
	size_t end = aText.size();
	while (begin < end && (aText[begin] == ' ' || aText[begin] == '\t'))
		++begin;
	while (end > begin && (aText[end - 1] == ' ' || aText[end - 1] == '\t' || aText[end - 1] == '\r'))
		--end;
	return aText.substr(begin, end - begin);
}

// "Ctrl"/"Control" and the arrow names are stored as their own spelling; a single
// letter or digit is upper-cased so "^c" and "^C" display the same.
std::string CanonicalKeyToken(const std::string &aToken, unsigned int aVirtualKey)
{
	const unsigned int ascii = AsciiVirtualKey(aToken.empty() ? '\0' : aToken[0]);
	if (ascii == aVirtualKey && aToken.size() == 1)
		return std::string(1, (char)aVirtualKey);
	if (PunctuationVirtualKey(aToken[0]) == aVirtualKey && aToken.size() == 1)
		return aToken;
	std::string out;
	for (const NamedKey &key : kNamedKeys)
	{
		if (key.virtualKey == aVirtualKey && EqualsIgnoreCase(aToken, key.name))
			return key.name;
	}
	return aToken;
}

} // namespace

// m088: two scopes, one spelling. The system scope (a script's global hotkey)
// needs a modifier, because RegisterHotKey would eat the key everywhere in the
// session. The menu scope (a command inside the deck window) may use a bare
// function key, because there the key only reaches our own window.
enum class HotkeyScope
{
	System,
	Menu
};

bool ParseHotkeyText(const std::string &aText, HotkeyScope aScope, unsigned int &aMods,
	unsigned int &aVirtualKey, std::string &aCanonical, std::string &aError)
{
	aMods = 0;
	aVirtualKey = 0;
	aCanonical.clear();
	aError.clear();

	const std::string text = TrimText(aText);
	if (text.empty())
	{
		aError = "快捷键为空（留即在清单里显示为「未绑定」）";
		return false;
	}
	if (text[0] == '~')
	{
		// AHK's "~" prefix means "also let the key through". RegisterHotKey has no
		// such mode: the deck would swallow the key system-wide. Say so.
		aError = "不支持 ~ 前缀（宿主的热键会吞掉该键，不放行）";
		return false;
	}

	unsigned int mods = 0;

	// The kernel's own rule (script2.cpp Hotkey parsing): modifier prefixes are
	// consumed from the left - one symbol at a time, or a whole name that is
	// immediately followed by '+' - and everything that remains is the key. So
	// "^!c", "^+b", "Ctrl+Alt+F12" and "^+=" all parse, while "a+b" is rejected
	// because "a" is a key, not a modifier.
	size_t at = 0;
	for (;;)
	{
		while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
			++at;
		for (;;)
		{
			const unsigned int symbolMod = SymbolModifier(text, at);
			if (!symbolMod)
				break;
			mods |= symbolMod;
			++at;
			while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
				++at;
		}

		const size_t separator = ModifierNameSeparator(text, at);
		if (separator == std::string::npos)
			break;

		const std::string name = TrimText(text.substr(at, separator - at));
		if (EqualsIgnoreCase(name, "Ctrl") || EqualsIgnoreCase(name, "Control"))
			mods |= kHotkeyControl;
		else if (EqualsIgnoreCase(name, "Alt"))
			mods |= kHotkeyAlt;
		else if (EqualsIgnoreCase(name, "Shift"))
			mods |= kHotkeyShift;
		else // Win / LWin / RWin
			mods |= kHotkeyWin;
		at = separator + 1; // past the '+'
	}

	const std::string key = TrimText(text.substr(at));

	if (key.empty())
	{
		aError = "缺少主键（例：^!d、F6、#+Enter）";
		return false;
	}
	if (mods == 0 && aScope == HotkeyScope::System)
	{
		// v1.3 registered the same bindings as a hotkey string; AHK could get away
		// with a bare letter only through its own keyboard hook. RegisterHotKey with
		// no modifier would eat that letter everywhere in the session.
		aError = "至少要一个修饰键（^ Ctrl、! Alt、+ Shift、# Win）";
		return false;
	}

	unsigned int virtualKey = 0;
	if (key.size() == 1)
	{
		virtualKey = AsciiVirtualKey(key[0]);
		if (!virtualKey)
			virtualKey = PunctuationVirtualKey(key[0]);
		if (!virtualKey)
		{
			aError = "无法识别的键「" + key + "」";
			return false;
		}
	}
	else
	{
		for (const NamedKey &named : kNamedKeys)
		{
			if (EqualsIgnoreCase(key, named.name))
			{
				virtualKey = named.virtualKey;
				break;
			}
		}
		if (!virtualKey)
		{
			aError = "无法识别的键「" + key + "」（支持字母、数字、F1-F24、"
				"Enter/Esc/Space/Tab/PgUp/PgDn/Home/End/Ins/Del/方向键 与 , . / ; ' [ ] \\ - = `）";
			return false;
		}
	}

	if (aScope == HotkeyScope::Menu && mods == 0 && key.size() == 1)
	{
		// A bare letter or digit in the menu scope would still eat the key while
		// the user types in the editor. Only the named keys (F3, Del, ...) may
		// stand on their own.
		aError = "至少要一个修饰键（^ Ctrl、! Alt、+ Shift、# Win），或使用功能键（F1-F24、Del 等）";
		return false;
	}

	aMods = mods | (aScope == HotkeyScope::System ? kHotkeyNoRepeat : 0);
	aVirtualKey = virtualKey;
	aCanonical = (mods & kHotkeyControl) ? "^" : "";
	if (mods & kHotkeyAlt)
		aCanonical += "!";
	if (mods & kHotkeyShift)
		aCanonical += "+";
	if (mods & kHotkeyWin)
		aCanonical += "#";
	aCanonical += CanonicalKeyToken(key, virtualKey);
	return true;
}

bool ParseAhkHotkey(const std::string &aText, unsigned int &aMods, unsigned int &aVirtualKey,
	std::string &aCanonical, std::string &aError)
{
	return ParseHotkeyText(aText, HotkeyScope::System, aMods, aVirtualKey, aCanonical, aError);
}

bool ParseMenuShortcut(const std::string &aText, unsigned int &aMods, unsigned int &aVirtualKey,
	std::string &aCanonical, std::string &aError)
{
	return ParseHotkeyText(aText, HotkeyScope::Menu, aMods, aVirtualKey, aCanonical, aError);
}

std::wstring DefaultHotkeyPath()
{
	// The UI gate must not touch the signed-in user's real bindings.
	wchar_t explicitPath[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"AHKDECK_HOTKEYS", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;

	wchar_t base[MAX_PATH] = {0};
	n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::wstring();
	return std::wstring(base) + L"\\AHKDeck\\hotkeys.txt";
}

bool LoadHotkeys(const std::wstring &aPath, std::vector<HotkeyEntry> &aOut, std::string &aError)
{
	aOut.clear();
	if (GetFileAttributesW(aPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		return true; // first run

	TextDocument document;
	if (!ReadTextDocument(aPath, document, aError))
		return false;

	size_t begin = 0;
	while (begin <= document.utf8.size())
	{
		size_t end = document.utf8.find('\n', begin);
		if (end == std::string::npos)
			end = document.utf8.size();

		const std::string line = TrimText(document.utf8.substr(begin, end - begin));
		const size_t equals = line.find('=');
		if (!line.empty() && equals != std::string::npos)
		{
			HotkeyEntry entry;
			entry.name = line.substr(0, equals);
			entry.hotkey = TrimText(line.substr(equals + 1));
			// A hand-edited file may repeat a name: last one wins, like Save does.
			bool replaced = false;
			for (auto &existing : aOut)
			{
				if (existing.name == entry.name)
				{
					existing.hotkey = entry.hotkey;
					replaced = true;
					break;
				}
			}
			if (!replaced)
				aOut.push_back(std::move(entry));
		}

		if (end == document.utf8.size())
			break;
		begin = end + 1;
	}
	return true;
}

bool SaveHotkeys(const std::wstring &aPath, const std::vector<HotkeyEntry> &aEntries, std::string &aError)
{
	std::string content;
	for (const auto &entry : aEntries)
	{
		if (entry.name.empty())
			continue;
		// m090: emptying a menu command has to survive the round trip. Dropping
		// every empty entry would silently give the command its built-in key
		// back, because a missing entry means "use the default". A menu.* row
		// therefore keeps its empty value ("menu.save="); a script row drops its
		// line, since a script has no default to fall back to.
		if (entry.hotkey.empty() && entry.name.compare(0, 5, "menu.") != 0)
			continue;
		// The file format splits on the first '=', so a name containing one could
		// never be read back. Fail with the name instead of writing a lost binding.
		if (entry.name.find('=') != std::string::npos)
		{
			aError = "脚本名里有 = ，无法保存它的快捷键：" + entry.name;
			return false;
		}
		content += entry.name;
		content += "=";
		content += entry.hotkey;
		content += "\r\n";
	}

	size_t slash = aPath.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
	{
		std::wstring directory = aPath.substr(0, slash);
		CreateDirectoryW(directory.c_str(), nullptr);
	}
	return WriteTextDocument(aPath, content, TextEncoding::Utf8NoBom, true, nullptr, aError);
}

void SetHotkeyEntry(std::vector<HotkeyEntry> &aEntries, const std::string &aName, const std::string &aHotkey)
{
	for (auto &entry : aEntries)
	{
		if (entry.name == aName)
		{
			entry.hotkey = aHotkey;
			return;
		}
	}
	aEntries.push_back(HotkeyEntry{aName, aHotkey});
}

std::string HotkeyOf(const std::vector<HotkeyEntry> &aEntries, const std::string &aName)
{
	for (const auto &entry : aEntries)
	{
		if (entry.name == aName)
			return entry.hotkey;
	}
	return std::string();
}

std::string OwnerOfHotkey(const std::vector<HotkeyEntry> &aEntries, const std::string &aName,
	const std::string &aHotkey)
{
	unsigned int mods = 0;
	unsigned int virtualKey = 0;
	std::string canonical;
	std::string error;
	if (!ParseAhkHotkey(aHotkey, mods, virtualKey, canonical, error))
		return std::string();

	for (const auto &entry : aEntries)
	{
		if (entry.name == aName || entry.hotkey.empty())
			continue;
		unsigned int otherMods = 0;
		unsigned int otherKey = 0;
		std::string otherCanonical;
		std::string otherError;
		if (!ParseAhkHotkey(entry.hotkey, otherMods, otherKey, otherCanonical, otherError))
			continue;
		if (otherKey == virtualKey && (otherMods & 0x00FF) == (mods & 0x00FF))
			return entry.name;
	}
	return std::string();
}

bool SameShortcut(const std::string &aLeft, const std::string &aRight)
{
	if (aLeft.empty() || aRight.empty())
		return false;
	unsigned int modsA = 0, keyA = 0, modsB = 0, keyB = 0;
	std::string canonicalA, canonicalB, error;
	if (!ParseMenuShortcut(aLeft, modsA, keyA, canonicalA, error) ||
		!ParseMenuShortcut(aRight, modsB, keyB, canonicalB, error))
		return false;
	return keyA == keyB && (modsA & 0x00FF) == (modsB & 0x00FF);
}

void MakeShortcutsUnique(std::vector<std::pair<std::string, std::string>> &aRows)
{
	for (size_t i = 0; i + 1 < aRows.size(); ++i)
	{
		if (aRows[i].second.empty())
			continue;
		for (size_t j = i + 1; j < aRows.size(); ++j)
		{
			if (!SameShortcut(aRows[i].second, aRows[j].second))
				continue;
			aRows[i].second.clear();
			break;
		}
	}
}

} // namespace ahkdeck

// HotkeyCapture.h - turn the current FLTK key event into AHK hotkey syntax.
//
// Shared by the inline cell in the script list and by the Settings > Shortcuts
// dialog, so both record exactly the same spelling (^!+# order, F1..F12,
// named keys, upper-case letters).
#pragma once

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <string>

namespace ahkdeck
{
namespace ui
{

inline std::string HotkeyModifierPrefix()
{
	std::string prefix;
	const int state = Fl::event_state();
	if (state & FL_CTRL)
		prefix += '^';
	if (state & FL_ALT)
		prefix += '!';
	if (state & FL_SHIFT)
		prefix += '+';
	if (state & FL_META)
		prefix += '#';
	return prefix;
}

inline std::string HotkeyKeyToken(int key)
{
	if (key >= FL_F + 1 && key <= FL_F + 12)
		return "F" + std::to_string(key - FL_F);
	switch (key)
	{
	case FL_Enter: case FL_KP_Enter: return "Enter";
	case FL_Tab: return "Tab";
	case FL_BackSpace: return "Backspace";
	case FL_Delete: return "Delete";
	case FL_Insert: return "Insert";
	case FL_Home: return "Home";
	case FL_End: return "End";
	case FL_Page_Up: return "PgUp";
	case FL_Page_Down: return "PgDn";
	case FL_Left: return "Left";
	case FL_Right: return "Right";
	case FL_Up: return "Up";
	case FL_Down: return "Down";
	case ' ': return "Space";
	default: break;
	}
	if (key >= 0x20 && key < 0x7F)
	{
		char c = (char)key;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		return std::string(1, c);
	}
	return std::string();
}

// Pecia's display form: "Ctrl+Alt+Shift+F7", and "（无）" for an empty
// shortcut. The stored form stays AHK syntax ("^!+F7"); only the visible
// text is translated.
inline std::string HotkeyDisplayText(const std::string &aAhk)
{
	if (aAhk.empty())
		return "（无）";
	std::string out;
	size_t i = 0;
	while (i < aAhk.size())
	{
		const char c = aAhk[i];
		const char *name = nullptr;
		if (c == '^')
			name = "Ctrl";
		else if (c == '!')
			name = "Alt";
		else if (c == '+')
			name = "Shift";
		else if (c == '#')
			name = "Win";
		else
			break;
		if (!out.empty())
			out += '+';
		out += name;
		++i;
	}
	if (i < aAhk.size())
	{
		if (!out.empty())
			out += '+';
		std::string key = aAhk.substr(i);
		// m091: a single letter is always shown upper-case. The stored text can
		// be lower-case ("^!m" from a hand-edited hotkeys.txt); the dialog, the
		// list cell and the menu label must not inherit that spelling.
		if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
			key[0] = (char)(key[0] - 'a' + 'A');
		out += key;
	}
	return out.empty() ? std::string("（无）") : out;
}

// Keep the full Pecia spelling visible inside the narrow 130px cell: with a
// 16px face "Ctrl+Alt+Shift+F9" is wider than the cell, and clipping it at
// "Ctrl+Alt+Shift+" reads as a bug. Drop a point at a time until it fits.
inline int HotkeyTextSize(const std::string &aText, int aMaxWidth)
{
	int size = 16;
	for (; size > 11; --size)
	{
		fl_font(FL_HELVETICA, size);
		int width = 0;
		int height = 0;
		fl_measure(aText.c_str(), width, height, 0);
		if (width <= aMaxWidth)
			break;
	}
	return size;
}

// The text shown while recording: same Pecia spelling, built from the live
// key event instead of an AHK string.
inline std::string HotkeyRecordText(int key)
{
	std::string text;
	const int state = Fl::event_state();
	if (state & FL_CTRL)
		text += "Ctrl+";
	if (state & FL_ALT)
		text += "Alt+";
	if (state & FL_SHIFT)
		text += "Shift+";
	if (state & FL_META)
		text += "Win+";
	text += HotkeyKeyToken(key);
	return text;
}

} // namespace ui
} // namespace ahkdeck

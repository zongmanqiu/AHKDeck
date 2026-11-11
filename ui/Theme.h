#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Text_Display.H>


// FLTK patch hooks (patches/fltk-1.4.5): one definition per process.
extern Fl_Color g_popupCheckColor;
extern Fl_Color g_borderColour;

namespace ahkdeck
{
namespace ui
{

struct ThemeColors
{
	Fl_Color text1;
	Fl_Color text2;
	Fl_Color background1;
	Fl_Color background2;
	Fl_Color background3;
	Fl_Color highlight1;
	Fl_Color highlight2;
	Fl_Color highlight3;
};

const ThemeColors &DeckColors();
Fl_Color ThemeBorderLine();
// m033: the menu bar sits between the title bar (background2) and the
// content bars (background3), so it reads as its own strip.
Fl_Color MenuBarBackground();

// m071: the deck's text and row sizes are one fact each, spelled once. A row is
// 26px; a dialog stacks rows with a 2px gap, so its step is 28.
constexpr int kTextSize = 16;
constexpr int kSubTextSize = 14;
constexpr int kRowStep = 28;
constexpr int TextSize() { return kTextSize; }
constexpr int SubTextSize() { return kSubTextSize; }
constexpr int RowStep() { return kRowStep; }

// m071: semantic colours outside the deck's eight-key palette. They used to be
// literals in the widget that happened to need them.
Fl_Color AlertRed();     // the one "failed / stopped" red
Fl_Color RunningGreen(); // the one "running" green
Fl_Color CloseHoverRed(); // the title bar's close-button hover

// m146: the script downloader's three action tones. A widget that invented
// its own blue/orange would be a second source for the same decision, so they
// live with the rest of the semantic colours.
Fl_Color DownloadBlue();
Fl_Color UpgradeAmber();
Fl_Color IdleGrey();

// The 1px outer ring of every deck window, main or dialog. It lives here
// because the ring is one element; a window that picked its own border tone is
// the difference between two chrome looks.
Fl_Color FrameBorderColour();

// Call once, before the first window/widget is created.
void ApplyDeckTheme();

void StyleButton(Fl_Button *aButton);
void StyleInput(Fl_Input *aInput);
void StyleTextDisplay(Fl_Text_Display *aDisplay);

// The deck's flat two-rectangle scrollbar: track + thumb, no FLTK grooves or
// arrows. The geometry must match Fl_Slider's hit test exactly, otherwise
// the thumb is drawn where the mouse cannot grab it.
void DeckDrawScrollbar(Fl_Scrollbar *aBar, bool aHorizontal);
void DeckDrawScrollbarCorner(Fl_Scrollbar *aVertical, Fl_Scrollbar *aHorizontal);

} // namespace ui
} // namespace ahkdeck

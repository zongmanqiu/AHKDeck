// ui/SourceChoice.cpp - see ui/SourceChoice.h (m147, m148).
#include "ui/SourceChoice.h"

#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"

namespace ahkdeck
{
namespace ui
{

Fl_Choice *MakeSourceChoice(int aX, int aY, int aW, int aH, int aSource,
	Fl_Callback *aCallback, void *aData)
{
	// m148: pecia's SettingsChoice, not the stock Fl_Choice. Its draw() owns
	// the flat box, the divider and the arrow; these six values are the exact
	// construction pecia uses in ui/UpdateDialog.cpp:94 (down_box included -
	// without it FLTK's scheme paints a raised grey button).
	Fl_Choice *choice = new SettingsChoice(aX, aY, aW, aH);
	const ThemeColors &colors = DeckColors();
	choice->box(FL_BORDER_BOX);
	choice->down_box(FL_BORDER_BOX);
	choice->color(colors.background1);
	choice->selection_color(colors.highlight2);
	choice->textcolor(colors.text1);
	choice->labelcolor(colors.text1); // SettingsChoice draws the arrow with it
	choice->textsize(TextSize());
	choice->labelsize(TextSize());

	// The item list, their order and the default are one fact, shared by the
	// updater and the script-download dialog.
	choice->add("Gitee");
	choice->add("GitHub");
	choice->value(aSource == 1 ? 1 : 0);
	if (aCallback)
		choice->callback(aCallback, aData);
	return choice;
}

} // namespace ui
} // namespace ahkdeck

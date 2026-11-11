// ui/SourceChoice.h - the one bottom function for a mirror picker.
#pragma once

#include <FL/Fl_Choice.H>

class Fl_Widget;

namespace ahkdeck
{
namespace ui
{

// Both the updater and the script-download dialog offer the same two mirrors.
// m147: the item list, their order, the default and the theme styling used to
// be written twice, and the download dialog overrode the text size afterwards.
// One factory keeps those facts in one place. `aSource` is 0 = Gitee, 1 =
// GitHub; any other value falls back to Gitee.
Fl_Choice *MakeSourceChoice(int aX, int aY, int aW, int aH, int aSource,
	Fl_Callback *aCallback, void *aData);

} // namespace ui
} // namespace ahkdeck

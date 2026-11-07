// AboutDialog.h - "关于 AHKDeck" dialog: version, author, repository links,
// license and donation QR codes. Structure mirrored from pecia's AboutDialog.
#pragma once

namespace ahkdeck
{
namespace ui
{

// Show the modal About dialog (blocks until closed). The frame switches are
// the owner window's persisted View options; the dialog never decides them.
void ShowAboutDialog(bool aRounded, bool aShadow);

} // namespace ui
} // namespace ahkdeck
#pragma once

// FltkErrorChannel.h - FLTK's own error channel, without the popup.
//
// m084: FLTK reports its own failures through Fl::error() / Fl::warning(),
// and the default implementation opens a MODAL DIALOG. That contradicts
// AGENTS requirement 3 ("errors go to the console, never to a popup"), and it
// is worse in a headless run: the MCP endpoint owns stdout, and a modal
// dialog in a process nobody is looking at is an error nobody can read.
//
// Fl::error and Fl::warning are function POINTERS, so the deck simply takes
// them over. The messages then go to the deck's console (the same column the
// kernel errors use) and to stderr (the MCP endpoint's log channel).

#include <functional>
#include <string>

namespace ahkdeck
{
namespace ui
{

// Takes over Fl::error / Fl::warning. Safe to call once, early: before a
// sink is set the messages only reach stderr.
void InstallFltkErrorChannel();

// Where the messages go. The deck sets this to its console once the window
// exists; the sink must not assume anything about the UI (it may be called
// during the first layout).
void SetFltkErrorSink(std::function<void(const std::string &)> aSink);

// The messages seen so far, newest last. The gates read this to prove that
// a session stayed quiet (a FLTK error that used to be a popup).
const std::string &FltkErrorLog();

// How many messages the channel has carried.
int FltkErrorCount();

} // namespace ui
} // namespace ahkdeck

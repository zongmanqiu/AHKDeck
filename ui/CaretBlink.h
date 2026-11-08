// CaretBlink.h - one process-wide 500 ms caret phase, shared by every text
// entry in the deck.
//
// Why: FLTK's Fl_Input does not blink at all (Fl_Input_::drawtext() only asks
// "does it have focus", never a timer), while Fl_Text_Display only honors
// show_cursor(). Without this, the editor's caret and the filter/find/chat
// inputs would have two different feels. One phase also means every widget
// blinks in step.
//
// The phase lives in an inline function's static so it is one per process, not
// one per translation unit.
#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Widget.H>

#include <cstddef>
#include <vector>

namespace CaretBlink
{

inline bool &phase()
{
	static bool on = true;
	return on;
}

inline std::vector<Fl_Widget *> &registry()
{
	static std::vector<Fl_Widget *> widgets;
	return widgets;
}

inline bool &started()
{
	static bool running = false;
	return running;
}

inline void onTick(void *)
{
	phase() = !phase();
	for (Fl_Widget *w : registry())
	{
		if (!w || !w->visible_r() || Fl::focus() != w)
			continue;
		// A text display owns its own mCursorOn flag; an input's draw() reads
		// the phase and hides its caret by painting it in the background
		// colour. Both end up with the same 500 ms rhythm.
		if (auto *display = dynamic_cast<Fl_Text_Display *>(w))
		{
			display->show_cursor(phase() ? 1 : 0);
			display->damage(FL_DAMAGE_CHILD);
		}
		else
		{
			w->redraw();
		}
	}
	Fl::repeat_timeout(0.5, onTick);
}

// True = this frame draws the caret.
inline bool visible() { return phase(); }

// Focus just arrived: show the caret immediately instead of waiting for the
// next half-second tick.
inline void reset() { phase() = true; }

inline void attach(Fl_Widget *w)
{
	if (!w)
		return;
	for (Fl_Widget *x : registry())
		if (x == w)
			return;
	registry().push_back(w);
	if (!started())
	{
		started() = true;
		Fl::add_timeout(0.5, onTick);
	}
}

inline void detach(Fl_Widget *w)
{
	auto &v = registry();
	for (size_t i = 0; i < v.size(); ++i)
	{
		if (v[i] == w)
		{
			v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
			return;
		}
	}
}

} // namespace CaretBlink

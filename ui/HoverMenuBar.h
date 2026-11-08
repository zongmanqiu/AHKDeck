// HoverMenuBar - Fl_Menu_Bar with a visible hover highlight on top-level items
// and a pressed fill for the menu that is currently open (m022).
//
// Stock Fl_Menu_Bar only highlights while a menu is open; hovering a title
// shows nothing. This subclass draws the bar itself with FLTK's exact item
// geometry (start x+3, each item = measure + 16), so the hover fill, the
// pressed fill and the popup all line up.

#pragma once

#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

class HoverMenuBar : public Fl_Menu_Bar
{
public:
	HoverMenuBar(int x, int y, int w, int h, const char *label = nullptr)
		: Fl_Menu_Bar(x, y, w, h, label)
	{
	}

	void setPressColor(Fl_Color aColor) { m_pressColor = aColor; }

	void draw() override
	{
		draw_box();
		if (!menu() || !menu()->text)
			return;
		const int highlight = m_menuOpen ? m_highlight : HoverIndex();
		int itemX = x() + 3;
		int index = 0;
		for (const Fl_Menu_Item *item = menu()->first(); item->text; item = item->next())
		{
			const int itemW = item->measure(0, this) + 16;
			if (index == highlight)
			{
				Fl_Color fill = selection_color();
				if (m_menuOpen && m_pressColor != 0)
					fill = m_pressColor;
				fl_draw_box(box(), itemX, y(), itemW, h(), fill);
			}
			Fl_Label label;
			label.value = item->text;
			label.image = nullptr;
			label.deimage = nullptr;
			label.type = item->labeltype_ ? item->labeltype_ : FL_NORMAL_LABEL;
			label.font = item->labelfont_ ? item->labelfont_ : textfont();
			label.size = item->labelsize_ ? item->labelsize_ : textsize();
			label.color = item->labelcolor_ ? item->labelcolor_ : textcolor();
			label.h_margin_ = label.v_margin_ = label.spacing = 0;
			if (!item->active())
				label.color = fl_inactive((Fl_Color)label.color);
			label.draw(itemX, y(), itemW, h(), FL_ALIGN_CENTER);
			itemX += itemW;
			++index;
		}
	}

	int handle(int aEvent) override
	{
		if (aEvent == FL_ENTER || aEvent == FL_LEAVE || aEvent == FL_MOVE)
			redraw();

		if (aEvent == FL_PUSH && menu() && menu()->text)
		{
			const Fl_Menu_Item *picked = ItemAt(Fl::event_x(), Fl::event_y());
			if (picked)
			{
				m_highlight = ItemIndexAt(Fl::event_x(), Fl::event_y());
				m_menuOpen = true;
				redraw();
				Fl::repeat_timeout(0.03, TrackHover, this);
				picked = menu()->pulldown(x(), y(), w(), h(), picked, this, 0, 1);
				picked_(picked);
				m_menuOpen = false;
				Fl::remove_timeout(TrackHover, this);
				m_highlight = -1;
				redraw();
			}
			return 1;
		}
		return Fl_Menu_Bar::handle(aEvent);
	}

private:
	void picked_(const Fl_Menu_Item *aItem)
	{
		// pulldown() also returns items that only open a submenu (the bar's own
		// 文件 / 编辑 / ... titles, and submenu parents inside them). Those carry a
		// NULL callback by design, and Fl_Menu_Item::do_callback() is a bare
		// `callback_(o, user_data_)` with no null test - the header even says the
		// caller must check first. Without this guard clicking a bare title ran off
		// to address 0 (measured 2026-11-08: 0xC0000005, ExceptionAddress 0x0,
		// ExceptionInformation[0]=8 = execute, stack top HoverMenuBar::handle).
		if (aItem && aItem->callback())
			aItem->do_callback(this);
	}

	const Fl_Menu_Item *ItemAt(int aX, int aY)
	{
		if (!menu() || aY < y() || aY >= y() + h())
			return nullptr;
		int itemX = x() + 3;
		for (const Fl_Menu_Item *item = menu()->first(); item->text; item = item->next())
		{
			const int itemW = item->measure(0, this) + 16;
			if (aX >= itemX && aX < itemX + itemW)
				return item;
			itemX += itemW;
		}
		return nullptr;
	}

	int ItemIndexAt(int aX, int aY) const
	{
		if (!menu() || aY < y() || aY >= y() + h())
			return -1;
		int itemX = x() + 3;
		int index = 0;
		for (const Fl_Menu_Item *item = menu()->first(); item->text; item = item->next())
		{
			const int itemW = item->measure(0, this) + 16;
			if (aX >= itemX && aX < itemX + itemW)
				return index;
			itemX += itemW;
			++index;
		}
		return -1;
	}

	int HoverIndex() const
	{
		if (Fl::belowmouse() != this)
			return -1;
		const int mx = Fl::event_x();
		const int my = Fl::event_y();
		if (mx < x() || mx >= x() + w() || my < y() || my >= y() + h())
			return -1;
		return ItemIndexAt(mx, my);
	}

	static void TrackHover(void *aSelf)
	{
		auto *self = static_cast<HoverMenuBar *>(aSelf);
		if (!self->m_menuOpen)
			return;
		int windowX = 0;
		int windowY = 0;
		for (Fl_Window *w = self->window(); w; w = w->window())
		{
			windowX += w->x();
			windowY += w->y();
		}
		const int index = self->ItemIndexAt(Fl::event_x_root() - windowX,
			Fl::event_y_root() - windowY);
		int highlight = self->m_highlight;
		if (index >= 0)
			highlight = index;
		else if (Fl::event_y_root() >= windowY + self->y()
			&& Fl::event_y_root() < windowY + self->y() + self->h())
			highlight = -1;
		if (highlight != self->m_highlight)
		{
			self->m_highlight = highlight;
			self->redraw();
		}
		Fl::repeat_timeout(0.03, TrackHover, self);
	}

	int m_highlight = -1;
	bool m_menuOpen = false;
	Fl_Color m_pressColor = 0;
};

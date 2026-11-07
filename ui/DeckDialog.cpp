#include "ui/DeckDialog.h"

#include "ui/DeckFrame.h"
#include "ui/DeckTitleBar.h"
#include "ui/Theme.h"
#include "ui/WindowFrame.h"
#include "ui/WindowShadow.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <windows.h>

namespace ahkdeck
{
namespace ui
{

int DeckDialog::ContentTop() const
{
	return DeckTitleBar::kHeight;
}

DeckDialog::DeckDialog(int aW, int aH, const char *aTitle)
	: Fl_Double_Window(aW, aH, aTitle)
{
	set_modal();
	// The frame is ours: no OS caption, no OS border. InstallDialogFrame then
	// gives the 1px strip somewhere to live and attaches the glow.
	border(0);
	box(FL_FLAT_BOX);
	color(DeckColors().background1);

	begin();
	mTitleBar = new DeckTitleBar(0, 0, aW, DeckTitleBar::kHeight);
	mTitleBar->setCaption(DeckTitleBar::kCaptionCloseOnly);
	mTitleBar->setOnClose([this]() { OnCaptionClose(); });
	end();
}

DeckDialog::~DeckDialog()
{
	Fl::remove_timeout(FirstFrameFix, this);
	// Destroy the glow before the window does: a shadow whose owner is gone
	// would be left floating on the desktop.
	windowShadow::detach(this);
}

void DeckDialog::SetFrameOptions(bool aRounded, bool aShadow)
{
	mRounded = aRounded;
	mShadow = aShadow;
	if (!mFrameInstalled)
		return;
	// The same five calls the main window makes when the user flips the switch;
	// the glow is applied to every attached window, so dialogs follow live.
	windowChrome::setRoundedCornersEnabled(mRounded);
	windowChrome::applyRoundedCorners(this, mRounded, (unsigned int)FrameBorderColour());
	windowChrome::refreshFrameBand(this);
	windowShadow::setEnabled(mShadow);
	windowShadow::applyAll();
	windowShadow::sync(this);
	redraw();
}

void DeckDialog::ShowCentered()
{
	// Centre on the work area, not the full monitor: a dialog whose bottom row
	// hides under the taskbar is not centered, however the maths says so.
	int x = 0;
	int y = 0;
	RECT work;
	if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
	{
		x = work.left + ((work.right - work.left) - w()) / 2;
		y = work.top + ((work.bottom - work.top) - h()) / 2;
	}
	else
	{
		x = (Fl::w() - w()) / 2;
		y = (Fl::h() - h()) / 2;
	}
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	position(x, y);

	show();
	if (!mFrameInstalled)
		InstallFrame();
	else
	{
		// hide() destroys the glow, so a re-shown dialog has to opt back in
		// before the sync can find anything to render.
		windowShadow::attach(this);
		SyncFrame();
	}
	// m089: Pecia's double self-heal (DialogBase::show): the 0s tick catches
	// the first frame, the 0.25s tick catches DWM's first composition. One
	// shot can still lose that race - with two, the decoration is right
	// within 0.25s and no click is needed.
	Fl::remove_timeout(FirstFrameFix, this);
	Fl::add_timeout(0.0, FirstFrameFix, this);
	Fl::add_timeout(0.25, FirstFrameFix, this);
}

void DeckDialog::InstallFrame()
{
	mFrameInstalled = true;
	InstallDialogFrame(this, mTitleBar, (unsigned int)FrameBorderColour(),
		mRounded, mShadow);
}

void DeckDialog::SyncFrame()
{
	windowChrome::refreshFrameBand(this);
	windowShadow::sync(this);
}

void DeckDialog::FirstFrameFix(void *aSelf)
{
	auto *self = static_cast<DeckDialog *>(aSelf);
	if (!self->shown())
		return;
	self->SyncFrame();
	self->redraw();
#if defined(_WIN32)
	// m089: Pecia's DialogBase::firstFrameFixCb also nudges the modal window
	// into the foreground. SW_SHOWNOACTIVATE leaves a modal unactivated, and
	// the first activation is what brings the first WM_PAINT / WM_WINDOWPOSCHANGED
	// storm - the wave that heals the frame. Without it the corner arc can stay
	// stale until the user clicks the title bar.
	if (HWND hwnd = fl_xid(self))
		SetForegroundWindow(hwnd);
#endif
}

void DeckDialog::hide()
{
	Fl::remove_timeout(FirstFrameFix, this);
	// A modal dialog has no subclass proc to notice the close, so nothing else
	// would take the glow with it.
	windowShadow::detach(this);
	Fl_Double_Window::hide();
}

void DeckDialog::draw()
{
	Fl_Double_Window::draw();
	if (mFrameInstalled)
		PaintDeckClientBorder(this);
}

void DeckDialog::resize(int X, int Y, int W, int H)
{
	Fl_Double_Window::resize(X, Y, W, H);
	if (mTitleBar)
		mTitleBar->resize(0, 0, DeckClientWidth(this), DeckTitleBar::kHeight);
	if (mFrameInstalled)
		SyncDeckFrame(this);
}

} // namespace ui
} // namespace ahkdeck

// DeckDialog.h - the skeleton every deck dialog is built on.
//
// The main window and its dialogs are the same product, so they wear the same
// frame: borderless window, our own caption bar, the 1px ring, the layered
// outer glow, and the same rounded-corner / shadow switches. Before this class
// existed the dialogs carried a native caption that only had DWM colours
// painted on it, which is why they read as a different application.
//
// Subclasses add content BELOW ContentTop(); nothing may sit under the caption.
//   begin();
//   ... widgets at y >= ContentTop() ...
//   end();
//   ShowCentered();
#pragma once

#include <FL/Fl_Double_Window.H>

namespace ahkdeck
{
namespace ui
{

class DeckTitleBar;

class DeckDialog : public Fl_Double_Window
{
public:
	DeckDialog(int aW, int aH, const char *aTitle);
	~DeckDialog() override;

	// y where content begins (just below the caption bar).
	int ContentTop() const;

	// The owner's persisted View switches. A dialog reads them; it never
	// decides. Pass them again when the user toggles 视图/窗口圆角 or 阴影.
	void SetFrameOptions(bool aRounded, bool aShadow);

	// Centre on the work area, show, then install the frame - there is no HWND
	// to decorate before show().
	void ShowCentered();

	void hide() override;

protected:
	DeckTitleBar *CaptionBar() const { return mTitleBar; }

	// The caption's close box. Override to veto (save first, confirm, ...).
	virtual void OnCaptionClose() { hide(); }

	void draw() override;
	void resize(int X, int Y, int W, int H) override;

private:
	void InstallFrame();
	void SyncFrame();

	// Pecia's lesson, learned the expensive way: the decorations are applied
	// after ShowWindow already made the window visible, so a dialog that never
	// gets a mouse message keeps a missing glow and a stale ring. One follow-up
	// tick after the first frame re-syncs both.
	static void FirstFrameFix(void *aSelf);

	DeckTitleBar *mTitleBar = nullptr;
	bool mRounded = true;
	bool mShadow = true;
	bool mFrameInstalled = false;
};

} // namespace ui
} // namespace ahkdeck

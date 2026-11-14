// ui/OptionsDialog.cpp - see ui/OptionsDialog.h.
//
// Layout follows the deck's dialog rules (ScriptInfoDialog is the reference):
// 16px margin, a 26px field row, one 10px gap between a label column and its
// control, and a right-aligned button row that never moves when the text
// changes. Every y is computed from the row above, so adding a second option is
// one more pair of lines rather than a re-measure of the window.
//
// m178: no explanatory paragraph anywhere in this window - the user's call.
// A heading plus its control is the whole vocabulary; anything that needs
// explaining belongs in the docs, not here.
#include "ui/OptionsDialog.h"

#include "ui/DeckDialog.h"
#include "ui/DeckTitleBar.h"
#include "ui/SourceChoice.h"
#include "ui/Theme.h"
#include "ui/ThemeWidgets.h"
#include "ui/UiMetrics.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>

namespace ahkdeck
{
namespace ui
{
namespace
{

// m182: the shared facts come from UiMetrics - one margin, one gap, one row
// height for every window. Only this dialog's own column widths stay local.
constexpr int kWidth = 440;
constexpr int kMargin = kUiEdge;
constexpr int kLabelW = 76;  // "官方下载源" needs more room than the 48px label
constexpr int kGap = kUiGap;
constexpr int kRowH = kUiControl;
constexpr int kBtnH = kUiControl;
constexpr int kChoiceW = 178;
constexpr int kHeadingH = 24;
constexpr int kGroupGap = kUiGap;

// The four windows a user can pick, and the ONE place their mapping to the
// setting lives. Order is the popup's order. 0 is 「永不」 because that is what
// core/Housekeeping.h already treats as "nothing expires" - the store format
// did not have to change for this window to exist.
struct RetentionChoice
{
	const char *label;
	int days;
};

constexpr RetentionChoice kRetention[] = {
	{ "永不", 0 },
	{ "7 天", 7 },
	{ "30 天", 30 },
	{ "180 天", 180 },
};
constexpr int kRetentionCount = (int)(sizeof(kRetention) / sizeof(kRetention[0]));

int ChoiceForDays(int aDays)
{
	// A hand-edited value (say 14) is not in the list. Show it as 30 - the
	// default - rather than silently landing on 永不, which would stop the
	// cleanup without the user asking.
	for (int i = 0; i < kRetentionCount; ++i)
	{
		if (kRetention[i].days == aDays)
			return i;
	}
	return 2;
}

class OptionsDialog : public DeckDialog
{
public:
	OptionsDialog(int aCurrentDays, int aCurrentSource, bool aRounded, bool aShadow)
		: DeckDialog(kWidth, Height(), "选项")
	{
		SetFrameOptions(aRounded, aShadow);
		mInitial = ChoiceForDays(aCurrentDays);
		mInitialSource = (aCurrentSource == 1) ? 1 : 0;
	}

	static int Height()
	{
		// caption + margin + two (heading + field row) groups + gap + buttons
		return DeckTitleBar::kHeight + kMargin
			+ (kHeadingH + kRowH) * 2 + kGroupGap
			+ kBtnH + kMargin;
	}

	void Build()
	{
		const ThemeColors &colors = DeckColors();
		begin();

		int y = ContentTop() + kMargin;

		// ① 官方下载源 - ONE setting, three consumers (script catalog URL,
		// which mirror a script comes from, update manifest URL, and which
		// release asset the updater takes). It lives here and nowhere else:
		// both dialogs show it read-only, so two places cannot disagree.
		Fl_Box *sourceHeading = new Fl_Box(FL_NO_BOX, kMargin, y,
			kWidth - 2 * kMargin, kHeadingH, nullptr);
		sourceHeading->copy_label("官方下载源");
		sourceHeading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		sourceHeading->labelsize(TextSize());
		sourceHeading->labelcolor(colors.text1);
		y += kHeadingH;

		Fl_Box *sourceLabel = new Fl_Box(FL_NO_BOX, kMargin, y, kLabelW, kRowH,
			nullptr);
		sourceLabel->copy_label("来源");
		sourceLabel->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
		sourceLabel->labelsize(SubTextSize());
		sourceLabel->labelcolor(colors.text2);

		// m147 keeps the mirror list in one factory (SourceChoice.h); this
		// window is now the only caller - the two dialogs read the value.
		mSource = MakeSourceChoice(kMargin + kLabelW + kGap, y, kChoiceW, kRowH,
			mInitialSource, nullptr, nullptr);
		y += kRowH + kGroupGap;

		// ② 临时文件
		Fl_Box *tempHeading = new Fl_Box(FL_NO_BOX, kMargin, y,
			kWidth - 2 * kMargin, kHeadingH, nullptr);
		tempHeading->copy_label("临时文件");
		tempHeading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		tempHeading->labelsize(TextSize());
		tempHeading->labelcolor(colors.text1);
		y += kHeadingH;

		Fl_Box *label = new Fl_Box(FL_NO_BOX, kMargin, y, kLabelW, kRowH, nullptr);
		label->copy_label("保留时长");
		label->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
		label->labelsize(SubTextSize());
		label->labelcolor(colors.text2);

		// m148's SettingsChoice, the same flat dropdown the updater uses: the
		// stock Fl_Choice paints a raised grey button under this scheme.
		mDays = new SettingsChoice(kMargin + kLabelW + kGap, y, kChoiceW, kRowH);
		mDays->box(FL_BORDER_BOX);
		mDays->down_box(FL_BORDER_BOX);
		mDays->color(colors.background1);
		mDays->selection_color(colors.highlight2);
		mDays->textcolor(colors.text1);
		mDays->labelcolor(colors.text1); // SettingsChoice draws the arrow with it
		mDays->textsize(TextSize());
		mDays->labelsize(TextSize());
		for (int i = 0; i < kRetentionCount; ++i)
			mDays->add(kRetention[i].label);
		mDays->value(mInitial);

		y += kRowH + kGroupGap;
		mOk = new HoverButton(0, 0, 0, kBtnH, "确定");
		mOk->color(colors.background2);
		mOk->labelcolor(colors.text1);
		mOk->labelsize(TextSize());
		mOk->callback(OnOk, this);
		mCancel = new HoverButton(0, 0, 0, kBtnH, "取消");
		mCancel->color(colors.background2);
		mCancel->labelcolor(colors.text1);
		mCancel->labelsize(TextSize());
		mCancel->callback(OnCancel, this);
		FitButtonRow({ mOk, mCancel }, kWidth, y + kBtnH / 2, kMargin, kGap);

		end();
	}

	void Run()
	{
		ShowCentered();
		while (shown())
			Fl::wait();
	}

	// The caption's × and Esc both cancel: nothing is written until 确定.
	void OnCaptionClose() override { mAccepted = false; hide(); }

	bool Accepted() const { return mAccepted; }

	OptionsResult Chosen() const
	{
		OptionsResult out;
		const int index = mDays ? mDays->value() : mInitial;
		out.days = kRetention[(index >= 0 && index < kRetentionCount) ? index : 2].days;
		out.source = (mSource && mSource->value() == 1) ? 1 : 0;
		return out;
	}

private:
	int handle(int aEvent) override
	{
		if (aEvent == FL_KEYDOWN && Fl::event_key() == FL_Escape)
		{
			mAccepted = false;
			hide();
			return 1;
		}
		return DeckDialog::handle(aEvent);
	}

	static void OnOk(Fl_Widget *, void *aData)
	{
		auto *self = static_cast<OptionsDialog *>(aData);
		self->mAccepted = true;
		self->hide();
	}

	static void OnCancel(Fl_Widget *, void *aData)
	{
		auto *self = static_cast<OptionsDialog *>(aData);
		self->mAccepted = false;
		self->hide();
	}

	int mInitial = 2;
	int mInitialSource = 0;
	bool mAccepted = false;
	SettingsChoice *mDays = nullptr;
	Fl_Choice *mSource = nullptr;
	HoverButton *mOk = nullptr;
	HoverButton *mCancel = nullptr;
};

} // namespace

bool ShowOptionsDialog(int aCurrentDays, int aCurrentSource, bool aRounded,
	bool aShadow, OptionsResult &aOut)
{
	OptionsDialog dialog(aCurrentDays, aCurrentSource, aRounded, aShadow);
	dialog.Build();
	dialog.Run();
	if (!dialog.Accepted())
		return false;
	aOut = dialog.Chosen();
	return true;
}

} // namespace ui
} // namespace ahkdeck

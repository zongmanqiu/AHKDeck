// AboutDialog.cpp - "关于 AHKDeck" dialog.
// The whole content area is ONE self-drawn widget (AboutPanel) that paints
// every text element with fl_draw at explicit local coordinates, so lines
// cannot drift apart. The repository row is a separate widget with two hit
// regions (Gitee | GitHub); the QR codes are SVGs embedded as RCDATA.
#include "ui/AboutDialog.h"

#include "core/Version.h"
#include "ui/DeckDialog.h"
#include "ui/DeckIcon.h"
#include "ui/DeckTitleBar.h"
#include "ui/Theme.h"

#include <FL/Fl_Widget.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/fl_draw.H>

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <utility>
#include <vector>

namespace ahkdeck
{
namespace ui
{
namespace
{

// Clickable repository row: "项目仓库： Gitee | GitHub". Each name is its own
// hit region, so both URLs are reachable from the one row.
class RepoLinksWidget : public Fl_Widget
{
public:
	RepoLinksWidget(int X, int Y, int W, int H, const char *prefix,
		const char *giteeUrl, const char *githubUrl)
		: Fl_Widget(X, Y, W, H), m_prefix(prefix ? prefix : "")
	{
		box(FL_NO_BOX);
		m_link[0].label = "Gitee";
		m_link[0].url = giteeUrl ? giteeUrl : "";
		m_link[1].label = "GitHub";
		m_link[1].url = githubUrl ? githubUrl : "";
	}

	// Full row text as one string; AboutPanel::contentWidth() measures it so
	// the window is wide enough for the whole row.
	static std::string fullLabel(const char *prefix)
	{
		return std::string(prefix ? prefix : "") + " Gitee | GitHub";
	}

	int handle(int event) override
	{
		switch (event)
		{
		case FL_ENTER:
		case FL_MOVE:
		{
			const int hit = hitAt(Fl::event_x());
			if (hit != m_hover)
			{
				m_hover = hit;
				redraw();
			}
			return 1;
		}
		case FL_LEAVE:
			if (m_hover != -1)
			{
				m_hover = -1;
				redraw();
			}
			return 1;
		case FL_PUSH:
			return 1;
		case FL_RELEASE:
		{
			if (!Fl::event_inside(this))
				return 0;
			const int hit = hitAt(Fl::event_x());
			if (hit >= 0 && !m_link[hit].url.empty())
				ShellExecuteA(nullptr, "open", m_link[hit].url.c_str(), nullptr,
					nullptr, SW_SHOWNORMAL);
			return 1;
		}
		}
		return 0;
	}

	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		// Clean background first so antialiased text edges do not bleed colors
		// from the parent panel beneath.
		fl_rectf(x(), y(), w(), h(), DeckColors().background1);
		const Fl_Color base = DeckColors().text1;
		const Fl_Color hot = DeckColors().highlight3;
		int tx = 0, ty = 0, segX[2] = { 0, 0 }, segW[2] = { 0, 0 };
		layout(&tx, &ty, segX, segW);
		fl_color(base);
		fl_draw(m_prefix.c_str(), tx, ty);
		for (int i = 0; i < 2; ++i)
		{
			if (i == 1)
			{
				fl_color(base);
				fl_draw(m_sep, segX[0] + segW[0], ty);
			}
			const Fl_Color col = (i == m_hover) ? hot : base;
			fl_color(col);
			fl_draw(m_link[i].label.c_str(), segX[i], ty);
			fl_line_style(FL_SOLID, 1);
			fl_color(col);
			fl_line(segX[i], ty + 2, segX[i] + segW[i], ty + 2);
			fl_line_style(0);
		}
		fl_pop_clip();
	}

	void setFontSize(int fs) { m_fs = fs; }

private:
	struct Link
	{
		std::string label, url;
	};

	// Centered row geometry; draw() and hitAt() share it so the painted links
	// and their clickable regions can never drift apart.
	void layout(int *tx, int *ty, int segX[2], int segW[2]) const
	{
		fl_font(FL_HELVETICA, m_fs);
		const int pw = (int)fl_width(m_prefix.c_str());
		const int lw0 = (int)fl_width(m_link[0].label.c_str());
		const int lw1 = (int)fl_width(m_link[1].label.c_str());
		const int sw = (int)fl_width(m_sep);
		const int gap = 4;
		const int total = pw + gap + lw0 + sw + lw1;
		*tx = x() + (w() - total) / 2;
		*ty = y() + m_fs;
		segX[0] = *tx + pw + gap;
		segW[0] = lw0;
		segX[1] = segX[0] + lw0 + sw;
		segW[1] = lw1;
	}

	int hitAt(int px) const
	{
		int tx = 0, ty = 0, segX[2] = { 0, 0 }, segW[2] = { 0, 0 };
		layout(&tx, &ty, segX, segW);
		for (int i = 0; i < 2; ++i)
			if (px >= segX[i] && px < segX[i] + segW[i])
				return i;
		return -1;
	}

	std::string m_prefix;
	const char *m_sep = " | ";
	Link m_link[2];
	int m_hover = -1;
	int m_fs = 16;
};

// Load an SVG QR code embedded as a Windows RCDATA resource (see
// version_info.rc.in and DeckIcon.h). Returns nullptr if absent.
Fl_Image *loadImageResource(int resid)
{
	HRSRC hrsc = FindResourceW(nullptr, MAKEINTRESOURCEW(resid), MAKEINTRESOURCEW(10)); // RT_RCDATA
	if (!hrsc)
		return nullptr;
	HGLOBAL hg = LoadResource(nullptr, hrsc);
	if (!hg)
		return nullptr;
	void *data = LockResource(hg);
	DWORD len = SizeofResource(nullptr, hrsc);
	if (!data || len == 0)
		return nullptr;
	Fl_SVG_Image *img = new Fl_SVG_Image(nullptr,
		static_cast<const unsigned char *>(data), static_cast<size_t>(len));
	// Construction fails silently into an empty image on a parse error.
	if (img->w() > 0)
		return img;
	delete img;
	return nullptr;
}

// Load exeDir/image/<base> as a Fl_Image*, trying .svg then .png. The caller
// passes the base name (no extension). Returns nullptr if absent.
Fl_Image *loadImageBase(const char *base)
{
	wchar_t exe[MAX_PATH];
	wchar_t path[MAX_PATH];
	if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
		return nullptr;
	wchar_t *slash = wcsrchr(exe, L'\\');
	if (!slash)
		return nullptr;
	*(slash + 1) = 0;
	char narrow[MAX_PATH * 2];
	auto narrowize = [&](const wchar_t *p) -> const char * {
		int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, narrow,
			(int)sizeof(narrow), nullptr, nullptr);
		return n > 0 ? narrow : nullptr;
	};
	swprintf(path, MAX_PATH, L"%simage\\%hs.svg", exe, base);
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
	{
		const char *np = narrowize(path);
		if (np)
		{
			Fl_SVG_Image *img = new Fl_SVG_Image(np);
			if (img->w() > 0)
				return img;
			delete img;
		}
	}
	swprintf(path, MAX_PATH, L"%simage\\%hs.png", exe, base);
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
	{
		const char *np = narrowize(path);
		if (np)
		{
			Fl_PNG_Image *img = new Fl_PNG_Image(np);
			if (img->w() > 0)
				return img;
			delete img;
		}
	}
	return nullptr;
}

// Placeholder for a missing QR image.
Fl_Image *placeholderImage(const char *caption)
{
	const int SZ = 200;
	Fl_Image_Surface surf(SZ, SZ);
	Fl_Surface_Device::push_current(&surf);
	fl_color(0xEC, 0xEC, 0xEC);
	fl_rectf(0, 0, SZ, SZ);
	fl_color(0xC0, 0xC0, 0xC0);
	fl_rect(0, 0, SZ, SZ);
	fl_color(0x9A, 0x9A, 0x9A);
	fl_font(FL_HELVETICA, 12);
	fl_draw(caption, 0, 0, SZ, SZ, FL_ALIGN_CENTER);
	Fl_Surface_Device::pop_current();
	return surf.image();
}

// The entire About content, self-drawn in one widget. A single centered column
// of lines (empty entries = blank lines); the first line is the app title
// (large), the rest body lines. QRs sit centered at the bottom. Width adapts
// to the widest line, clamped to a minimum that fits the QRs.
class AboutPanel : public Fl_Widget
{
public:
	static constexpr int kSidePad = 10;
	// SVG images are pre-rasterized at this resolution so they render fully
	// (Fl_SVG_Image's own on-the-fly downscale can draw only part of the art).
	static constexpr int kQrSize = 160;
	// Generous gap so one code is not accidentally scanned while aiming at
	// the other.
	static constexpr int kQrGap = 28;
	// White frame around each QR so it stays distinct from the panel.
	static constexpr int kQrBorder = 2;
	static constexpr int kMinW = 2 * kQrSize + kQrGap + 2 * kSidePad;  // 368
	static constexpr int kMinH = 490;
	// Line indices reserved for the clickable repository rows (drawn by
	// RepoLinksWidget, not by draw()); panel layout and the widgets must agree.
	// m178: the scripts repository gets its own row - a user who wants to fork
	// one script should not have to guess that it lives in another project.
	static constexpr size_t kRepoLineIndex = 5;
	static constexpr size_t kScriptsLineIndex = 6;
	static constexpr int kBottomPad = 5;

	AboutPanel(int X, int Y, int W, int H, int fs, std::vector<std::string> lines)
		: Fl_Widget(X, Y, W, H), m_fs(fs), m_lines(std::move(lines))
	{
		box(FL_NO_BOX);
		m_qr0Name = "微信";
		m_qr1Name = "支付宝";
		m_qr0 = qrReady(ID_QR_WECHAT, "WeChatPay", m_qr0Name.c_str());
		m_qr1 = qrReady(ID_QR_ALIPAY, "ALiPay", m_qr1Name.c_str());
	}

	~AboutPanel() override
	{
		delete m_qr0;
		delete m_qr1;
	}

	// Repository link text (drawn by RepoLinksWidget); contentWidth() needs it
	// here to measure the widest line correctly. More than one row is allowed.
	void addUrlLabel(const char *label)
	{
		if (label && *label)
			m_urlLabels.push_back(label);
	}

	// Top Y (relative to the panel top) of line `index`, using exactly the
	// advance rules of draw(), so the link widget cannot drift when lines are
	// added or the font size changes.
	int lineTopY(size_t index) const
	{
		int y = 12;
		for (size_t i = 0; i < index && i < m_lines.size(); ++i)
		{
			if (m_lines[i].empty())
			{
				y += 2 * (m_fs / 2 + 4);
				continue;
			}
			y += ((i == 0) ? m_fs + 14 : m_fs) + 8;
		}
		return y;
	}

	// SVG -> pre-rasterized at kQrSize; PNG -> as-is; missing -> placeholder.
	// Prefers the QR SVG embedded in the exe and falls back to an exe-adjacent
	// image/<base> file.
	static Fl_Image *qrReady(int resid, const char *base, const char *caption)
	{
		Fl_Image *img = loadImageResource(resid);
		if (img)
		{
			if (dynamic_cast<Fl_SVG_Image *>(img))
				static_cast<Fl_SVG_Image *>(img)->resize(kQrSize, kQrSize);
			return img;
		}
		img = loadImageBase(base);
		if (img)
		{
			if (dynamic_cast<Fl_SVG_Image *>(img))
				static_cast<Fl_SVG_Image *>(img)->resize(kQrSize, kQrSize);
			return img;
		}
		return placeholderImage(caption);
	}

	// Widest line (pixels) at the current font; called at show time when the
	// graphics context is ready so fl_measure is reliable.
	int contentWidth() const
	{
		fl_font(FL_HELVETICA, m_fs);
		int maxW = 0;
		for (const auto &s : m_lines)
		{
			int tw = 0, th = 0;
			fl_measure(s.c_str(), tw, th);
			if (tw > maxW)
				maxW = tw;
		}
		if (!m_urlLabels.empty())
		{
			for (const auto &label : m_urlLabels)
			{
				int urlW = 0, urlH = 0;
				fl_measure(label.c_str(), urlW, urlH);
				if (urlW > maxW)
					maxW = urlW;
			}
		}
		return maxW + 2 * kSidePad;
	}

	// Content height mirroring draw()'s advance logic via lineTopY().
	int contentHeight() const
	{
		int y = lineTopY(m_lines.size());
		if (m_qr0 && m_qr1)
			y += m_fs + 8 + kQrSize;
		return y;
	}

	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		const int cx = x() + w() / 2;
		int y = this->y() + 12;

		fl_font(FL_HELVETICA, m_fs);
		for (size_t i = 0; i < m_lines.size(); ++i)
		{
			const std::string &s = m_lines[i];
			if (s.empty())
			{
				y += 2 * (m_fs / 2 + 4);
				continue;
			}
			const bool isTitle = (i == 0);
			fl_font(isTitle ? FL_HELVETICA_BOLD : FL_HELVETICA,
				isTitle ? m_fs + 14 : m_fs);
			fl_color(DeckColors().text1);
			fl_draw(s.c_str(), cx - (int)fl_width(s.c_str()) / 2,
				y + (isTitle ? m_fs + 14 : m_fs));
			y += (isTitle ? m_fs + 14 : m_fs) + 8;
		}

		// Two QR images side by side, centered at the bottom, each with a
		// caption directly above it and a white frame around the code.
		if (m_qr0 && m_qr1)
		{
			const int totalW = 2 * kQrSize + kQrGap;
			const int x0 = x() + (w() - totalW) / 2;
			if (x0 >= x() && x0 + totalW <= x() + w())
			{
				fl_font(FL_HELVETICA, m_fs);
				fl_color(DeckColors().text1);
				const int cap0 = (int)fl_width(m_qr0Name.c_str());
				fl_draw(m_qr0Name.c_str(), x0 + kQrSize / 2 - cap0 / 2, y + m_fs);
				const int cap1 = (int)fl_width(m_qr1Name.c_str());
				fl_draw(m_qr1Name.c_str(),
					x0 + kQrSize + kQrGap + kQrSize / 2 - cap1 / 2, y + m_fs);
				const int qy = y + m_fs + 8;
				const int frame0x = x0 - kQrBorder;
				const int frame1x = x0 + kQrSize + kQrGap - kQrBorder;
				const int frameY = qy - kQrBorder;
				const int frameSz = kQrSize + 2 * kQrBorder;
				fl_draw_box(FL_FLAT_BOX, frame0x, frameY, frameSz, frameSz, FL_WHITE);
				fl_draw_box(FL_FLAT_BOX, frame1x, frameY, frameSz, frameSz, FL_WHITE);
				m_qr0->draw(x0, qy, kQrSize, kQrSize, 0, 0);
				m_qr1->draw(x0 + kQrSize + kQrGap, qy, kQrSize, kQrSize, 0, 0);
			}
		}
		fl_pop_clip();
	}

private:
	int m_fs;
	std::vector<std::string> m_lines;
	Fl_Image *m_qr0 = nullptr;
	Fl_Image *m_qr1 = nullptr;
	std::string m_qr0Name, m_qr1Name;
	std::vector<std::string> m_urlLabels;
};

class AboutDialog : public DeckDialog
{
public:
	AboutDialog()
		: DeckDialog(AboutPanel::kMinW, AboutPanel::kMinH + DeckTitleBar::kHeight, "关于")
	{
		const int fs = 16;
		const int top = ContentTop();

		const std::string title = std::string("AHKDeck ") + VersionString();
		const std::string copyright = "Copyright © 2026 邱宗满";
		const std::string license = "依据 GPL-2.0 协议许可";
		// Same URLs the README advertises, so the window and the project page
		// can never disagree about where the source lives.
		const std::string repoGitee = "https://gitee.com/qiuzongman/AHKDeck";
		const std::string repoGithub = "https://github.com/zongmanqiu/AHKDeck";
		const std::string scriptsGitee =
			"https://gitee.com/qiuzongman/ahk-v2-scripts";
		const std::string scriptsGithub =
			"https://github.com/zongmanqiu/ahk-v2-scripts";

		std::vector<std::string> lines = {
			title,                          // 1. name + version
			"",                             // 2. blank (m091: the slogan is gone)
			copyright,                      // 3. copyright w/ author
			license,                        // 4. license
			"",                             // 5. blank
			"",                             // 6. 项目仓库 row (RepoLinksWidget)
			"",                             // 7. 脚本仓库 row (RepoLinksWidget)
			"",                             // 8. blank
			"赞助",                         // 9. sponsor
		};

		begin();
		m_panel = new AboutPanel(0, top, AboutPanel::kMinW,
			AboutPanel::kMinH, fs, std::move(lines));

		const char *repoPrefix = "项目仓库：";
		const char *scriptsPrefix = "脚本仓库：";
		m_panel->addUrlLabel(RepoLinksWidget::fullLabel(repoPrefix).c_str());
		m_panel->addUrlLabel(RepoLinksWidget::fullLabel(scriptsPrefix).c_str());
		m_urlWidget = new RepoLinksWidget(0,
			top + m_panel->lineTopY(AboutPanel::kRepoLineIndex),
			AboutPanel::kMinW, fs + 8, repoPrefix,
			repoGitee.c_str(), repoGithub.c_str());
		m_urlWidget->setFontSize(fs);
		m_scriptsWidget = new RepoLinksWidget(0,
			top + m_panel->lineTopY(AboutPanel::kScriptsLineIndex),
			AboutPanel::kMinW, fs + 8, scriptsPrefix,
			scriptsGitee.c_str(), scriptsGithub.c_str());
		m_scriptsWidget->setFontSize(fs);
		end();
	}

	// Fit width to the widest line and height to the actual content, then
	// centre on the work area (DeckDialog). The panel is NOT the window
	// resizable: shrinking the window must not shrink the panel off-centre.
	void centerAndShow()
	{
		int newW = m_panel->contentWidth();
		if (newW < AboutPanel::kMinW)
			newW = AboutPanel::kMinW;
		const int top = ContentTop();
		const int newH = m_panel->contentHeight() + AboutPanel::kBottomPad + top + 1;
		m_panel->resize(0, top, newW, newH - top);
		m_urlWidget->resize(0, m_urlWidget->y(), newW, m_urlWidget->h());
		m_scriptsWidget->resize(0, m_scriptsWidget->y(), newW, m_scriptsWidget->h());
		size(newW, newH);
		ShowCentered();
	}

private:
	AboutPanel *m_panel = nullptr;
	RepoLinksWidget *m_urlWidget = nullptr;
	RepoLinksWidget *m_scriptsWidget = nullptr;
};

} // namespace

void ShowAboutDialog(bool aRounded, bool aShadow)
{
	AboutDialog dlg;
	dlg.SetFrameOptions(aRounded, aShadow);
	dlg.centerAndShow();
	while (dlg.shown())
		Fl::wait();
}

} // namespace ui
} // namespace ahkdeck
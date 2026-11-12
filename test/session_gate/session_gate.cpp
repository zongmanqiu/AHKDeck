// m011 gate - several scripts in one host process, and who owns which record.
//
// This is the test host the plan calls for: the CLI stays single-script (so
// "--run a.ahk b.ahk" never has to guess where script arguments end), and the
// session API gets exercised here instead. It runs the probe corpus in five
// scenarios and fails the build (non-zero exit) on any broken claim:
//
//   A  three sessions concurrently: ids and threads distinct, one session's
//      throw does not disturb the others, StopAll ends the resident one for real;
//   B  a script that never parses: no thread id to route on, its record must
//      still reach its own session;
//   C  a script file that cannot even be read: same, via a host record;
//   D  parity: the same script alone vs. concurrently reports the same
//      message/file/line/rc - only the thread and session ids may differ;
//   E  an error raised in a timer callback: an AHK thread, not an OS thread,
//      so the record must still carry the session's own thread id;
//   F  pulling a script's own tray menu out of the host (m024): the kernel's
//      notify-icon message pops it on that session's thread, at the pointer,
//      one session at a time, and a finished session has no window left;
//   G  the menu-less script and the tray-menu count the deck now checks
//      before popping it (m160).
//
// Usage: session_gate <probeDir>   (Logs next to this exe; cwd holds heartbeats)

#include "engine/DeckEngine.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using ahkdeck::Engine;
using ahkdeck::ErrorRecord;
using ahkdeck::Session;
using ahkdeck::SessionState;

namespace {

int g_failures = 0;

void Check(bool aOk, const char *aWhat)
{
	std::printf("%s  %s\n", aOk ? "[PASS]" : "[FAIL]", aWhat);
	std::fflush(stdout);
	if (!aOk)
		++g_failures;
}

// Narrow -> wide for file I/O. The gate keeps its own copy on purpose: it must
// read the bytes the engine wrote, not share a helper that can drift with it.
std::wstring WideOfPath(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), out.data(), n);
	return out;
}

// The gate's own anchor, captured before any script thread moves the process
// working directory (A_WorkingDir is process-wide in AutoHotkey): every relative
// path the gate reads - a session log, a heartbeat - resolves against this, the
// same way the engine pins its log root at Load().
std::wstring g_cwd;

std::wstring Resolve(const std::string &aPath)
{
	std::wstring wide = WideOfPath(aPath);
	bool rooted = wide.size() >= 2 && wide[1] == L':';
	return rooted || g_cwd.empty() ? wide : g_cwd + L"\\" + wide;
}

std::string ExeDirectoryUtf8()
{
	wchar_t path[MAX_PATH] = {0};
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::string();
	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash)
		*slash = 0;
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
	if (bytes <= 1)
		return std::string();
	std::string out((size_t)bytes, '\0');
	WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), bytes, nullptr, nullptr);
	out.resize((size_t)(bytes - 1));
	return out;
}

// Session.logPath is relative to the exe (m051 portable layout).
std::string LogFullOf(const std::string &aRelative)
{
	return ExeDirectoryUtf8() + "\\" + aRelative;
}

std::string ReadAll(const std::string &aPath)
{
	std::ifstream f(Resolve(aPath).c_str(), std::ios::binary);
	std::string out;
	char buf[4096];
	while (f.read(buf, sizeof(buf)) || f.gcount())
		out.append(buf, (size_t)f.gcount());
	return out;
}

size_t CountLines(const std::string &aText)
{
	size_t n = 0;
	for (size_t i = 0; i < aText.size(); ++i)
		if (aText[i] == '\n')
			++n;
	return n;
}

bool Contains(const std::string &aHaystack, const std::string &aNeedle)
{
	return aHaystack.find(aNeedle) != std::string::npos;
}

// Heartbeat lines are "n|tick|osThreadId". Reading the id from the script's own
// pen is how the gate proves the writer thread IS the session's kernel thread.
bool HeartbeatThread(const std::string &aText, std::string &aThread)
{
	size_t first = aText.find('|');
	if (first == std::string::npos)
		return false;
	size_t second = aText.find('|', first + 1);
	if (second == std::string::npos)
		return false;
	size_t eol = aText.find('\n', second + 1);
	aThread = aText.substr(second + 1, eol == std::string::npos ? std::string::npos : eol - second - 1);
	return !aThread.empty();
}

struct Collector
{
	std::vector<ErrorRecord> all;

	std::vector<ErrorRecord> of(DWORD aSession) const
	{
		std::vector<ErrorRecord> out;
		for (const auto &rec : all)
			if (rec.session == aSession)
				out.push_back(rec);
		return out;
	}
};

void Drain(Engine &aEngine, Collector &aCollector)
{
	ErrorRecord rec;
	while (aEngine.PopRecord(rec, 0))
		aCollector.all.push_back(std::move(rec));
}

// Pumps records until aDone() or the deadline. The pump is what attributes a
// record and writes the per-session JSONL, so a scenario is not over until its
// records left the queue.
bool RunUntil(Engine &aEngine, Collector &aCollector, DWORD aMs, const std::function<bool()> &aDone)
{
	DWORD deadline = GetTickCount() + aMs;
	for (;;)
	{
		Drain(aEngine, aCollector);
		if (aDone())
			return true;
		if ((LONG)(deadline - GetTickCount()) < 0)
			return false;
		Sleep(10);
	}
}

bool SessionFinished(Engine &aEngine, DWORD aSessionId)
{
	Session s;
	return aEngine.GetSession(aSessionId, s) && s.state == SessionState::Finished;
}

DWORD StartOne(Engine &aEngine, const std::wstring &aPath, const std::vector<std::wstring> &aArgs)
{
	std::string error;
	DWORD id = aEngine.StartSession(aPath, aArgs, error);
	if (!id)
		std::printf("[ERROR] StartSession failed: %s\n", error.c_str());
	return id;
}

// The engine reports the kernel thread's own code; a fatal record that left the
// thread unwinding with 0 is the official code 2 (m010).
DWORD EffectiveRc(const Session &aSession)
{
	return (aSession.rc == 0 && aSession.fatal) ? 2u : aSession.rc;
}

bool AllFinished(Engine &aEngine)
{
	for (const Session &s : aEngine.Sessions())
		if (s.state != SessionState::Finished)
			return false;
	return true;
}

// Independent of the engine's table: asks the kernel object directly whether that
// thread is still executing. A session whose thread died without being reaped
// would otherwise hide behind a "running" state.
bool ThreadAlive(DWORD aThreadId)
{
	if (!aThreadId)
		return false;
	HANDLE h = OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, aThreadId);
	if (!h)
		return false;
	bool alive = WaitForSingleObject(h, 0) != WAIT_OBJECT_0;
	CloseHandle(h);
	return alive;
}

// The kernel shows a script's tray menu as a top-level menu window (#32768) on
// that script thread, so the gate can read "did the menu pop, on whose thread,
// and where" without asking the script to report anything.
struct MenuSearch
{
	DWORD threadId = 0;
	DWORD processId = 0;
	HWND found = nullptr;
	int count = 0;
};

BOOL CALLBACK EnumMenuWindow(HWND aHwnd, LPARAM aParam)
{
	MenuSearch *search = reinterpret_cast<MenuSearch *>(aParam);
	DWORD pid = 0;
	const DWORD tid = GetWindowThreadProcessId(aHwnd, &pid);
	if (pid != search->processId || !IsWindowVisible(aHwnd))
		return TRUE;
	wchar_t className[32] = {0};
	if (GetClassNameW(aHwnd, className, 32) == 0 || wcscmp(className, L"#32768") != 0)
		return TRUE;
	++search->count;
	if (!search->found && (!search->threadId || tid == search->threadId))
		search->found = aHwnd;
	return TRUE;
}

// The menu window of one thread, or (when aThreadId is 0) any menu of this
// process; aCount receives how many menus the process owns in total.
HWND FindMenuOfThread(DWORD aThreadId, int *aCount = nullptr)
{
	MenuSearch own;
	own.threadId = aThreadId;
	own.processId = GetCurrentProcessId();
	EnumWindows(EnumMenuWindow, reinterpret_cast<LPARAM>(&own));
	if (aCount)
		*aCount = own.count;
	return own.found;
}

std::wstring g_probeDir;

std::wstring ProbeFile(const wchar_t *aName)
{
	return g_probeDir + L"\\" + aName;
}

// The heartbeat path must be absolute: A_WorkingDir is the *process* cwd, which
// the first script thread moves to its own script folder, so a relative name
// would land the beats wherever the last script decided - and the gate would be
// measuring the wrong file.
std::wstring HeartbeatPath(const char *aSuffix)
{
	return g_cwd + L"\\heartbeat-" + WideOfPath(std::string(aSuffix));
}

// Reads one heartbeat sample for the "grows / stops" checks. The name is the
// suffix only: long.ahk gets "heartbeat-<suffix>" as its first argument.
struct Beats
{
	size_t lines = 0;
	size_t bytes = 0;
	std::string text;
};

Beats SampleBeats(const char *aSuffix)
{
	Beats out;
	out.text = ReadAll(std::string("heartbeat-") + aSuffix);
	out.lines = CountLines(out.text);
	out.bytes = out.text.size();
	return out;
}

} // namespace

int main()
{
	// CommandLineToArgvW, not main(int, wchar_t**): the MSVC CRT hands main()
	// narrow argv, so declaring a wide second parameter reinterprets every char*
	// as wchar_t* - the probe path arrives as garbage and every script "cannot
	// be opened". Same entry the host itself uses.
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv || argc < 2)
	{
		std::printf("[ERROR] usage: session_gate <probeDir>\n");
		return 2;
	}
	g_probeDir = argv[1];
	LocalFree(argv);

	wchar_t cwd[MAX_PATH] = {0};
	DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwd);
	if (cwdLen > 0 && cwdLen < MAX_PATH)
		g_cwd = cwd;

	Engine &engine = Engine::Instance();
	std::string error;
	if (!engine.Load(error))
	{
		std::printf("[ERROR] engine: %s\n", error.c_str());
		return 1;
	}

	// Preflight: if the corpus is not on disk, every scenario below would "pass"
	// on synthetic file-open errors - that is not evidence, so stop here.
	const wchar_t *needed[] = { L"long.ahk", L"boom.ahk", L"exit3.ahk", L"bad_syntax.ahk",
		L"timer_boom.ahk", L"tray_a.ahk", L"tray_b.ahk", L"tray_empty.ahk" };
	size_t found = 0;
	for (const wchar_t *name : needed)
		if (GetFileAttributesW(ProbeFile(name).c_str()) != INVALID_FILE_ATTRIBUTES)
			++found;
	if (found != sizeof(needed) / sizeof(needed[0])
		|| GetFileAttributesW(ProbeFile(L"definitely-not-here.ahk").c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		std::printf("[ERROR] probe corpus wrong: %zu of %zu files present"
			" (and the unreadable one must be absent)\n",
			found, sizeof(needed) / sizeof(needed[0]));
		return 2;
	}

	// -----------------------------------------------------------------------
	// A: three sessions at once
	// -----------------------------------------------------------------------
	DWORD sLong = StartOne(engine, ProbeFile(L"long.ahk"), { HeartbeatPath("a.log") });
	DWORD sBoom = StartOne(engine, ProbeFile(L"boom.ahk"), {});
	DWORD sExit = StartOne(engine, ProbeFile(L"exit3.ahk"), {});

	Check(sLong && sBoom && sExit && sLong != sBoom && sBoom != sExit && sLong != sExit,
		"A1 three sessions, three distinct ids");
	Session longA, boomA, exitA;
	engine.GetSession(sLong, longA);
	engine.GetSession(sBoom, boomA);
	engine.GetSession(sExit, exitA);
	Check(longA.threadId && boomA.threadId && exitA.threadId
		&& longA.threadId != boomA.threadId && boomA.threadId != exitA.threadId
		&& longA.threadId != exitA.threadId,
		"A2 three distinct kernel thread ids");

	Collector colA;
	bool reached = RunUntil(engine, colA, 10000, [&] {
		return SessionFinished(engine, sBoom) && SessionFinished(engine, sExit);
	});
	Check(reached, "A3 boom and ExitApp 3 both finished within 10 s");

	engine.GetSession(sLong, longA);
	Check(longA.state == SessionState::Running,
		"A4 the resident script was still running when its neighbours ended");
	Check(ThreadAlive(longA.threadId),
		"A4b and its kernel thread is really alive (no neighbour tore it down)");

	std::vector<ErrorRecord> boomRecs = colA.of(sBoom);
	Check(boomRecs.size() == 1, "A5 exactly one record landed on boom's session");
	if (boomRecs.size() == 1)
	{
		const ErrorRecord &r = boomRecs[0];
		Check(r.message == "m011-boom", "A6 boom's message survived routing");
		Check(r.phase == "runtime" && r.severity == "fail", "A7 boom kept its runtime/fail class");
		Check(r.thread == (int)boomA.threadId, "A8 boom's record carries boom's own thread id");
		Check(Contains(r.json, "\"session\":" + std::to_string(sBoom)),
			"A9 the JSONL line carries \"session\" for the journal");
	}
	Check(colA.of(sLong).empty() && colA.of(sExit).empty(),
		"A10 neither the resident nor the clean script borrowed boom's error");
	bool anyUnattributed = false;
	for (const auto &r : colA.all)
		if (!r.session)
			anyUnattributed = true;
	Check(!anyUnattributed, "A11 every record in this run belongs to a session");

	engine.GetSession(sBoom, boomA);
	engine.GetSession(sExit, exitA);
	Check(EffectiveRc(boomA) == 2, "A12 boom exits with the official fatal code 2");
	Check(EffectiveRc(exitA) == 3, "A13 ExitApp 3 reaches the host exit code unchanged");

	// The timer ticks every 200 ms; give it a window before reading.
	Sleep(500);
	Beats early = SampleBeats("a.log");
	Check(early.lines >= 1, "A14 the resident script is really ticking");
	std::string beatThread;
	Check(HeartbeatThread(early.text, beatThread) && beatThread == std::to_string(longA.threadId),
		"A15 the heartbeat writer IS the session's kernel thread");
	Sleep(600);
	Beats mid = SampleBeats("a.log");
	Check(mid.lines > early.lines, "A16 the heartbeat keeps growing while the session lives");

	size_t signalled = engine.StopAll();
	// Capture immediately: a failure that does not name itself gets argued about.
	// 1412 (ERROR_INVALID_THREAD_ID) means the target has no message queue yet -
	// i.e. the script thread had not finished creating its window, which is a
	// timing story, not a routing story. Anything else is a different bug.
	DWORD stopErr = GetLastError();
	Check(signalled >= 1, "A17 StopAll signalled the live session");
	if (signalled == 0)
		std::printf("        PostThreadMessage failed, last error = %lu\n", (unsigned long)stopErr);
	bool stopped = RunUntil(engine, colA, 5000, [&] { return AllFinished(engine); });
	Check(stopped, "A18 StopAll finished every session within 5 s");
	Sleep(400);
	Beats afterStop = SampleBeats("a.log");
	Check(afterStop.bytes >= mid.bytes, "A19 the last beats were written before the stop took effect");
	Sleep(400);
	Beats afterStop2 = SampleBeats("a.log");
	Check(afterStop2.bytes == afterStop.bytes, "A20 nothing writes to the heartbeat after StopAll");

	// JSONL exclusivity: a session's file may only ever hold its own records.
	struct PerFile
	{
		DWORD id;
		const char *what;
		size_t wantLines;
	};
	const PerFile files[] = { { sLong, "long", 0 }, { sBoom, "boom", 1 }, { sExit, "exit3", 0 } };
	bool exclusive = true;
	for (const PerFile &pf : files)
	{
		Session s;
		engine.GetSession(pf.id, s);
		std::string text = ReadAll(LogFullOf(s.logPath));
		if (CountLines(text) != pf.wantLines)
		{
			std::printf("        %s: %zu line(s) in %s, expected %zu\n",
				pf.what, CountLines(text), s.logPath.c_str(), pf.wantLines);
			exclusive = false;
		}
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			pos = eol == std::string::npos ? text.size() : eol + 1;
			if (line.empty())
				continue;
			if (!Contains(line, "\"session\":" + std::to_string(pf.id))
				|| !Contains(line, "\"thread\":" + std::to_string(s.threadId)))
			{
				std::printf("        %s: foreign line in %s\n", pf.what, s.logPath.c_str());
				exclusive = false;
			}
		}
	}
	Check(exclusive, "A21 each session's JSONL holds only its own session and thread");

	// -----------------------------------------------------------------------
	// B: a script that never parses (no thread id exists to route on)
	// -----------------------------------------------------------------------
	Collector colB;
	DWORD sBad = StartOne(engine, ProbeFile(L"bad_syntax.ahk"), {});
	bool badDone = RunUntil(engine, colB, 5000, [&] { return SessionFinished(engine, sBad); });
	Session bad;
	engine.GetSession(sBad, bad);
	Check(badDone, "B1 the unparsable script finished on its own");
	Check(bad.threadId == 0, "B2 it never owned a kernel thread");
	Check(bad.rc == 2, "B3 load failure reports the official code 2");
	std::vector<ErrorRecord> badRecs = colB.of(sBad);
	Check(badRecs.size() == 1 && badRecs[0].phase == "load" && badRecs[0].severity == "fail",
		"B4 its load error was attributed without a thread id");
	Check(Contains(ReadAll(LogFullOf(bad.logPath)), "\"session\":" + std::to_string(sBad)),
		"B5 the record is in its own log, not a neighbour's");

	// -----------------------------------------------------------------------
	// C: a script file that cannot be read at all
	// -----------------------------------------------------------------------
	Collector colC;
	DWORD sMissing = StartOne(engine, ProbeFile(L"definitely-not-here.ahk"), {});
	Drain(engine, colC);
	Session missing;
	bool haveMissing = engine.GetSession(sMissing, missing) && missing.state == SessionState::Finished;
	Check(haveMissing, "C1 the unreadable script got a finished session");
	std::vector<ErrorRecord> missingRecs = colC.of(sMissing);
	Check(missingRecs.size() == 1 && Contains(missingRecs[0].message, "could not be opened"),
		"C2 the host-recorded reason reached that session");
	Check(Contains(ReadAll(LogFullOf(missing.logPath)), "\"session\":" + std::to_string(sMissing)),
		"C3 and into its own log file");

	// -----------------------------------------------------------------------
	// D: the same script alone, then beside a resident one
	// -----------------------------------------------------------------------
	Collector colD;
	DWORD sBoomAlone = StartOne(engine, ProbeFile(L"boom.ahk"), {});
	RunUntil(engine, colD, 5000, [&] { return SessionFinished(engine, sBoomAlone); });

	DWORD sLong2 = StartOne(engine, ProbeFile(L"long.ahk"), { HeartbeatPath("d.log") });
	DWORD sBoomWith = StartOne(engine, ProbeFile(L"boom.ahk"), {});
	RunUntil(engine, colD, 10000, [&] { return SessionFinished(engine, sBoomWith); });
	engine.StopSession(sLong2);
	RunUntil(engine, colD, 5000, [&] { return AllFinished(engine); });

	std::vector<ErrorRecord> alone = colD.of(sBoomAlone);
	std::vector<ErrorRecord> with = colD.of(sBoomWith);
	Check(alone.size() == 1 && with.size() == 1, "D1 both boom runs reported exactly one record");
	bool sameText = alone.size() == 1 && with.size() == 1
		&& alone[0].message == with[0].message
		&& alone[0].file == with[0].file
		&& alone[0].line == with[0].line
		&& alone[0].severity == with[0].severity;
	Check(sameText, "D2 same message/file/line alone and concurrently");
	Session sa, sw;
	engine.GetSession(sBoomAlone, sa);
	engine.GetSession(sBoomWith, sw);
	Check(EffectiveRc(sa) == EffectiveRc(sw), "D3 and the same exit code");
	Check(alone.size() == 1 && with.size() == 1 && alone[0].thread != with[0].thread,
		"D4 the two runs really were two different threads");

	// -----------------------------------------------------------------------
	// E: an error raised from a timer callback
	// -----------------------------------------------------------------------
	Collector colE;
	DWORD sTimer = StartOne(engine, ProbeFile(L"timer_boom.ahk"), {});
	RunUntil(engine, colE, 10000, [&] { return SessionFinished(engine, sTimer); });
	Session timerSession;
	engine.GetSession(sTimer, timerSession);
	std::vector<ErrorRecord> timerRecs = colE.of(sTimer);
	Check(!timerRecs.empty() && Contains(timerRecs[0].message, "m011-timer-boom"),
		"E1 the timer error landed on its own session");
	Check(!timerRecs.empty() && timerRecs[0].thread == (int)timerSession.threadId,
		"E2 a timer tick shares the script thread, so thread routing stays exact");
	Check(SessionFinished(engine, sTimer), "E3 the fatal record ended the session (official Exit, no box)");
	// m011 measured the pre-fix shape of this: a repeating timer re-raising the
	// same failure 70 times in 10 s (17 KB) because only the dialog vanished.
	Check(timerRecs.size() <= 3, "E4 no error storm: a fatal record stops the session, not one thread");
	Check(EffectiveRc(timerSession) == 2, "E5 and the session ends with the fatal code");

	// F: pulling a script's own tray menu out of the host (m024)
	// -----------------------------------------------------------------------
	// The deck posts the kernel's OWN notify-icon message (AHK_NOTIFYICON =
	// WM_USER+4, lParam = WM_RBUTTONUP); MainWindowProc answers it by calling
	// mTrayMenu->Display() with no coordinates, so the menu lands at the pointer
	// whatever the script's Menu coord mode says. That last clause is the point:
	// the first version handed screen coordinates to A_TrayMenu.Show(x, y), and the
	// kernel interprets those by g.CoordMode, whose default is *Client*
	// (defines.h:1027, COORD_MODE_CLIENT = 0) - measured offset (428,331) with a
	// window at (420,300). That is the bug the user reported.
	Collector colF;
	DWORD sTrayA = StartOne(engine, ProbeFile(L"tray_a.ahk"), {});
	DWORD sTrayB = StartOne(engine, ProbeFile(L"tray_b.ahk"), {});
	Check(sTrayA && sTrayB, "F1 two resident sessions started");

	HWND windowA = nullptr;
	HWND windowB = nullptr;
	bool windowsUp = RunUntil(engine, colF, 10000, [&] {
		windowA = engine.FindScriptWindow(sTrayA);
		windowB = engine.FindScriptWindow(sTrayB);
		return windowA != nullptr && windowB != nullptr;
	});
	Check(windowsUp, "F2 each running session has a main window the host can name");
	Session sessionA, sessionB;
	engine.GetSession(sTrayA, sessionA);
	engine.GetSession(sTrayB, sessionB);
	Check(windowA != windowB, "F3 and the two windows are not the same window");
	DWORD ownerProcess = 0;
	const DWORD ownerThread = GetWindowThreadProcessId(windowA, &ownerProcess);
	Check(ownerThread == sessionA.threadId && ownerProcess == GetCurrentProcessId(),
		"F4 that window belongs to this session's kernel thread, in this process");

	// Pin the pointer away from the screen edges first: TrackPopupMenu flips
	// the menu when it does not fit, and a user moving the mouse during ctest
	// made this cell read a stale anchor (dx -96 on a 1920-wide desktop).
	SetCursorPos(400, 300);
	Sleep(60);
	POINT cursorBefore = {};
	GetCursorPos(&cursorBefore);
	PostMessageW(windowA, 0x0404 /*AHK_NOTIFYICON*/, 1, WM_RBUTTONUP);
	HWND menuA = nullptr;
	bool menuUp = RunUntil(engine, colF, 5000, [&] {
		menuA = FindMenuOfThread(sessionA.threadId);
		return menuA != nullptr;
	});
	Check(menuUp, "F5 the kernel popped that session's own tray menu, with no script-side help");
	const DWORD menuThread = menuA ? GetWindowThreadProcessId(menuA, nullptr) : 0;
	Check(menuThread == sessionA.threadId,
		"F6 the menu runs on that session's kernel thread, not the host's");
	RECT menuRect = {};
	if (menuA)
		GetWindowRect(menuA, &menuRect);
	// The user's complaint is exactly this pair of numbers: the menu must be
	// anchored at the pointer, never at some window's origin. A fixture that sits
	// at (0,0) cannot tell the two apart, and a pointer near the bottom edge makes
	// the menu flip above the point (TrackPopupMenu does that on its own), so the
	// anchor is either the menu's top-left or its bottom-left.
	const int dx = menuRect.left - cursorBefore.x;
	const int dyTop = menuRect.top - cursorBefore.y;
	const int dyBottom = menuRect.bottom - cursorBefore.y;
	const bool anchored = dx >= -2 && dx <= 8 && ((dyTop >= -2 && dyTop <= 8) || (dyBottom >= -2 && dyBottom <= 8));
	Check(anchored,
		("F7 the menu is anchored at the pointer: cursor " + std::to_string(cursorBefore.x) + ","
			+ std::to_string(cursorBefore.y) + " -> menu rect " + std::to_string(menuRect.left) + ","
			+ std::to_string(menuRect.top) + ".." + std::to_string(menuRect.right) + ","
			+ std::to_string(menuRect.bottom) + " (dx " + std::to_string(dx) + ", dyTop "
			+ std::to_string(dyTop) + ", dyBottom " + std::to_string(dyBottom)
			+ (dyBottom >= -2 && dyBottom <= 8 ? ", flipped above" : "") + ")").c_str());

	int menusWhileAOpen = 0;
	FindMenuOfThread(0, &menusWhileAOpen);
	Check(menusWhileAOpen == 1,
		"F8 the neighbour session popped nothing (one menu window in the process, not two)");

	// A menu is tracked in a modal loop owned by the script thread, and
	// WM_CANCELMODE posted from another process does not close it (measured). The
	// documented way is EndMenu() from a window on that same thread, so the gate
	// asks the session itself - the same channel any host-side "close it" action
	// would have to use.
	PostMessageW(windowA, 0x8003 /*deck: dismiss my menu*/, 0, 0);
	bool menuGone = RunUntil(engine, colF, 5000, [&] {
		return !IsWindow(menuA) || !IsWindowVisible(menuA);
	});
	Check(menuGone, "F9 the session can dismiss its own menu (EndMenu on its own thread)");

	PostMessageW(windowB, 0x0404, 1, WM_RBUTTONUP);
	HWND menuB = nullptr;
	bool secondMenu = RunUntil(engine, colF, 5000, [&] {
		menuB = FindMenuOfThread(sessionB.threadId);
		return menuB != nullptr && menuB != menuA;
	});
	Check(secondMenu, "F10 the second session answers for itself, with its own menu window");
	PostMessageW(windowB, 0x8003, 0, 0);
	RunUntil(engine, colF, 3000, [&] { return FindMenuOfThread(0) == nullptr; });

	engine.StopSession(sTrayA);
	RunUntil(engine, colF, 8000, [&] { return SessionFinished(engine, sTrayA); });
	Check(engine.FindScriptWindow(sTrayA) == nullptr,
		"F11 a finished session has no window to post to (a recycled thread id must not be followed)");
	Check(engine.FindScriptWindow(999999) == nullptr, "F12 an unknown session id yields no window");
	engine.StopSession(sTrayB);
	RunUntil(engine, colF, 8000, [&] { return AllFinished(engine); });

	// -----------------------------------------------------------------------
	// G: the menu-less script and the predicate the deck checks before popping
	// it (m160). DeckUI::OpenScriptMenu asks mEngine.TrayMenu + GetMenuItemCount
	// before deciding between "post AHK_NOTIFYICON" and "front the window"; the
	// emptied-menu script is exactly the m158 case (a window-type script that
	// did A_TrayMenu.Delete() and adds nothing back). Proving the count here -
	// with real kernel sessions - is what the one-line UI branch turns on; the
	// desktop-side tray click that used to pop the empty menu is deliberately
	// not automated: the keyboard recipe over the shell tray is fragile (see
	// test/ui_gate/README), and the count IS its whole decision.
	// -----------------------------------------------------------------------
	Collector colG;
	DWORD sNoMenu = StartOne(engine, ProbeFile(L"tray_empty.ahk"), {});
	DWORD sWithMenu = StartOne(engine, ProbeFile(L"tray_b.ahk"), {});
	Check(sNoMenu && sWithMenu, "G1 the menu-less script and the menu'd control started");
	bool gWindowsUp = RunUntil(engine, colG, 10000, [&] {
		return engine.FindScriptWindow(sNoMenu) && engine.FindScriptWindow(sWithMenu);
	});
	Check(gWindowsUp, "G2 both have windows (front-target exists for the fallback branch)");
	HMENU emptyMenu = nullptr;
	HMENU fullMenu = nullptr;
	bool menusReady = RunUntil(engine, colG, 5000, [&] {
		emptyMenu = engine.TrayMenu(sNoMenu);
		fullMenu = engine.TrayMenu(sWithMenu);
		return emptyMenu != nullptr && fullMenu != nullptr;
	});
	Check(menusReady, "G3 the kernel hands out the tray menu even after A_TrayMenu.Delete()");
	Check(emptyMenu && GetMenuItemCount(emptyMenu) == 0,
		"G4 the menu-less script's count is 0 - the exact number OpenScriptMenu tests now");
	Check(fullMenu && GetMenuItemCount(fullMenu) >= 1,
		"G5 the control's count is >= 1, so the guard never fires on a real menu");
	Check(engine.TrayMenu(999999) == nullptr,
		"G6 an unknown session id reports no menu (the guard must not deref it)");
	engine.StopSession(sNoMenu);
	engine.StopSession(sWithMenu);
	RunUntil(engine, colG, 8000, [&] { return SessionFinished(engine, sNoMenu) && SessionFinished(engine, sWithMenu); });

	std::printf("\n%s: %d failure(s)\n", g_failures ? "FAIL" : "OK", g_failures);
	std::fflush(stdout);
	engine.Unload();
	return g_failures ? 1 : 0;
}

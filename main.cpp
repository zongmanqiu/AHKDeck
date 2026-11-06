// AHKDeck - in-process AHK v2 script deck.
//
// Entries:
//   * "AHKDeck.exe --version"  -> console line, exit 0 (scripted smoke probe,
//     used by 5_test and by scripts);
//   * "AHKDeck.exe --quit [pid]" -> tells running deck windows to stop their
//     sessions and exit; a pid targets one instance (the UI gates pass theirs
//     so they cannot kill a user's deck). Without pid it targets every deck
//     (m023: the close box now hides to the tray);
//   * "AHKDeck.exe [--journal] --run <script> [args...]" -> runs a real .ahk
//     file inside this process (no second task-manager entry) and streams every
//     error / warning to this console plus the session's
//     logs/<script>-<id>-<timestamp>.jsonl. The process exit code is the
//     script's own official exit code (load failure = 2, just like
//     AutoHotkey.exe). --journal replaces the human layout with one JSON object
//     per line (started / kernel records / exited) for machines: AI agents and
//     scripts tail the stream instead of parsing prose.
//   * "AHKDeck.exe --mcp-stdio" -> the same deck window, plus an MCP endpoint on
//     stdin/stdout (m064): newline-delimited JSON-RPC exposing the script tools,
//     so an external agent can write into the editing area, run it, read the
//     console and iterate - with the scripts still living in this one process.
//   * bare double-click launch -> the FLTK deck window (m014): script list,
//     run / stop / stop-all, the console panel and the session state column.
//     No terminal window is ever created (rules-standing.md 13).
//
// Several scripts at once is an engine capability (m011) and the UI drives it;
// this CLI stays one script per invocation so "--run a.ahk --run b.ahk" never
// has to guess where the script arguments end.
//
// License: GPL-2.0-or-later (our own files), see LICENSE and
// THIRD-PARTY-NOTICES.md for the embedded third-party components.

#include "core/Version.h"
#include "engine/DeckEngine.h"
#include "ui/DeckUI.h"
#include "ui/NativeHooks.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::wstring ExeDirectory()
{
	wchar_t path[MAX_PATH] = {0};
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::wstring();
	std::wstring dir(path, n);
	const size_t slash = dir.find_last_of(L"\\/");
	return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

LONG __stdcall CrashFilter(EXCEPTION_POINTERS *aInfo)
{
	const std::wstring logs = ExeDirectory() + L"\\Logs";
	const std::wstring dumps = logs + L"\\CrashDumps";
	CreateDirectoryW(logs.c_str(), nullptr);
	CreateDirectoryW(dumps.c_str(), nullptr);

	SYSTEMTIME st = {};
	GetLocalTime(&st);
	wchar_t stamp[48] = {0};
	swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%lu",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		GetCurrentProcessId());

	const std::wstring base = dumps + L"\\crash-" + stamp;
	if (aInfo && aInfo->ExceptionRecord)
	{
		const EXCEPTION_RECORD *rec = aInfo->ExceptionRecord;
		const HANDLE file = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			MINIDUMP_EXCEPTION_INFORMATION info = {};
			info.ThreadId = GetCurrentThreadId();
			info.ExceptionPointers = aInfo;
			info.ClientPointers = FALSE;
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
				(MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
					MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
				&info, nullptr, nullptr);
			CloseHandle(file);
		}

		const HANDLE txt = CreateFileW((base + L".txt").c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (txt != INVALID_HANDLE_VALUE)
		{
			char line[512] = {0};
			const int n = std::snprintf(line, sizeof(line),
				"AHKDeck crash\r\npid: %lu\r\nthread: %lu\r\ncode: 0x%08lX\r\naddress: 0x%p\r\nflags: 0x%08lX\r\n",
				GetCurrentProcessId(), GetCurrentThreadId(),
				(unsigned long)rec->ExceptionCode, rec->ExceptionAddress,
				(unsigned long)rec->ExceptionFlags);
			DWORD written = 0;
			WriteFile(txt, line, (DWORD)(n > 0 ? n : 0), &written, nullptr);
			CloseHandle(txt);
		}
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashHandler()
{
	SetUnhandledExceptionFilter(CrashFilter);
	std::set_terminate([] {
		RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	});
}

// The exe links as /SUBSYSTEM:WINDOWS (double-click must not open a console -
// rules-standing.md 13), which means a CLI run has no console either. Attach to
// the parent's when there is one and point stdio at CONOUT$; when there is none
// (started from Explorer), stdout is a dead handle and the CRT discards the
// writes - which is why the print sites are not gated: the script still runs and
// every record still reaches the session JSONL, only the console text is lost.
void AttachConsoleForCli()
{
	// If a parent already redirected us (a pipe from ctest, a file from a script),
	// the CRT's stdout already points where that reader looks. Grabbing CONOUT$ in
	// that case would blind the very harness that launched us - the journal gate
	// reads a pipe, so this check is not theoretical.
	HANDLE outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	const DWORD fileType = outHandle ? GetFileType(outHandle) : FILE_TYPE_UNKNOWN;
	if (fileType == FILE_TYPE_PIPE || fileType == FILE_TYPE_DISK)
		return;

	if (GetConsoleWindow() != nullptr)
		return; // already attached (started from a console the CRT knew about)
	if (!AttachConsole(ATTACH_PARENT_PROCESS))
		return; // no parent console: keep running headless, the JSONL is the record
	// freopen, scoped-off for these two lines only: the _s form is not reachable
	// through <cstdio> (it eats the <stdio.h> include guard), and redirecting an
	// existing stdout FILE onto CONOUT$ is precisely what freopen is for.
#pragma warning(push)
#pragma warning(disable : 4996)
	if (!std::freopen("CONOUT$", "w", stdout))
		return;
	(void)std::freopen("CONOUT$", "w", stderr);
#pragma warning(pop)
}

std::string BaseName(const std::wstring &aPath)
{
	size_t p = aPath.find_last_of(L"\\/");
	std::wstring leaf = p == std::wstring::npos ? aPath : aPath.substr(p + 1);
	if (leaf.empty())
		return std::string("?");
	int n = WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), (int)leaf.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), (int)leaf.size(), out.data(), n, nullptr, nullptr);
	return out;
}

// Decode an exit code the way m004's pair gate does: crash severities as
// 0xNNNNNNNN, everything else decimal, so the console never hides a fastfail.
std::string FormatExitCode(DWORD aCode)
{
	if (aCode >= 0xC0000000u)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08X", aCode);
		return buf;
	}
	return std::to_string(aCode);
}

std::string LocalTimestamp()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

// The journal's own events are hand-written JSON, so every string goes through
// here (the kernel escapes its own records).
std::string JsonString(const std::string &aUtf8)
{
	std::string out = "\"";
	for (char c : aUtf8)
	{
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += c;
		}
		else if ((unsigned char)c < 0x20)
		{
			static const char hex[] = "0123456789abcdef";
			out += "\\u00";
			out += hex[((unsigned char)c) >> 4];
			out += hex[((unsigned char)c) & 0xF];
		}
		else
			out += c;
	}
	out += "\"";
	return out;
}

void JournalEvent(const char *aName, DWORD aSession, const char *aExtra)
{
	std::printf("{\"event\":\"%s\",\"session\":%lu,\"ts\":%s%s}\n",
		aName, (unsigned long)aSession, JsonString(LocalTimestamp()).c_str(), aExtra);
	std::fflush(stdout);
}

void PrintRecord(const ahkdeck::ErrorRecord &aRec)
{
	std::printf("%s  %-8s %-7s %s:%d  %s\n",
		aRec.ts.c_str(),
		aRec.severity.empty() ? aRec.kind.c_str() : aRec.severity.c_str(),
		aRec.phase.c_str(),
		aRec.file.empty() ? "?" : aRec.file.c_str(),
		aRec.line,
		aRec.message.c_str());
	if (!aRec.extra.empty())
		std::printf("                           Specifically: %s\n", aRec.extra.c_str());
	std::fflush(stdout);
}

int RunScript(bool aJournal, int aArgc, wchar_t **aArgv, int aRunIndex)
{
	if (aArgc < aRunIndex + 2)
	{
		if (aJournal)
			std::printf("{\"event\":\"error\",\"message\":%s}\n",
				JsonString("usage: AHKDeck.exe --run <script.ahk> [args...]").c_str());
		else
			std::printf("usage: AHKDeck.exe --run <script.ahk> [args...]\n");
		return 2;
	}

	std::wstring scriptPath = aArgv[aRunIndex + 1];
	std::vector<std::wstring> scriptArgs(aArgv + aRunIndex + 2, aArgv + aArgc);

	ahkdeck::Engine &engine = ahkdeck::Engine::Instance();
	std::string error;
	if (!engine.Load(error))
	{
		if (aJournal)
			std::printf("{\"event\":\"error\",\"message\":%s}\n", JsonString(error).c_str());
		else
			std::printf("[ERROR] engine: %s\n", error.c_str());
		return 1;
	}

	DWORD sessionId = engine.StartSession(scriptPath, scriptArgs, error);
	if (!sessionId)
	{
		if (aJournal)
			std::printf("{\"event\":\"error\",\"message\":%s}\n", JsonString(error).c_str());
		else
			std::printf("[ERROR] engine: %s\n", error.c_str());
		return 1;
	}

	ahkdeck::Session session;
	engine.GetSession(sessionId, session);
	if (!error.empty())
	{
		// The script runs, only its log is missing - say so, once, in the
		// consumer's own format.
		if (aJournal)
			std::printf("{\"event\":\"log_error\",\"session\":%lu,\"message\":%s}\n",
				(unsigned long)sessionId, JsonString(error).c_str());
		else
			std::printf("[WARN] engine: %s\n", error.c_str());
	}

	if (aJournal)
	{
		JournalEvent("started", sessionId,
			(",\"script\":" + JsonString(BaseName(scriptPath)) + ",\"log\":"
				+ JsonString(session.logPath)).c_str());
	}
	else
	{
		std::printf("script  : %s\n", BaseName(scriptPath).c_str());
		std::printf("session : %lu\n", (unsigned long)sessionId);
		std::printf("log     : %s\n\n", session.logPath.c_str());
		std::fflush(stdout);
	}

	int count = 0;
	DWORD rc = 0;
	ahkdeck::ErrorRecord rec;
	for (;;)
	{
		if (engine.PopRecord(rec, 10))
		{
			if (aJournal)
				std::printf("%s\n", rec.json.c_str());
			else
				PrintRecord(rec);
			++count;
			continue;
		}
		if (engine.WaitSession(sessionId, 0, &rc))
		{
			// Drain whatever is still queued, then report the official exit code.
			while (engine.PopRecord(rec, 0))
			{
				if (aJournal)
					std::printf("%s\n", rec.json.c_str());
				else
					PrintRecord(rec);
				++count;
			}
			break;
		}
		Sleep(10);
	}

	engine.GetSession(sessionId, session);
	// A "fail" record on this session is exactly the case where the official
	// dialog's "Exit" gives the process exit code 2; the thread unwinds with 0.
	if (rc == 0 && session.fatal)
		rc = 2;

	if (aJournal)
	{
		JournalEvent("exited", sessionId, (",\"rc\":" + std::to_string((unsigned long)rc)).c_str());
	}
	else
	{
		if (count == 0)
			std::printf("\n(no console output)\n");
		std::printf("\nexit code: %s\n", FormatExitCode(rc).c_str());
	}
	std::fflush(stdout);

	engine.Unload();
	return (int)rc;
}

} // namespace

int main()
{
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return 1;

	InstallCrashHandler();

	bool journal = false;
	bool mcpStdio = false;
	int runIndex = -1;
	for (int i = 1; i < argc; ++i)
	{
		if (std::wcscmp(argv[i], L"--journal") == 0)
			journal = true;
		else if (std::wcscmp(argv[i], L"--mcp-stdio") == 0)
			mcpStdio = true;
		else if (std::wcscmp(argv[i], L"--run") == 0 && runIndex < 0)
			runIndex = i;
	}

	// Any switch means "someone typed this in a console": get that console back.
	// A bare launch (argc == 1) deliberately stays headless - no terminal window.
	// --mcp-stdio is the exception: its stdout is the MCP protocol channel, and
	// grabbing a console would put human text on the wire.
	if (argc > 1 && !mcpStdio)
		AttachConsoleForCli();

	int result;
	if (argc > 1 && std::wcscmp(argv[1], L"--version") == 0)
	{
		std::printf("AHKDeck %s\n", ahkdeck::VersionString());
		result = 0;
	}
	else if (argc > 1 && std::wcscmp(argv[1], L"--quit") == 0)
	{
		// The close box now hides to the tray (m023), so a scripted "really exit"
		// needs a channel of its own: find the running deck's control window and
		// tell it to stop every session and leave.
		const DWORD targetPid = argc > 2 ? (DWORD)wcstoul(argv[2], nullptr, 10) : 0;
		const int signalled = ahkdeck::ui::NativeHooks::PostQuitToRunningDecks(targetPid);
		std::printf(signalled ? "AHKDeck: quit signalled for %d window(s)\n"
			: "AHKDeck: no running deck\n", signalled);
		result = signalled ? 0 : 1;
	}
	else if (mcpStdio)
	{
		// m064: an MCP client (WorkBuddy / Qoder) starts this process and speaks
		// newline-delimited JSON-RPC on stdin/stdout. This is the deck window with
		// one extra endpoint, not a proxy: the editing area, the sessions and the
		// console all belong to this process, so the client's tool calls move the
		// window the user is watching, and when the client lets go the deck dies
		// with every script inside it (AGENTS 11a).
		//
		// The single-instance handshake below is deliberately skipped: the client
		// needs this very process to stay alive as its server.
		result = ahkdeck::ui::Run(GetModuleHandleW(nullptr), true);
	}
	else if (runIndex > 0)
	{
		result = RunScript(journal, argc, argv, runIndex);
	}
	else if (argc > 1)
	{
		std::printf("usage: AHKDeck.exe [--version | --quit [pid] | --mcp-stdio | [--journal] --run <script.ahk> [args...]]\n");
		result = 0;
	}
	else
	{
		// Bare launch: the window is the product. One deck per session; a second
		// double-click wakes the existing instance instead of starting a rival
		// tray icon / hotkey table. CLI modes (--run, --quit, --version) stay
		// independent so the gates and scripts can still drive a running deck.
		HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\AHKDeck.SingleInstance");
		if (single && GetLastError() == ERROR_ALREADY_EXISTS)
		{
			int shown = 0;
			for (int attempt = 0; attempt < 12 && shown == 0; ++attempt)
			{
				shown = ahkdeck::ui::NativeHooks::PostShowToRunningDecks();
				if (shown == 0)
					Sleep(100);
			}
			CloseHandle(single);
			return shown > 0 ? 0 : 1;
		}
		result = ahkdeck::ui::Run(GetModuleHandleW(nullptr));
		if (single)
			CloseHandle(single);
	}

	LocalFree(argv);
	return result;
}

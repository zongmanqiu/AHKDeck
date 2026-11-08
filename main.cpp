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
//   * "AHKDeck.exe --validate <script.ahk>" -> parses the file with the kernel's
//     own /validate mode and exits before a single statement runs (m103). Output
//     is JSONL: the kernel's load-error records, then {"ok":...,"rc":...}. This
//     only works in a fresh process (the kernel refuses a second script once one
//     is loaded), so the deck's validate_editor tool spawns this mode per call.
//   * "AHKDeck.exe --mcp-stdio" -> the same deck, plus an MCP endpoint on
//     stdin/stdout (m064): newline-delimited JSON-RPC exposing the script tools,
//     so an external agent can write into the editing area, run it, read the
//     console and iterate - with the scripts still living in this one process.
//     m117: the window starts hidden to the tray, so frequent tool calls never
//     put a deck window in front of the user; the tray icon brings it back.
//   * bare double-click launch -> the FLTK deck window (m014): script list,
//     run / stop / stop-all, the console panel and the session state column.
//     No terminal window is ever created (rules-standing.md 13).
//
// Several scripts at once is an engine capability (m011) and the UI drives it;
// this CLI stays one script per invocation so "--run a.ahk --run b.ahk" never
// has to guess where the script arguments end.
//
// License: GPL-2.0 (only), see LICENSE and
// THIRD-PARTY-NOTICES.md for the embedded third-party components.

#include "core/ResourceStats.h"
#include "core/Version.h"
#include "engine/DeckEngine.h"
#include "ui/DeckUI.h"
#include "ui/NativeHooks.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

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

struct ModuleRange
{
	ULONGLONG base = 0;
	ULONGLONG size = 0;
	std::string name;
};

// Loaded modules, for the stack scan below. Toolhelp is the cheap route; if it
// fails (a crashing process is not a healthy one) the stack is written as raw
// addresses and the dmp still carries the module list.
void CollectModules(std::vector<ModuleRange> &aOut)
{
	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
		GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;
	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Module32FirstW(snap, &entry))
	{
		do
		{
			ModuleRange mod;
			mod.base = reinterpret_cast<ULONGLONG>(entry.modBaseAddr);
			mod.size = entry.modBaseSize;
			char leaf[MAX_PATH] = {0};
			WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, leaf, MAX_PATH, nullptr, nullptr);
			mod.name = leaf;
			aOut.push_back(mod);
		} while (Module32NextW(snap, &entry));
	}
	CloseHandle(snap);
}

// "module+0xoffset" when aAddress lands inside a loaded image, else empty.
std::string DescribeAddress(ULONGLONG aAddress, const std::vector<ModuleRange> &aModules)
{
	for (const ModuleRange &mod : aModules)
	{
		if (aAddress >= mod.base && aAddress < mod.base + mod.size)
		{
			char text[160] = {0};
			std::snprintf(text, sizeof(text), "%s+0x%llx", mod.name.c_str(),
				static_cast<unsigned long long>(aAddress - mod.base));
			return text;
		}
	}
	return std::string();
}

// Walk the crashing thread's stack from RSP and report every word that points
// into a loaded image. The dmp has the whole memory but neither WinDbg nor a
// symbol server here, so the crash report itself has to answer "which code
// called the bad pointer": those image pointers are the call chain, and with a
// map/pdb of the same build the offsets turn into function names. The scan is
// bounded to the committed stack region that holds RSP, so it cannot touch the
// guard page and cannot fault a second time.
void AppendStack(std::string &aText, ULONGLONG aRsp,
	const std::vector<ModuleRange> &aModules)
{
	if (aRsp == 0)
		return;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(reinterpret_cast<const void *>(aRsp), &mbi, sizeof(mbi)))
		return;
	const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
		PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
	if ((mbi.Protect & readable) == 0)
		return;
	const ULONGLONG high = reinterpret_cast<ULONGLONG>(mbi.BaseAddress) + mbi.RegionSize;
	const ULONGLONG wanted = aRsp + 0x4000;
	const ULONGLONG stop = high < wanted ? high : wanted;
	int frames = 0;
	for (ULONGLONG at = aRsp; at + sizeof(ULONGLONG) <= stop && frames < 64;
		at += sizeof(ULONGLONG))
	{
		const ULONGLONG value = *reinterpret_cast<const ULONGLONG *>(at);
		const std::string where = DescribeAddress(value, aModules);
		if (where.empty())
			continue;
		char line[192] = {0};
		std::snprintf(line, sizeof(line), "  #%d  %p  %s\r\n", frames,
			reinterpret_cast<const void *>(value), where.c_str());
		aText += line;
		++frames;
	}
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
	bool dumped = false;
	ULONG dumpError = 0;
	ULONGLONG dumpBytes = 0;
	if (aInfo && aInfo->ExceptionRecord)
	{
		const HANDLE file = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			MINIDUMP_EXCEPTION_INFORMATION info = {};
			info.ThreadId = GetCurrentThreadId();
			info.ExceptionPointers = aInfo;
			info.ClientPointers = FALSE;
			dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
				(MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
					MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
				&info, nullptr, nullptr) != FALSE;
			if (!dumped)
				dumpError = GetLastError();
			LARGE_INTEGER size = {};
			if (GetFileSizeEx(file, &size))
				dumpBytes = static_cast<ULONGLONG>(size.QuadPart);
			CloseHandle(file);
		}
		else
		{
			dumpError = GetLastError();
		}
	}

	std::string text;
	text.reserve(4096);
	char line[256] = {0};
	std::snprintf(line, sizeof(line), "AHKDeck crash\r\npid: %lu\r\nthread: %lu\r\n",
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long>(GetCurrentThreadId()));
	text += line;

	const EXCEPTION_RECORD *rec = aInfo ? aInfo->ExceptionRecord : nullptr;
	if (rec)
	{
		std::snprintf(line, sizeof(line), "code: 0x%08lX\r\naddress: %p\r\nflags: 0x%08lX\r\n",
			static_cast<unsigned long>(rec->ExceptionCode), rec->ExceptionAddress,
			static_cast<unsigned long>(rec->ExceptionFlags));
		text += line;
		const DWORD params = rec->NumberParameters;
		std::snprintf(line, sizeof(line), "params: %lu\r\n",
			static_cast<unsigned long>(params));
		text += line;
		for (DWORD i = 0; i < params && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
		{
			// Access-violation information[0] is the fault kind: 0 read, 1 write,
			// 8 execute. An "execute" at address 0 is a call through a null
			// function pointer, which is worth spelling out. information[1] is the
			// address the access targeted, so it gets no kind label.
			const char *kind = "";
			if (rec->ExceptionCode == 0xC0000005UL && i == 0)
				kind = rec->ExceptionInformation[i] == 8 ? " (execute)"
					: rec->ExceptionInformation[i] == 1 ? " (write)" : " (read)";
			std::snprintf(line, sizeof(line), "info[%lu]: 0x%llX%s\r\n",
				static_cast<unsigned long>(i),
				static_cast<unsigned long long>(rec->ExceptionInformation[i]), kind);
			text += line;
		}
	}

	std::snprintf(line, sizeof(line), "dmp: %s (err=%lu, %llu bytes)\r\n",
		dumped ? "ok" : "failed", static_cast<unsigned long>(dumpError),
		static_cast<unsigned long long>(dumpBytes));
	text += line;

	std::vector<ModuleRange> modules;
	modules.reserve(128);
	CollectModules(modules);
	std::snprintf(line, sizeof(line), "modules: %llu\r\nstack:\r\n",
		static_cast<unsigned long long>(modules.size()));
	text += line;

#if defined(_M_X64)
	if (aInfo && aInfo->ContextRecord)
	{
		const CONTEXT *ctx = aInfo->ContextRecord;
		std::snprintf(line, sizeof(line), "rip: %p\r\nrsp: %p\r\n",
			reinterpret_cast<const void *>(ctx->Rip),
			reinterpret_cast<const void *>(ctx->Rsp));
		text += line;
		AppendStack(text, ctx->Rsp, modules);
	}
#endif

	const HANDLE txt = CreateFileW((base + L".txt").c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (txt != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(txt, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		CloseHandle(txt);
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

// m103: validate-only pass. The kernel's "/validate" makes LoadFromFile parse
// the script and return 0 (2 on a load failure) before InitForExecution runs,
// so nothing executes. Output is JSONL - the kernel's load-error records, then
// one summary line - because validate_editor reads it back as tool fields.
int RunValidate(wchar_t *aScriptPath)
{
	ahkdeck::Engine &engine = ahkdeck::Engine::Instance();
	std::string error;
	std::vector<ahkdeck::ErrorRecord> records;
	const int rc = engine.ValidateFile(aScriptPath, error, records);

	for (const auto &rec : records)
		std::printf("%s\n", rec.json.c_str());

	if (rc == 1)
		std::printf("{\"ok\":false,\"rc\":1,\"error\":%s}\n",
			JsonString(error.empty() ? "the kernel could not validate" : error).c_str());
	else
		std::printf("{\"ok\":%s,\"rc\":%d}\n", rc == 0 ? "true" : "false", rc);
	std::fflush(stdout);
	return rc;
}

// m107: one JSON line with the same numbers the status bar draws. Two samples
// 300 ms apart turn the CPU counters into a real delta, and the first GPU
// collect only primes PDH. Nothing here touches the window, so this mode is
// also the machine reader for the stats gate.
//
// pid is in the line on purpose: it lets a reader (or a person with Task
// Manager open) confirm that these numbers belong to THIS process, not to
// something the deck started alongside it.
int RunStats()
{
	ahkdeck::ResourceSampler sampler;
	sampler.Sample();
	Sleep(300);
	const ahkdeck::ResourceSample sample = sampler.Sample();
	std::printf("{\"ok\":true,\"pid\":%lu,\"cpu_percent\":%.1f,\"working_set_bytes\":%llu,"
		"\"private_working_set_bytes\":%llu,\"private_bytes\":%llu,"
		"\"threads\":%u,\"handles\":%u,\"gpu_percent\":%.1f,\"text\":%s}\n",
		(unsigned long)GetCurrentProcessId(),
		sample.cpuPercent,
		(unsigned long long)sample.workingSetBytes,
		(unsigned long long)sample.privateWorkingSetBytes,
		(unsigned long long)sample.privateBytes,
		(unsigned)sample.threadCount, (unsigned)sample.handleCount,
		sample.gpuPercent,
		JsonString(ahkdeck::FormatResources(sample)).c_str());
	std::fflush(stdout);
	return 0;
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
	int validateIndex = -1;
	for (int i = 1; i < argc; ++i)
	{
		if (std::wcscmp(argv[i], L"--journal") == 0)
			journal = true;
		else if (std::wcscmp(argv[i], L"--mcp-stdio") == 0)
			mcpStdio = true;
		else if (std::wcscmp(argv[i], L"--run") == 0 && runIndex < 0)
			runIndex = i;
		else if (std::wcscmp(argv[i], L"--validate") == 0 && validateIndex < 0)
			validateIndex = i;
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
	else if (argc > 1 && std::wcscmp(argv[1], L"--stats") == 0)
	{
		// m107: the machine reader for the status bar's resource numbers.
		result = RunStats();
	}
	else if (mcpStdio)
	{
		// m064: an MCP client (WorkBuddy / Qoder) starts this process and speaks
		// newline-delimited JSON-RPC on stdin/stdout. This is the deck with one
		// extra endpoint, not a proxy: the editing area, the sessions and the
		// console all belong to this process, and when the client lets go the deck
		// dies with every script inside it (AGENTS 11a). m117: the window goes
		// hidden to the tray right after startup, so an agent that calls tools all
		// day never gets a deck window in the way; the tray icon is the way back.
		//
		// The single-instance handshake below is deliberately skipped: the client
		// needs this very process to stay alive as its server.
		result = ahkdeck::ui::Run(GetModuleHandleW(nullptr), true);
	}
	else if (validateIndex > 0 && validateIndex + 1 < argc)
	{
		result = RunValidate(argv[validateIndex + 1]);
	}
	else if (runIndex > 0)
	{
		result = RunScript(journal, argc, argv, runIndex);
	}
	else if (argc > 1)
	{
		std::printf("usage: AHKDeck.exe [--version | --stats | --quit [pid] | --validate <script.ahk> | --mcp-stdio | [--journal] --run <script.ahk> [args...]]\n");
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

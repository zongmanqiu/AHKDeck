// deck engine - out-of-process AutoHotkey host: one session per child process.
//
// m192: the kernel is the official AutoHotkey interpreter built as a plain EXE
// (patches/AutoHotkey-v2.0.31), started as a child of this host. One script =
// one process: closing the host closes every script through the session's job
// object (KILL_ON_JOB_CLOSE), and a script that crashes can no longer take the
// host down with it - the property the in-process kernel never had.
//
// The public surface is the m011 one, unchanged on purpose: the UI, the MCP
// agent and the journal gate all keep speaking the same vocabulary (session id,
// state, rc, one JSONL per session, PopRecord routing). Only the transport
// underneath changed:
//
//   errors/warnings   kernel patch 2 (AHKDECK_ERROR_FILE) appends UTF-8 JSON
//                     lines to a per-session raw file under temp\work; the
//                     engine pumps it, attributes each record to its session
//                     and writes the same JSONL the in-process kernel produced.
//   FileAppend "*"    the child's stdout (STARTF_USESTDHANDLES pipe); a per-
//                     session reader thread wraps each line as a "stdout"
//                     record, the same shape the H kernel's output sink gave.
//   tray menu         kernel patch 4 (AHKDECK_DUMP_MENU): the host asks the
//                     child's main window for a JSON snapshot of the live tray
//                     menu and rebuilds it as a host-owned HMENU, so the deck's
//                     merged tray submenu (m132) survives the process split.
//                     Clicks travel back as WM_COMMAND to the same window -
//                     the path the kernel itself uses for its default item.
//   stop              WM_CLOSE to the child's main window (the kernel's own
//                     close path); TerminateJobObject only when no window is
//                     left to close.
//
// Errors never reach a modal dialog: the patched kernel writes them to the raw
// file instead. Records still funnel through one queue, PopRecord attributes
// and logs them, so no consumer has to redo the routing.
//
// Direction: engine depends on core; core never includes engine.

#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ahkdeck {

// One error / warning line, parsed from the kernel's JSON.
struct ErrorRecord
{
	std::string json;     // kernel line with "session":<id> inserted, verbatim in the JSONL
	std::string ts;       // local time, ISO-ish with milliseconds
	std::string kind;     // "error" | "warn" | "stdout" | "log"
	std::string phase;    // "load" | "runtime"
	std::string severity; // "fail" | "critical" | "fail_or_ok" | "warn"
	std::string file;     // script file name as the official dialog shows it
	std::string message;
	std::string extra;    // "Specifically:" text (may be empty)
	// m175: what the dialog shows but file+line cannot carry. A run copy lives
	// in .temp and a #Include'd frame is another file, so "line 11" alone often
	// does not identify the code the caller has in front of it.
	std::string func;     // function that was running ("" at load time)
	std::string source;   // text of the failing line, no line number
	std::string stack;    // call stack, top frame first, leaf file names; "\r\n" separated
	DWORD session = 0;    // owning session; 0 = could not be attributed
	int thread = 0;       // the child process id in the m192 engine
	int seq = 0;
	int line = 0;
	// The record has been written to its session's JSONL. The log write and the
	// console write belong together, but a tool that wants the log NOW may
	// flush the log itself (FlushPendingLogs) before the UI tick gets there -
	// without this flag the record would land in the file twice (m083).
	bool logged = false;
};

enum class SessionState
{
	Running,  // child process alive
	Stopping, // WM_CLOSE posted, not reaped yet
	Finished  // reaped: rc is valid
};

struct Session
{
	DWORD id = 0;
	std::wstring path;   // script path exactly as StartSession was given it
	std::string name;    // base name, UTF-8 (console + journal label)
	std::string logPath; // relative temp\logs\<name>-<id>-<timestamp>.jsonl (m177)
	DWORD threadId = 0;  // m192: the child process id; 0 when it never started
	SessionState state = SessionState::Running;
	DWORD rc = 0;        // official exit code, valid once Finished
	bool fatal = false;  // a "fail" record landed on this session
};

class Engine
{
public:
	static Engine &Instance();

	// Locates the kernel executable (AHKDECK_KERNEL override, else next to the
	// host). On failure aError explains why (relative paths only).
	bool Load(std::string &aError);
	void Unload();
	bool IsLoaded() const { return !mKernelPath.empty(); }

	// The absolute log root pinned by Load() (m011). The UI opens this exact
	// directory instead of rebuilding a relative "logs" path: a running script
	// changes the process cwd, and a relative path would resolve elsewhere.
	// Empty until Load() succeeds.
	const std::wstring &LogRoot() const { return mLogRoot; }

	// Starts one script as one child process and returns its id (> 0). A script
	// that cannot even be opened still gets a session, finished with the
	// official load-failure code 2, so its error record has a home. aError stays
	// non-empty while the session runs only if its JSONL could not be created.
	DWORD StartSession(const std::wstring &aScriptPath, const std::vector<std::wstring> &aArgs,
		std::string &aError);

	// Validates one script file with the kernel's own /validate switch:
	// LoadFromFile parses and returns before a single statement executes.
	// m192: the kernel is a plain EXE again, so validate is one short-lived
	// child (no fresh-process constraint, no COM Main re-entry). The child runs
	// with AHKDECK_ERROR_FILE set, so load errors come back as the same JSON
	// records a session would produce. Returns the kernel exit code
	// (0 = parses, 2 = load failure, 1 = no kernel);
	// aRecords receives the load errors without log routing (there is no session).
	int ValidateFile(const std::wstring &aScriptPath, std::string &aError,
		std::vector<ErrorRecord> &aRecords);

	// Snapshot copies - handles and the kernel stay private. Finished sessions
	// stay in the table: the UI list and the journal both need their rc.
	bool GetSession(DWORD aSessionId, Session &aOut) const;
	std::vector<Session> Sessions() const;
	static const char *StateName(SessionState aState);

	// The session's main window (class "AutoHotkey", process = the child), or
	// nullptr when there is none. That window is what the kernel dispatches
	// OnMessage() through, so the deck's menu bridge (m023) is posted here -
	// and the tray bridge's WM_COMMAND clicks land on exactly the same path.
	HWND FindScriptWindow(DWORD aSessionId) const;

	// m132: the session's live tray menu as a HOST-OWNED HMENU, rebuilt from the
	// JSON snapshot kernel patch 4 writes on demand. The engine keeps exactly
	// one menu per session and destroys it on the next request or when the
	// session goes away. nullptr when the session is unknown, finished, or the
	// child did not answer in time (a script busy in a long operation cannot
	// pump messages; the next tick tries again).
	HMENU TrayMenu(DWORD aSessionId) const;

	// Blocks until the session's child process exits; writes its official exit
	// code (load failure = 2, same as AutoHotkey.exe) into aExitCode once it has
	// finished. Returns false while still running, or when aSessionId is unknown.
	bool WaitSession(DWORD aSessionId, DWORD aTimeoutMs, DWORD *aExitCode);

	// Stops the way the kernel itself does it: WM_CLOSE to the child's main
	// window -> OnExit -> clean exit. When there is no window left to close,
	// TerminateJobObject is the last resort. StopSession returns false when the
	// session is already finished; StopAll returns how many it signalled.
	bool StopSession(DWORD aSessionId);
	size_t StopAll();

	// Consumes one record: attributes it to a session, appends it to that
	// session's JSONL (flushed, so a reader never waits on a buffer), and hands
	// it over. aTimeoutMs = INFINITE blocks until one arrives; false on timeout.
	bool PopRecord(ErrorRecord &aRecord, DWORD aTimeoutMs);

	// How many kernel records are still waiting to be routed and written to
	// their session's JSONL. A record reaches the log when the UI tick drains
	// the queue: the console write and the log write happen together, in one
	// place, so the two can never disagree. A caller that wants the log NOW
	// waits for this to reach zero - otherwise it reads a file that is still
	// empty because the UI has not ticked yet (m083).
	size_t PendingRecords() const;

	// Writes every pending record to its session's JSONL right now and returns
	// how many it wrote. The queue keeps the records - the UI still gets them
	// for the console - but the file is already complete when this returns.
	// A caller that reads the log does not have to wait for the UI tick, and
	// the two can no longer race each other (m083).
	size_t FlushPendingLogs();

private:
	Engine() = default;
	~Engine();
	Engine(const Engine &) = delete;
	Engine &operator=(const Engine &) = delete;

	// One child process: the interpreter exe running one script, its own job
	// object (host exit = every script gone), a raw error JSONL the kernel
	// patch appends to, and a stdout pipe a reader thread drains.
	struct Entry
	{
		Session pub;
		HANDLE process = nullptr;  // owned; closed when the session is reaped
		HANDLE job = nullptr;      // owned; closed with the process
		DWORD pid = 0;
		bool started = false;      // the child process was actually created
		std::wstring rawPath;      // absolute: temp\work\<...>-errors.jsonl (kernel appends)
		ULONGLONG rawOffset = 0;   // bytes already pumped into the queue
		std::wstring menuPath;     // absolute: temp\work\<...>-menu.json (patch 4 target)
		HANDLE stdoutRead = nullptr;   // owned pipe end; closing it ends the reader
		HANDLE stdoutThread = nullptr; // owned reader thread
		std::string stdoutPending;     // partial line across ReadFile boundaries
		std::wstring logFull;          // absolute, resolved against mLogRoot
		std::ofstream log;             // pub.logPath
		size_t logBytes = 0;           // bytes in log; m106 rotation threshold
	};

	// Reads the child's stdout pipe. One thread per session: ReadFile blocks,
	// and that is the point - no polling, and closing the read handle ends it.
	static unsigned __stdcall StdoutThread(void *aArg);
	void EnqueueStdout(Entry &aEntry, const std::string &aLine);

	// Reads every session's raw error file from rawOffset on, parses complete
	// lines and enqueues them. Called by every consumer before it looks at the
	// queue - the child appends asynchronously, so without the pump an early
	// reader would see an empty log and conclude the script said nothing (m083).
	void PumpRawFiles();

	// Routes one record to its session, writes it to that session's JSONL and
	// applies the "a failure stops the script" rule. The caller holds
	// mSessionsMutex; the return value is the session to ask to stop, or 0.
	DWORD PublishRecord(ErrorRecord &aRecord);

	// All four assume mSessionsMutex is held.
	DWORD Route(const ErrorRecord &aRecord) const;
	void StampSession(ErrorRecord &aRecord) const;
	void WriteLog(const ErrorRecord &aRecord);
	// m106: one session keeps exactly two files - the live one and .1. Rotating
	// in place (not per-run files) means a long-running heartbeat cannot grow a
	// log without bound, while a caller tailing the file still sees whole lines.
	void RotateLog(Entry &aEntry);
	void ReapLocked(Entry &aEntry) const;

	// m132: (re)builds the host-owned HMENU from a JSON snapshot. The caller
	// holds mSessionsMutex; the returned menu is stored in mMenus.
	HMENU BuildMenuLocked(DWORD aSessionId, const std::wstring &aSnapshotPath);

	std::wstring mKernelPath; // the official interpreter exe, patched
	// The log root, absolute, captured in Load(): an AHK script's A_WorkingDir is
	// the *process* cwd, so the first script that starts flips it (m011 reading:
	// session logs started landing in the script's own folder). Nothing the host
	// opens later may stay relative, or one script's error log ends up inside
	// another script's folder. Scripts keep the official cwd behaviour - the host
	// is the one that has to stop depending on it.
	std::wstring mLogRoot;
	// m106: per-session JSONL cap, from AHKDECK_LOG_CAP (the gate sets it small);
	// 8 MB keeps a heartbeat script's log two-files-small without touching code.
	size_t mLogCap = 8u * 1024u * 1024u;

	mutable std::mutex mSessionsMutex;
	std::map<DWORD, Entry> mSessions;    // key = session id, ascending
	std::map<DWORD, DWORD> mThreadIndex; // child pid -> session id (kept after finish)
	mutable std::map<DWORD, HMENU> mMenus; // session id -> host-owned tray menu (m132)
	DWORD mNextSessionId = 1;

	std::string mUnattributedDisplay; // logs\_unattributed-<timestamp>.jsonl, for the console
	std::wstring mUnattributedLog;    // the same file, absolute, made on demand
	std::ofstream mUnattributedFile;

	mutable std::mutex mErrorMutex;
	std::condition_variable mErrorCv;
	std::deque<ErrorRecord> mErrors;
};

} // namespace ahkdeck

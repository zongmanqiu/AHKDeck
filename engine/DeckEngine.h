// deck engine - in-process AutoHotkey_H host: one session per script.
//
// One script = one kernel thread inside *this* process. No child process, no
// second task-manager entry: closing the host closes every script, because the
// kernel threads die with the process image that owns them.
//
// A Session is the unit the UI and the AI both see: an id, the script it runs,
// a state, an exit code and exactly one JSONL of kernel records. m011 keeps the
// session bookkeeping here (not in the CLI) so every consumer routes records the
// same way - a second copy of that logic in the UI would be a second truth.
//
// Errors never reach a modal dialog: the patched kernel (see
// patches/AutoHotkey_H-v2.0.25/source/deck_sink.h) hands one UTF-8 JSON line to
// OnError() on the failing thread. That callback only enqueues - PopRecord()
// attributes the line to a session, appends it to that session's JSONL and
// returns it, so no consumer has to redo the routing.
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
	std::string kind;     // "error" | "warn"
	std::string phase;    // "load" | "runtime"
	std::string severity; // "fail" | "critical" | "fail_or_ok" | "warn"
	std::string file;     // script file name as the official dialog shows it
	std::string message;
	std::string extra;    // "Specifically:" text (may be empty)
	DWORD session = 0;    // owning session; 0 = could not be attributed
	int thread = 0;
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
	Running,  // kernel thread alive (still executing even after StopSession)
	Stopping, // WM_QUIT posted, not reaped yet
	Finished  // reaped: rc is valid
};

struct Session
{
	DWORD id = 0;
	std::wstring path;   // script path exactly as StartSession was given it
	std::string name;    // base name, UTF-8 (console + journal label)
	std::string logPath; // relative logs\<name>-<id>-<timestamp>.jsonl
	DWORD threadId = 0;  // kernel script thread; 0 when it never started
	SessionState state = SessionState::Running;
	DWORD rc = 0;        // official exit code, valid once Finished
	bool fatal = false;  // a "fail" record landed on this session
};

class Engine
{
public:
	static Engine &Instance();

	// Loads the patched kernel and installs the error sink.
	// On failure aError explains why (relative paths only, never a machine path).
	bool Load(std::string &aError);
	void Unload();
	bool IsLoaded() const { return mModule != nullptr; }

	// The absolute log root pinned by Load() (m011). The UI opens this exact
	// directory instead of rebuilding a relative "logs" path: a running script
	// changes the process cwd, and a relative path would resolve elsewhere.
	// Empty until Load() succeeds.
	const std::wstring &LogRoot() const { return mLogRoot; }

	// Starts one script as one session and returns its id (> 0). A script that
	// cannot even be read or parsed still gets a session, finished with the
	// official load-failure code 2, so its error record has a home. aError stays
	// non-empty while the session runs only if its JSONL could not be created.
	DWORD StartSession(const std::wstring &aScriptPath, const std::vector<std::wstring> &aArgs,
		std::string &aError);

	// Validates one script file with the kernel's own /validate mode (m103):
	// the command-line parser runs LoadFromFile and returns before a single
	// statement executes. Two hard facts decide where this may be called:
	//   * the kernel exposes no other validate entry (no COM method, no export);
	//   * once any source file has been loaded, the COM Main refuses with
	//     ERROR_NOT_SUPPORTED - so this belongs in a FRESH process, which is why
	//     the deck spawns itself as a short-lived helper (agent/Toolbox).
	// Returns the kernel exit code (0 = parses, 2 = load failure, 1 = no kernel);
	// aRecords receives the load errors without log routing (there is no session).
	int ValidateFile(const std::wstring &aScriptPath, std::string &aError,
		std::vector<ErrorRecord> &aRecords);

	// Snapshot copies - handles and the kernel stay private. Finished sessions
	// stay in the table: the UI list and the journal both need their rc.
	bool GetSession(DWORD aSessionId, Session &aOut) const;
	std::vector<Session> Sessions() const;
	static const char *StateName(SessionState aState);

	// The session's script-thread main window, or nullptr when there is none
	// (unknown id, never started, or already finished). That window is what the
	// kernel dispatches OnMessage() through, so the deck's menu bridge (m023) is
	// posted here: it replaces v1.3's WinExist("ahk_pid " pid), which cannot work
	// now that every session lives in one process.
	HWND FindScriptWindow(DWORD aSessionId) const;

	// Blocks until the session's kernel thread exits; writes its official exit
	// code (load failure = 2, same as AutoHotkey.exe) into aExitCode once it has
	// finished. Returns false while still running, or when aSessionId is unknown.
	bool WaitSession(DWORD aSessionId, DWORD aTimeoutMs, DWORD *aExitCode);

	// Stops the way the kernel itself does it: PostThreadMessage(WM_QUIT) ->
	// message loop -> clean TerminateApp. StopSession returns false when the
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

	static void __stdcall OnError(const char *aJson);
	// m061: FileAppend's stdout target, same queue and same attribution rules.
	static void __stdcall OnOutput(const char *aJson);
	static void __stdcall OnThreadStarted(void *aCtx);

	// Waits until the kernel's report about a failed load has arrived. The
	// kernel raises it from the script thread, which can be a moment AFTER
	// NewThread() gave up on that thread - and the first load of a cold kernel
	// takes long enough for the caller to read the log before the record
	// exists. Without this wait, "the script failed" and "the script said
	// nothing" look the same to the caller (m083).
	bool WaitForLoadRecord(const std::wstring &aScriptPath, DWORD aTimeoutMs);

	// Routes one record to its session, writes it to that session's JSONL and
	// applies the "a failure stops the script" rule. The caller holds
	// mSessionsMutex; the return value is the thread to ask to stop, or 0.
	DWORD PublishRecord(ErrorRecord &aRecord);

	struct Entry
	{
		Session pub;
		HANDLE thread = nullptr; // owned; closed when the session is reaped
		bool started = false;    // the kernel called our start callback
		void *ctx = nullptr;     // StartCtx (see DeckEngine.cpp), owned by the entry
		std::wstring logFull;    // absolute, resolved against mLogRoot
		std::ofstream log;       // pub.logPath
		size_t logBytes = 0;     // bytes in log; m106 rotation threshold
	};

	// All four assume mSessionsMutex is held.
	DWORD Route(const ErrorRecord &aRecord) const;
	void StampSession(ErrorRecord &aRecord) const;
	void WriteLog(const ErrorRecord &aRecord);
	// m106: one session keeps exactly two files - the live one and .1. Rotating
	// in place (not per-run files) means a long-running heartbeat cannot grow a
	// log without bound, while a caller tailing the file still sees whole lines.
	void RotateLog(Entry &aEntry);
	void ReapLocked(Entry &aEntry) const;

	HMODULE mModule = nullptr;
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
	// DeckNewThread, not the public 3-parameter NewThread: only the 5-parameter
	// form wires the start callback, which both proves the script loaded and
	// yields a race-free thread handle.
	DWORD(__stdcall *mNewThread)(LPWSTR, LPWSTR, LPWSTR, void (*)(void *), void *) = nullptr;
	BOOL(__stdcall *mAhkReady)(DWORD) = nullptr;
	void(__stdcall *mSetErrorSink)(void(__stdcall *)(const char *)) = nullptr;
	void(__stdcall *mSetOutputSink)(void(__stdcall *)(const char *)) = nullptr;
	void(__stdcall *mSetIsolation)(bool) = nullptr;

	mutable std::mutex mSessionsMutex;
	std::map<DWORD, Entry> mSessions;    // key = session id, ascending
	std::map<DWORD, DWORD> mThreadIndex; // kernel thread id -> session id (kept after finish)
	DWORD mNextSessionId = 1;

	std::string mUnattributedDisplay; // logs\_unattributed-<timestamp>.jsonl, for the console
	std::wstring mUnattributedLog;    // the same file, absolute, made on demand
	std::ofstream mUnattributedFile;

	mutable std::mutex mErrorMutex;
	std::condition_variable mErrorCv;
	std::deque<ErrorRecord> mErrors;
};

} // namespace ahkdeck

#include "engine/DeckEngine.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace ahkdeck {

// ---------------------------------------------------------------------------
// Minimal JSON reader for the kernel's fixed record layout.
// The producer is our own patch, so a full parser would be dead weight; the
// layout is flat: {"key":<string|int>, ...} with no nesting and no arrays.
// ---------------------------------------------------------------------------

static std::string JsonUnescape(const std::string &aIn)
{
	std::string out;
	out.reserve(aIn.size());
	for (size_t i = 0; i < aIn.size(); ++i)
	{
		char c = aIn[i];
		if (c != '\\' || i + 1 >= aIn.size())
		{
			out += c;
			continue;
		}
		char e = aIn[++i];
		switch (e)
		{
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		case 'b': out += '\b'; break;
		case 'f': out += '\f'; break;
		case 'u':
			// Only \u00XX (control characters) are ever emitted by the sink.
			if (i + 4 < aIn.size())
			{
				int v = 0;
				bool ok = true;
				for (int k = 1; k <= 4; ++k)
				{
					char h = aIn[i + k];
					v <<= 4;
					if (h >= '0' && h <= '9') v |= h - '0';
					else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
					else { ok = false; break; }
				}
				i += 4;
				if (ok) out += (char)v;
			}
			break;
		default:
			out += e;
			break;
		}
	}
	return out;
}

static bool FieldFind(const std::string &aJson, const char *aKey, size_t &aValuePos)
{
	std::string needle = "\"";
	needle += aKey;
	needle += "\":";
	size_t p = aJson.find(needle);
	if (p == std::string::npos)
		return false;
	aValuePos = p + needle.size();
	return true;
}

static bool GetString(const std::string &aJson, const char *aKey, std::string &aOut)
{
	size_t p = 0;
	if (!FieldFind(aJson, aKey, p) || p >= aJson.size() || aJson[p] != '"')
		return false;
	std::string raw;
	for (size_t i = p + 1; i < aJson.size(); ++i)
	{
		if (aJson[i] == '\\' && i + 1 < aJson.size())
		{
			raw += aJson[i];
			raw += aJson[i + 1];
			++i;
			continue;
		}
		if (aJson[i] == '"')
		{
			aOut = JsonUnescape(raw);
			return true;
		}
		raw += aJson[i];
	}
	return false;
}

static bool GetInt(const std::string &aJson, const char *aKey, int &aOut)
{
	size_t p = 0;
	if (!FieldFind(aJson, aKey, p))
		return false;
	int v = 0;
	bool any = false;
	for (size_t i = p; i < aJson.size(); ++i)
	{
		if (aJson[i] >= '0' && aJson[i] <= '9')
		{
			v = v * 10 + (aJson[i] - '0');
			any = true;
		}
		else
			break;
	}
	if (any)
		aOut = v;
	return any;
}

static void ParseRecord(const std::string &aJson, ErrorRecord &aRec)
{
	aRec.json = aJson;
	GetString(aJson, "ts", aRec.ts);
	GetString(aJson, "kind", aRec.kind);
	GetString(aJson, "phase", aRec.phase);
	GetString(aJson, "severity", aRec.severity);
	GetString(aJson, "file", aRec.file);
	GetString(aJson, "message", aRec.message);
	GetString(aJson, "extra", aRec.extra);
	GetInt(aJson, "thread", aRec.thread);
	GetInt(aJson, "seq", aRec.seq);
	GetInt(aJson, "line", aRec.line);
}

// ---------------------------------------------------------------------------

namespace {

// Passed to the kernel's start callback (which runs ON the script thread) so we
// can grab a handle race-free: by the time NewThread() returns, the script may
// already have finished, and OpenThread() on a dead thread id fails.
struct StartCtx
{
	HANDLE thread = nullptr;
	DWORD id = 0;
	bool started = false;
};

// The kernel gives every script thread its own main window: g_hWnd and
// g_WindowClassMain are thread_local (.3rd\AutoHotkey_H-v2.0.25\source\globaldata.cpp:493)
// and the window is created with the class WINDOW_CLASS_MAIN == "AutoHotkey"
// (source\script.cpp:1027, source\defines.h:54). Script GUIs use the *other*
// class ("AutoHotkeyGUI"), so an exact class match is what separates the two -
// and it is the window OnMessage() dispatches through, which is why the deck's
// menu bridge (m023) has to be posted there and not to any window of the script.
constexpr wchar_t kScriptWindowClass[] = L"AutoHotkey";

struct ScriptWindowSearch
{
	DWORD threadId = 0;
	DWORD processId = 0;
	HWND found = nullptr;
};

BOOL CALLBACK EnumScriptWindow(HWND aHwnd, LPARAM aParam)
{
	ScriptWindowSearch *search = reinterpret_cast<ScriptWindowSearch *>(aParam);
	wchar_t className[64] = {0};
	if (GetClassNameW(aHwnd, className, 64) == 0 || wcscmp(className, kScriptWindowClass) != 0)
		return TRUE;

	// EnumThreadWindows already restricts the candidate to our script thread, but
	// a thread id can be recycled by another process after the thread dies, and a
	// second AHK instance on this machine has a window of the same class. Both
	// sides must agree before the deck posts to it.
	DWORD windowProcessId = 0;
	const DWORD windowThreadId = GetWindowThreadProcessId(aHwnd, &windowProcessId);
	if (windowThreadId != search->threadId || windowProcessId != search->processId)
		return TRUE;

	search->found = aHwnd;
	return FALSE;
}

std::string Utf8Of(const std::wstring &aWide)
{
	if (aWide.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), out.data(), n, nullptr, nullptr);
	return out;
}

// The log path is UTF-8 for the console and the journal, but file I/O needs the
// wide form: a narrow path goes through the ANSI code page and mangles every
// Chinese script name.
std::wstring WideOf(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), out.data(), n);
	return out;
}

// Same escaping rules as the kernel's sink (DeckPutEscaped), so a host-made
// record is indistinguishable from a kernel one apart from the missing seq.
std::string JsonEscape(const std::string &aIn)
{
	std::string out;
	for (char c : aIn)
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
	return out;
}

bool SamePathI(const std::string &aLeft, const std::string &aRight)
{
	if (aLeft.size() != aRight.size())
		return false;
	for (size_t i = 0; i < aLeft.size(); ++i)
	{
		char l = aLeft[i] == '\\' ? '/' : aLeft[i];
		char r = aRight[i] == '\\' ? '/' : aRight[i];
		if (std::tolower((unsigned char)l) != std::tolower((unsigned char)r))
			return false;
	}
	return true;
}

// Base name without extension, UTF-8 - the JSONL file is named after the script.
std::string StemOf(const std::wstring &aPath)
{
	size_t p = aPath.find_last_of(L"\\/");
	std::wstring leaf = p == std::wstring::npos ? aPath : aPath.substr(p + 1);
	size_t dot = leaf.find_last_of(L'.');
	if (dot != std::wstring::npos)
		leaf.resize(dot);
	return Utf8Of(leaf);
}

std::string TimestampFileName()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04u%02u%02u-%02u%02u%02u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	return buf;
}

// Same shape as the kernel's "ts" field, so synthetic records look native.
std::string LocalTimestamp()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[32];
	_snprintf_s(buf, _countof(buf), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

// A record for a failure the kernel never saw (the script file could not even be
// read). It carries the kernel's fields except "seq", which that counter owns.
ErrorRecord MakeSyntheticRecord(DWORD aSessionId, const std::string &aMessage)
{
	ErrorRecord rec;
	rec.session = aSessionId;
	rec.kind = "error";
	rec.phase = "load";
	rec.severity = "fail";
	rec.file = "?";
	rec.message = aMessage;
	rec.ts = LocalTimestamp();
	rec.json = "{\"session\":" + std::to_string(aSessionId) + ",\"ts\":\"" + rec.ts
		+ "\",\"thread\":0,\"kind\":\"error\",\"phase\":\"load\",\"severity\":\"fail\""
		+ ",\"file\":\"?\",\"line\":0,\"message\":\"" + JsonEscape(aMessage) + "\",\"extra\":\"\"}";
	return rec;
}

// Reads a script file the way the official engine does: UTF-8 (with or without
// BOM) or UTF-16LE (BOM). The kernel's NewThread() runs script *text*, not a
// path - the path only feeds A_ScriptDir / A_ScriptName / #Include resolution.
bool ReadScriptText(const std::wstring &aPath, std::wstring &aText, std::string &aError)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		aError = "script file could not be opened";
		return false;
	}

	std::string bytes;
	char chunk[4096];
	DWORD got = 0;
	while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got)
		bytes.append(chunk, got);
	CloseHandle(file);

	size_t begin = 0;
	bool utf16 = false;
	if (bytes.size() >= 3 && bytes[0] == '\xEF' && bytes[1] == '\xBB' && bytes[2] == '\xBF')
		begin = 3; // UTF-8 BOM
	else if (bytes.size() >= 2 && bytes[0] == '\xFF' && bytes[1] == '\xFE')
		begin = 2, utf16 = true; // UTF-16LE BOM

	if (utf16)
	{
		aText.assign(reinterpret_cast<const wchar_t *>(bytes.data() + begin),
			(bytes.size() - begin) / 2);
		return true;
	}

	int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + begin, (int)(bytes.size() - begin), nullptr, 0);
	aText.assign((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, bytes.data() + begin, (int)(bytes.size() - begin),
		aText.data(), n);
	return true;
}

std::wstring QuoteArg(const std::wstring &aArg)
{
	if (aArg.find_first_of(L" \t\"") == std::wstring::npos)
		return aArg;
	std::wstring out = L"\"";
	for (wchar_t c : aArg)
	{
		if (c == L'"')
			out += L'\\';
		out += c;
	}
	out += L'"';
	return out;
}

} // namespace

Engine &Engine::Instance()
{
	// Function-local static: thread-safe init, destroyed at process exit.
	static Engine instance;
	return instance;
}

Engine::~Engine()
{
	if (mModule)
		Unload();
}

const char *Engine::StateName(SessionState aState)
{
	switch (aState)
	{
	case SessionState::Running: return "running";
	case SessionState::Stopping: return "stopping";
	default: return "finished";
	}
}

bool Engine::Load(std::string &aError)
{
	if (mModule)
		return true;

	std::wstring dllPath;
	wchar_t buf[MAX_PATH] = {0};
	if (GetEnvironmentVariableW(L"AHKDECK_KERNEL", buf, MAX_PATH) > 0)
	{
		dllPath = buf;
	}
	else
	{
		if (!GetModuleFileNameW(nullptr, buf, MAX_PATH))
		{
			aError = "cannot resolve the host executable path";
			return false;
		}
		// Same directory as AHKDeck.exe; the build copies the kernel there.
		std::wstring dir(buf);
		size_t slash = dir.find_last_of(L'\\');
		dir.resize(slash == std::wstring::npos ? 0 : slash + 1);
		dllPath = dir + L"AutoHotkey64.dll";
	}

	mModule = LoadLibraryW(dllPath.c_str());
	if (!mModule)
	{
		aError = "AutoHotkey64.dll could not be loaded (missing next to AHKDeck.exe or blocked)";
		return false;
	}

	mNewThread = reinterpret_cast<decltype(mNewThread)>(GetProcAddress(mModule, "DeckNewThread"));
	mAhkReady = reinterpret_cast<decltype(mAhkReady)>(GetProcAddress(mModule, "ahkReady"));
	mSetErrorSink = reinterpret_cast<decltype(mSetErrorSink)>(GetProcAddress(mModule, "DeckSetErrorSink"));
	mSetIsolation = reinterpret_cast<decltype(mSetIsolation)>(GetProcAddress(mModule, "DeckSetScriptIsolation"));

	if (!mNewThread || !mSetErrorSink || !mSetIsolation)
	{
		aError = "AutoHotkey64.dll is not an AHKDeck-capable kernel (patched build required)";
		FreeLibrary(mModule);
		mModule = nullptr;
		return false;
	}

	// Install once; the sink is process-wide inside the kernel.
	mSetErrorSink(&Engine::OnError);
	// Every script thread here must behave like a standalone script (the official
	// AutoHotkey.exe), not like a secondary thread of one multi-script process.
	mSetIsolation(true);

	// m051: portable layout - logs live next to the exe, never in the process
	// cwd or %LOCALAPPDATA%. Resolve while the host owns the path; the first
	// script thread changes the process working directory.
	wchar_t exePath[MAX_PATH] = {0};
	DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	if (exeLen > 0 && exeLen < MAX_PATH)
	{
		wchar_t *slash = wcsrchr(exePath, L'\\');
		if (slash)
			*slash = 0;
		mLogRoot = std::wstring(exePath) + L"\\Logs";
		CreateDirectoryW(mLogRoot.c_str(), nullptr);
	}
	return true;
}

void Engine::Unload()
{
	HMODULE module = mModule;
	if (!module)
		return;
	StopAll();

	// Scripts must be gone before FreeLibrary: unloading code that a live
	// thread is executing in would crash the host.
	std::vector<DWORD> ids;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		for (const auto &it : mSessions)
			ids.push_back(it.first);
	}
	for (DWORD id : ids)
	{
		DWORD rc = 0;
		WaitSession(id, 5000, &rc);
	}

	bool stillRunning = false;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		for (auto &it : mSessions)
		{
			if (it.second.thread)
				CloseHandle(it.second.thread);
			delete static_cast<StartCtx *>(it.second.ctx);
			if (it.second.pub.state != SessionState::Finished)
				stillRunning = true;
		}
		mSessions.clear();
		mThreadIndex.clear();
	}
	(void)stillRunning; // nothing can undo a live script here; the module stays mapped either way
	mModule = nullptr;
	mNewThread = nullptr;
	mAhkReady = nullptr;
	mSetErrorSink = nullptr;
	mSetIsolation = nullptr;
	// Never FreeLibrary the kernel. AHK_H's detach path tears the remaining script
	// threads down while the caller holds the loader lock, which is not a state
	// this host ever needs: the OS reclaims the module at process exit, where the
	// same detach runs with lpvReserved set and no threads left to wait for.
	// Unloading is therefore not a feature here.
	//
	// Attribution, stated as measured and not as a story: the ~1-in-40 "host writes
	// the record, then never exits" hang was last seen before two changes landed
	// (this one, and the OpenThread retry that stops a handleless session from
	// spinning the consumer forever). The follow-up arm measured 0 hangs in 200
	// runs, so the defect no longer reproduces - but nothing here separates which
	// change mattered. See .plan/m012-宿主退出挂起.md.
}

void __stdcall Engine::OnError(const char *aJson)
{
	// Runs on the failing script thread. Nothing but an enqueue happens here:
	// formatting and file I/O belong to the consumer, never to the script.
	try
	{
		if (!aJson)
			return;
		ErrorRecord rec;
		ParseRecord(std::string(aJson), rec);
		Engine &engine = Engine::Instance();
		{
			std::lock_guard<std::mutex> lock(engine.mErrorMutex);
			engine.mErrors.push_back(std::move(rec));
		}
		engine.mErrorCv.notify_one();
	}
	catch (...)
	{
		// A throwing callback across a DLL boundary would be fatal.
	}
}

void __stdcall Engine::OnThreadStarted(void *aCtx)
{
	try
	{
		auto *ctx = static_cast<StartCtx *>(aCtx);
		if (!ctx)
			return;
		// GetCurrentThreadId, not the id returned later by NewThread(): this is
		// the only moment where the script thread is guaranteed to be alive.
		ctx->id = GetCurrentThreadId();
		// SYNCHRONIZE for WaitForSingleObject, QUERY_INFORMATION because
		// GetExitCodeThread refuses handles without it (it silently returns 0
		// otherwise, which is how ExitApp's code got lost once already).
		ctx->thread = OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, ctx->id);
		ctx->started = true;
	}
	catch (...)
	{
	}
}

// All callers hold mSessionsMutex.
DWORD Engine::Route(const ErrorRecord &aRecord) const
{
	auto it = mThreadIndex.find((DWORD)aRecord.thread);
	if (it != mThreadIndex.end())
		return it->second;

	// Single-script host run (the --run case): whatever the kernel reported, even
	// from a thread that died before our start callback, belongs to that session.
	// m010 relied on this for load failures, and official scripts report from
	// their own script thread whether the error is in auto-execute, a hotkey or a
	// timer callback - those are AHK "threads", not OS threads.
	if (mSessions.size() == 1)
		return mSessions.begin()->first;

	// A script that never started has no thread id to match on, and its load
	// error names the file we handed the kernel - that is the only handle here.
	// Newest first: the failure being reported is the one that just happened.
	for (auto riter = mSessions.rbegin(); riter != mSessions.rend(); ++riter)
	{
		const Entry &entry = riter->second;
		if (entry.started)
			continue;
		std::string given = Utf8Of(entry.pub.path);
		if (SamePathI(aRecord.file, given))
			return entry.pub.id;
		wchar_t full[MAX_PATH] = {0};
		if (GetFullPathNameW(entry.pub.path.c_str(), MAX_PATH, full, nullptr) > 0
			&& SamePathI(aRecord.file, Utf8Of(full)))
			return entry.pub.id;
	}
	return 0;
}

// All callers hold mSessionsMutex.
void Engine::StampSession(ErrorRecord &aRecord) const
{
	if (aRecord.json.compare(0, 11, "{\"session\":") == 0)
		return; // host-synthesised records arrive stamped already
	aRecord.json.replace(0, 1, "{\"session\":" + std::to_string(aRecord.session) + ",");
}

// All callers hold mSessionsMutex. A record is written to exactly one file and
// flushed: an AI tailing the JSONL must not wait for a buffer the host may never
// close, because the host can be killed - that is the point of requirement 2.
void Engine::WriteLog(const ErrorRecord &aRecord)
{
	std::ofstream *log = &mUnattributedFile;
	std::wstring path;

	if (aRecord.session)
	{
		auto it = mSessions.find(aRecord.session);
		if (it != mSessions.end())
		{
			log = &it->second.log;
			path = it->second.logFull;
		}
	}
	if (path.empty())
	{
		if (mUnattributedLog.empty())
		{
			mUnattributedDisplay = "logs\\_unattributed-" + TimestampFileName() + ".jsonl";
			mUnattributedLog = mLogRoot.empty()
				? WideOf(mUnattributedDisplay)
				: mLogRoot + L"\\" + WideOf("_unattributed-" + TimestampFileName() + ".jsonl");
		}
		path = mUnattributedLog;
	}

	if (!log->is_open())
		log->open(path.c_str(), std::ios::binary | std::ios::app);
	if (!log->is_open())
		return; // StartSession already reported the log problem; the record still reaches the console
	log->write(aRecord.json.data(), (std::streamsize)aRecord.json.size());
	log->put('\n');
	log->flush();
}

DWORD Engine::StartSession(const std::wstring &aScriptPath, const std::vector<std::wstring> &aArgs,
	std::string &aError)
{
	if (!mModule)
	{
		aError = "engine is not loaded";
		return 0;
	}

	DWORD sessionId = 0;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		sessionId = mNextSessionId++;

		Entry entry;
		entry.pub.id = sessionId;
		entry.pub.path = aScriptPath;
		entry.pub.name = StemOf(aScriptPath);
		// One session = one JSONL from the moment it exists, even if it stays
		// empty: the path is printed to the console and to the journal, so it
		// must resolve. Displayed relative (rule 9), opened absolute (mLogRoot).
		std::string leaf = std::to_string(sessionId) + "-" + TimestampFileName() + ".jsonl";
		entry.pub.logPath = "Logs\\" + entry.pub.name + "-" + leaf;
		entry.logFull = mLogRoot.empty()
			? WideOf(entry.pub.logPath) // no cwd captured (Load was skipped): stay relative
			: mLogRoot + L"\\" + WideOf(entry.pub.name + "-" + leaf);
		CreateDirectoryW(mLogRoot.c_str(), nullptr);
		entry.log.open(entry.logFull.c_str(), std::ios::binary | std::ios::trunc);
		if (!entry.log.is_open())
			aError = "the session log could not be created"; // runs anyway: records land on the console
		mSessions[sessionId] = std::move(entry);
	}

	// NewThread() runs script TEXT. Reading it here (not letting the kernel open
	// the path) keeps line numbers byte-identical, while passing the real path on
	// the command line gives A_ScriptDir / A_ScriptName / #Include the same
	// meaning as the official executable.
	std::wstring text;
	std::string readError;
	if (!ReadScriptText(aScriptPath, text, readError))
	{
		{
			std::lock_guard<std::mutex> lock(mErrorMutex);
			mErrors.push_back(MakeSyntheticRecord(sessionId, readError));
		}
		mErrorCv.notify_one();

		std::lock_guard<std::mutex> lock(mSessionsMutex);
		Entry &stored = mSessions[sessionId];
		stored.started = false;
		stored.pub.state = SessionState::Finished;
		stored.pub.rc = 2; // CRITICAL_ERROR, as the official engine reports it
		stored.pub.fatal = true;
		return sessionId;
	}

	std::wstring cmd = QuoteArg(aScriptPath);
	for (const auto &arg : aArgs)
		cmd += L" " + QuoteArg(arg);

	// No title: ThreadMain copies the title into the buffer that also holds the
	// default script text, and a short text + long title would overflow it
	// (upstream quirk). The window title is ours to provide at the UI milestone.
	StartCtx *ctx = new StartCtx();
	DWORD threadId = mNewThread(text.data(), cmd.data(), nullptr, &Engine::OnThreadStarted, ctx);

	std::lock_guard<std::mutex> lock(mSessionsMutex);
	Entry &stored = mSessions[sessionId];
	stored.ctx = ctx;

	if (!threadId && !ctx->started)
	{
		// Load-time failure: the sink already reported why, and Route() attaches
		// that record to this session by file; the official exit code for it is
		// CRITICAL_ERROR (2). There is no thread handle left to wait on.
		stored.started = false;
		stored.pub.state = SessionState::Finished;
		stored.pub.rc = 2;
		delete ctx;
		stored.ctx = nullptr;
		return sessionId;
	}

	if (!threadId)
		threadId = ctx->id; // started, but NewThread could not report the id

	if (ctx->started && !ctx->thread)
	{
		// m011 saw this once in ~40 runs: the script thread reported itself, but
		// OpenThread failed on it, so the host has nothing to wait on. Without a
		// handle the session can never be reaped, and a consumer that waits for
		// its exit code waits forever. Retry once (the thread is alive by
		// definition here), then say so out loud instead of spinning.
		ctx->thread = OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, threadId);
		if (!ctx->thread)
		{
			ErrorRecord rec = MakeSyntheticRecord(sessionId,
				"the script thread handle could not be opened; this session cannot be tracked");
			{
				std::lock_guard<std::mutex> queue(mErrorMutex);
				mErrors.push_back(rec);
			}
			mErrorCv.notify_one();
			stored.started = false; // nothing to wait on: report it as finished
			stored.pub.state = SessionState::Finished;
			stored.pub.rc = 2;
			delete ctx;
			stored.ctx = nullptr;
			return sessionId;
		}
	}

	stored.started = ctx->started;
	stored.thread = ctx->thread;
	stored.pub.threadId = threadId;
	stored.pub.state = SessionState::Running;
	if (threadId)
		mThreadIndex[threadId] = sessionId;
	// The index entry is never pruned: records can still arrive for a session
	// whose thread was reaped, and finished sessions stay in the table anyway.
	return sessionId;
}

// All callers hold mSessionsMutex. A session is Finished only once its kernel
// thread has actually been reaped: the UI polls Sessions() and must never be
// told "running" about a thread that is already gone (m011 read this from a
// neighbour's teardown - the thread had died while the table still said Running).
void Engine::ReapLocked(Entry &aEntry) const
{
	if (!aEntry.started || !aEntry.thread || aEntry.pub.state == SessionState::Finished)
		return;
	if (WaitForSingleObject(aEntry.thread, 0) != WAIT_OBJECT_0)
		return;
	DWORD rc = 0;
	if (!GetExitCodeThread(aEntry.thread, &rc))
		return; // cannot read the code: keep the state honest instead of guessing
	CloseHandle(aEntry.thread);
	aEntry.thread = nullptr;
	aEntry.pub.state = SessionState::Finished;
	aEntry.pub.rc = rc;
	// m027: the thread is gone, so no more records can be produced by it. Close
	// the JSONL handle now instead of holding one per finished session until the
	// engine unloads. WriteLog reopens on demand, so a late record still lands.
	if (aEntry.log.is_open())
		aEntry.log.close();
}

bool Engine::GetSession(DWORD aSessionId, Session &aOut) const
{
	std::lock_guard<std::mutex> lock(mSessionsMutex);
	auto it = mSessions.find(aSessionId);
	if (it == mSessions.end())
		return false;
	// Polling a session reaps it: reading the state is what notices the thread is
	// gone, so the const-correctness here is deliberately bent (mSessionsMutex is
	// held, and no other thread can be looking at the same entry).
	ReapLocked(const_cast<Entry &>(it->second));
	aOut = it->second.pub;
	return true;
}

std::vector<Session> Engine::Sessions() const
{
	std::vector<Session> out;
	std::lock_guard<std::mutex> lock(mSessionsMutex);
	out.reserve(mSessions.size());
	for (const auto &it : mSessions)
	{
		ReapLocked(const_cast<Entry &>(it.second));
		out.push_back(it.second.pub);
	}
	return out;
}

bool Engine::WaitSession(DWORD aSessionId, DWORD aTimeoutMs, DWORD *aExitCode)
{
	HANDLE handle = nullptr;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it == mSessions.end())
			return false;
		ReapLocked(it->second);
		if (it->second.pub.state == SessionState::Finished)
		{
			if (aExitCode)
				*aExitCode = it->second.pub.rc;
			return true; // already reaped: the entry stays queryable
		}
		handle = it->second.thread;
	}

	if (!handle)
		return false; // started but unopenable: never report a code we cannot read

	DWORD rc = 0;
	if (WaitForSingleObject(handle, aTimeoutMs) != WAIT_OBJECT_0)
		return false; // still running; keep the entry
	if (!GetExitCodeThread(handle, &rc))
		return false; // handle lacks query access; never report a guessed code

	std::lock_guard<std::mutex> lock(mSessionsMutex);
	auto it = mSessions.find(aSessionId);
	if (it != mSessions.end())
	{
		CloseHandle(it->second.thread);
		it->second.thread = nullptr;
		it->second.pub.state = SessionState::Finished;
		it->second.pub.rc = rc;
	}
	if (aExitCode)
		*aExitCode = rc;
	return true;
}

HWND Engine::FindScriptWindow(DWORD aSessionId) const
{
	DWORD threadId = 0;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it == mSessions.end() || !it->second.started)
			return nullptr;
		if (it->second.pub.state == SessionState::Finished)
			return nullptr; // its window is gone; a reused thread id must not be followed
		threadId = it->second.pub.threadId;
	}
	if (!threadId)
		return nullptr;

	ScriptWindowSearch search;
	search.threadId = threadId;
	search.processId = GetCurrentProcessId();
	EnumThreadWindows(threadId, EnumScriptWindow, reinterpret_cast<LPARAM>(&search));
	return search.found;
}

bool Engine::StopSession(DWORD aSessionId)
{
	DWORD threadId = 0;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it == mSessions.end() || !it->second.started)
			return false; // unknown, or already finished (also the load-failure case)
		if (it->second.pub.state == SessionState::Finished)
			return false;
		it->second.pub.state = SessionState::Stopping;
		threadId = it->second.pub.threadId;
	}
	// The kernel's TerminateSubThread posts WM_QUIT and only falls back to
	// WM_CLOSE when the post fails. A script sitting in a modal/menu loop can
	// accept the post and ignore it, so close the window as well: that is the
	// kernel's own "close the script" path, and a finished script has no window
	// left to close.
	bool signalled = PostThreadMessageW(threadId, WM_QUIT, 0, 0) != FALSE;
	if (HWND hwnd = FindScriptWindow(aSessionId))
	{
		DWORD_PTR result = 0;
		if (SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 500, &result))
			signalled = true;
	}
	return signalled;
}

size_t Engine::StopAll()
{
	std::vector<DWORD> sessionIds;
	std::vector<DWORD> threadIds;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		for (auto &it : mSessions)
		{
			if (!it.second.started || it.second.pub.state == SessionState::Finished)
				continue;
			it.second.pub.state = SessionState::Stopping;
			sessionIds.push_back(it.first);
			threadIds.push_back(it.second.pub.threadId);
		}
	}
	size_t signalled = 0;
	for (size_t i = 0; i < threadIds.size(); ++i)
	{
		bool one = PostThreadMessageW(threadIds[i], WM_QUIT, 0, 0) != FALSE;
		// Close the window as well: a thread that accepted WM_QUIT but is stuck
		// in a modal loop otherwise never reaches TerminateApp.
		if (HWND hwnd = FindScriptWindow(sessionIds[i]))
		{
			DWORD_PTR result = 0;
			if (SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 500, &result))
				one = true;
		}
		if (one)
			++signalled;
	}
	return signalled;
}

bool Engine::PopRecord(ErrorRecord &aRecord, DWORD aTimeoutMs)
{
	ErrorRecord rec;
	{
		std::unique_lock<std::mutex> lock(mErrorMutex);
		if (aTimeoutMs == INFINITE)
		{
			mErrorCv.wait(lock, [this] { return !mErrors.empty(); });
		}
		else if (!mErrorCv.wait_for(lock, std::chrono::milliseconds(aTimeoutMs),
				[this] { return !mErrors.empty(); }))
		{
			return false;
		}
		rec = std::move(mErrors.front());
		mErrors.pop_front();
	}

	// Routing and logging happen here, once: console, journal and (later) the UI
	// all read the same session id and the same JSONL.
	DWORD quitThread = 0;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		if (!rec.session)
			rec.session = Route(rec);
		StampSession(rec);
		WriteLog(rec);
		if (rec.session && (rec.severity == "fail" || rec.severity == "critical"))
		{
			auto it = mSessions.find(rec.session);
			if (it != mSessions.end())
			{
				it->second.pub.fatal = true;
				// An unhandled failure is the official dialog's "Exit": the script
				// stops. The kernel only unwinds the thread that raised it, so a
				// repeating timer would re-raise forever - m011 measured 17 KB of
				// identical records in 10 s. Stopping the session is what the user
				// would have clicked, and it is the only way to keep the storm out
				// of the log an AI is about to read.
				if (it->second.started && it->second.pub.state == SessionState::Running)
				{
					it->second.pub.state = SessionState::Stopping;
					quitThread = it->second.pub.threadId;
				}
			}
		}
	}
	if (quitThread)
		PostThreadMessageW(quitThread, WM_QUIT, 0, 0);
	aRecord = std::move(rec);
	return true;
}

} // namespace ahkdeck

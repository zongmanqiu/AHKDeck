#include "engine/DeckEngine.h"

#include "core/AppPaths.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <process.h>

namespace ahkdeck {

// The stdout reader thread's argument. One shared type: a local struct in each
// function would be a different type each time, and a pointer across that
// boundary is a bug waiting for a compiler update.
struct StdoutCtx
{
	Engine *engine;
	DWORD sessionId;
};

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
	GetString(aJson, "func", aRec.func);
	GetString(aJson, "source", aRec.source);
	GetString(aJson, "stack", aRec.stack);
	GetInt(aJson, "thread", aRec.thread);
	GetInt(aJson, "seq", aRec.seq);
	GetInt(aJson, "line", aRec.line);
}

// ---------------------------------------------------------------------------

namespace {

// The kernel gives every script process one main window of its own: the class
// is WINDOW_CLASS_MAIN == "AutoHotkey" (source\script.cpp registers it) and
// script GUIs use the *other* class ("AutoHotkeyGUI"), so an exact class match
// is what separates the two. It is the window OnMessage() dispatches through -
// which is why the deck's menu bridge (m023) posts there, and why kernel patch
// 4's AHKDECK_DUMP_MENU and the stop path (WM_CLOSE) go to the same window.
constexpr wchar_t kScriptWindowClass[] = L"AutoHotkey";

// Kernel patch 4 (patches/AutoHotkey-v2.0.31, script2.cpp): the deck asks the
// child's main window for a JSON snapshot of its live tray menu.
constexpr UINT kDeckDumpMenu = WM_APP + 0x1DE0;

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

// A record for a failure the kernel never saw (the script file could not even
// be opened, or the child could not be started). It carries the kernel's
// fields except "seq", which that counter owns.
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

// The child needs the host's whole environment plus the three AHKDECK_* knobs.
// CreateProcess takes a full block, not overrides - and the block must be
// sorted by name (case-insensitive), or CreateProcess fails with
// ERROR_INVALID_PARAMETER. So collect every entry, drop our own names, add
// ours, sort, join.
std::vector<wchar_t> BuildChildEnvironment(const std::wstring &aErrorFile,
	const std::wstring &aMenuFile)
{
	std::vector<std::pair<std::wstring, std::wstring>> entries;
	wchar_t *env = GetEnvironmentStringsW();
	if (env)
	{
		for (wchar_t *p = env; *p;)
		{
			std::wstring entry = p;
			const size_t eq = entry.find(L'=');
			if (eq != std::wstring::npos && !entry.empty()
				&& entry.compare(0, 8, L"AHKDECK_") != 0)
				entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
			p += entry.size() + 1;
		}
		FreeEnvironmentStringsW(env);
	}
	entries.emplace_back(L"AHKDECK_ERROR_FILE", aErrorFile);
	entries.emplace_back(L"AHKDECK_MENU_FILE", aMenuFile);
	entries.emplace_back(L"AHKDECK_NO_TRAY", L"1");
	// m194: A_AhkPath must name the host, not the kernel EXE. The official
	// scripts use InStr(A_AhkPath, "AHKDeck.exe") to tell "hosted" from
	// "standalone"; when it names the kernel they re-show their own tray icon
	// (A_IconHidden := false) and the merged tray menu loses its purpose.
	// The script.cpp patch reads this and restores the in-process DLL semantics.
	wchar_t hostExe[MAX_PATH] = {0};
	if (GetModuleFileNameW(nullptr, hostExe, MAX_PATH))
		entries.emplace_back(L"AHKDECK_HOST_EXE", hostExe);
	std::sort(entries.begin(), entries.end(),
		[](const std::pair<std::wstring, std::wstring> &a,
			const std::pair<std::wstring, std::wstring> &b)
		{
			// Case-insensitive by name; names are unique after the drop above.
			const size_t n = std::min<size_t>(a.first.size(), b.first.size());
			for (size_t i = 0; i < n; ++i)
			{
				const wchar_t l = towupper(a.first[i]);
				const wchar_t r = towupper(b.first[i]);
				if (l != r)
					return l < r;
			}
			return a.first.size() < b.first.size();
		});
	std::wstring block;
	for (const auto &e : entries)
		block += e.first + L"=" + e.second + L'\0';
	block += L'\0'; // the block itself is double-NUL terminated
	return std::vector<wchar_t>(block.begin(), block.end());
}

// Reads a file (or a file prefix) as one UTF-8 blob. The child appends JSON
// lines with FILE_APPEND_DATA; the pump reads from its last offset on.
bool ReadFileRange(const std::wstring &aPath, ULONGLONG aOffset, std::string &aOut,
	ULONGLONG &aNewSize)
{
	aOut.clear();
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size))
	{
		CloseHandle(file);
		return false;
	}
	aNewSize = (ULONGLONG)size.QuadPart;
	if (aNewSize <= aOffset)
	{
		CloseHandle(file);
		return true;
	}
	LARGE_INTEGER from;
	from.QuadPart = (LONGLONG)aOffset;
	SetFilePointerEx(file, from, nullptr, FILE_BEGIN);
	char chunk[4096];
	DWORD got = 0;
	while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got)
		aOut.append(chunk, got);
	CloseHandle(file);
	return true;
}

// Reads one whole file for the tray-menu snapshot.
bool ReadWholeFile(const std::wstring &aPath, std::string &aOut)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	char chunk[4096];
	DWORD got = 0;
	while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got)
		aOut.append(chunk, got);
	CloseHandle(file);
	return true;
}

// ---------------------------------------------------------------------------
// Menu snapshot parser + HMENU builder (m132 across the process split).
// The kernel patch writes {"items":[...]} where an item is
//   {"id":N,"text":"...","checked":b,"enabled":b,"radio":b,"default":b,
//    "items":[...]}   or   {"separator":true}
// ---------------------------------------------------------------------------

struct MenuItemData
{
	UINT id = 0;
	std::wstring text;
	bool separator = false;
	bool checked = false;
	bool enabled = true;
	bool radio = false;
	bool def = false;
	std::vector<MenuItemData> items;
};

struct MenuParser
{
	const char *p = nullptr;
	const char *end = nullptr;

	void SkipWs()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			++p;
	}

	bool Expect(char c)
	{
		SkipWs();
		if (p < end && *p == c)
		{
			++p;
			return true;
		}
		return false;
	}

	std::wstring ReadString()
	{
		SkipWs();
		std::string raw;
		if (p >= end || *p != '"')
			return std::wstring();
		++p;
		while (p < end && *p != '"')
		{
			if (*p == '\\' && p + 1 < end)
			{
				raw += *p++;
				raw += *p++;
				continue;
			}
			raw += *p++;
		}
		if (p < end)
			++p; // closing quote
		return WideOf(JsonUnescape(raw));
	}

	bool ReadBool()
	{
		SkipWs();
		if (p + 4 <= end && memcmp(p, "true", 4) == 0)
		{
			p += 4;
			return true;
		}
		if (p + 5 <= end && memcmp(p, "false", 5) == 0)
			p += 5;
		return false;
	}

	UINT ReadUint()
	{
		SkipWs();
		UINT v = 0;
		while (p < end && *p >= '0' && *p <= '9')
			v = v * 10 + (UINT)(*p++ - '0');
		return v;
	}

	// Reads one object into aOut; unknown keys are skipped so the patch can add
	// fields later without breaking the host.
	bool ReadItem(MenuItemData &aOut)
	{
		if (!Expect('{'))
			return false;
		for (;;)
		{
			SkipWs();
			if (p < end && *p == '}')
			{
				++p;
				return true;
			}
			std::wstring key = ReadString();
			if (!Expect(':'))
				return false;
			SkipWs();
			if (key == L"id")
				aOut.id = ReadUint();
			else if (key == L"text")
				aOut.text = ReadString();
			else if (key == L"separator")
				aOut.separator = ReadBool();
			else if (key == L"checked")
				aOut.checked = ReadBool();
			else if (key == L"enabled")
				aOut.enabled = ReadBool();
			else if (key == L"radio")
				aOut.radio = ReadBool();
			else if (key == L"default")
				aOut.def = ReadBool();
			else if (key == L"items")
				ReadItems(aOut.items);
			else
				SkipValue();
			SkipWs();
			if (p < end && *p == ',')
			{
				++p;
				continue;
			}
		}
	}

	void ReadItems(std::vector<MenuItemData> &aOut)
	{
		if (!Expect('['))
			return;
		for (;;)
		{
			SkipWs();
			if (p >= end)
				return;
			if (*p == ']')
			{
				++p;
				return;
			}
			MenuItemData item;
			if (ReadItem(item))
				aOut.push_back(std::move(item));
			SkipWs();
			if (p < end && *p == ',')
				++p;
		}
	}

	void SkipValue()
	{
		SkipWs();
		if (p >= end)
			return;
		if (*p == '"')
		{
			(void)ReadString();
			return;
		}
		if (*p == '{' || *p == '[')
		{
			char open = *p, close = open == '{' ? '}' : ']';
			int depth = 0;
			while (p < end)
			{
				if (*p == '"')
				{
					(void)ReadString();
					continue;
				}
				if (*p == open)
					++depth;
				else if (*p == close && --depth == 0)
				{
					++p;
					return;
				}
				++p;
			}
			return;
		}
		while (p < end && *p != ',' && *p != '}')
			++p;
	}
};

HMENU BuildMenuTree(const std::vector<MenuItemData> &aItems)
{
	HMENU menu = CreatePopupMenu();
	if (!menu)
		return nullptr;
	int index = 0;
	for (const MenuItemData &item : aItems)
	{
		if (item.separator)
		{
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			++index;
			continue;
		}
		HMENU sub = nullptr;
		UINT flags = MF_STRING;
		if (!item.items.empty())
		{
			sub = BuildMenuTree(item.items);
			if (!sub)
				continue;
			flags |= MF_POPUP;
		}
		if (!item.enabled)
			flags |= MF_GRAYED;
		if (item.checked)
			flags |= MF_CHECKED;
		if (sub)
			AppendMenuW(menu, flags, (UINT_PTR)sub, item.text.c_str());
		else if (item.id)
			AppendMenuW(menu, flags, item.id, item.text.c_str());
		else
			AppendMenuW(menu, flags | MF_GRAYED, 0, item.text.c_str());
		if (item.radio)
		{
			// Radio appearance is a TYPE bit, not an AppendMenu flag: the bullet
			// only renders when the item is also checked.
			MENUITEMINFOW mi = {sizeof(mi)};
			mi.fMask = MIIM_FTYPE;
			mi.fType = MFT_RADIOCHECK;
			SetMenuItemInfoW(menu, (UINT)index, TRUE, &mi);
		}
		if (item.def)
			SetMenuDefaultItem(menu, (UINT)index, TRUE);
		++index;
	}
	return menu;
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
	if (!mKernelPath.empty())
		return true;

	wchar_t buf[MAX_PATH] = {0};
	if (GetEnvironmentVariableW(L"AHKDECK_KERNEL", buf, MAX_PATH) > 0)
	{
		mKernelPath = buf;
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
		mKernelPath = dir + L"AutoHotkey64.exe";
	}

	if (GetFileAttributesW(mKernelPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		aError = "AutoHotkey64.exe was not found (missing next to AHKDeck.exe or blocked)";
		mKernelPath.clear();
		return false;
	}

	// m051: portable layout - logs live next to the exe, never in the process
	// cwd or %LOCALAPPDATA%. Resolve while the host owns the path; a script
	// child inherits the cwd but the host's own paths are already absolute.
	// m177: they live under the one temp\ root; see core/AppPaths.h.
	mLogRoot = AppTempDir(kTempLogs);
	if (!mLogRoot.empty())
	{
		// m106: the cap is an env knob only (no settings entry): the gate runs
		// with a tiny value, a user never has to know it exists.
		wchar_t capText[32] = {0};
		const DWORD capLen = GetEnvironmentVariableW(L"AHKDECK_LOG_CAP", capText, 32);
		if (capLen > 0 && capLen < 32)
		{
			const long long parsed = _wtoi64(capText);
			if (parsed >= 64 * 1024 && parsed <= 64ll * 1024 * 1024)
				mLogCap = (size_t)parsed;
		}
	}
	return true;
}

void Engine::Unload()
{
	StopAll();

	// Give every child a bounded chance to exit cleanly (it got WM_CLOSE from
	// StopAll), then terminate the leftovers - a hung script must not keep the
	// host alive at exit (the m012 lesson, now cheap to enforce: the child is
	// an object we own, not a thread inside us).
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		for (auto &it : mSessions)
		{
			Entry &entry = it.second;
			if (entry.started && entry.pub.state != SessionState::Finished)
			{
				if (entry.job)
					TerminateJobObject(entry.job, 2);
			}
		}
	}
	std::vector<DWORD> ids;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		for (const auto &it : mSessions)
			ids.push_back(it.first);
	}
	for (DWORD id : ids)
	{
		DWORD rc = 0;
		WaitSession(id, 3000, &rc);
	}

	std::lock_guard<std::mutex> lock(mSessionsMutex);
	for (auto &it : mSessions)
	{
		Entry &entry = it.second;
		if (entry.stdoutThread)
		{
			// The reader exits when its pipe end closes (in ReapLocked or below).
			if (WaitForSingleObject(entry.stdoutThread, 2000) != WAIT_OBJECT_0)
				TerminateThread(entry.stdoutThread, 1);
			CloseHandle(entry.stdoutThread);
			entry.stdoutThread = nullptr;
		}
		if (entry.stdoutRead)
		{
			CloseHandle(entry.stdoutRead);
			entry.stdoutRead = nullptr;
		}
		if (entry.process)
		{
			CloseHandle(entry.process);
			entry.process = nullptr;
		}
		if (entry.job)
		{
			CloseHandle(entry.job);
			entry.job = nullptr;
		}
		if (entry.log.is_open())
			entry.log.close();
	}
	for (auto &it : mMenus)
	{
		if (it.second)
			DestroyMenu(it.second);
	}
	mMenus.clear();
	mSessions.clear();
	mThreadIndex.clear();
	mKernelPath.clear();
}

unsigned __stdcall Engine::StdoutThread(void *aArg)
{
	StdoutCtx *ctx = static_cast<StdoutCtx *>(aArg);
	Engine *engine = ctx->engine;
	const DWORD sessionId = ctx->sessionId;
	delete ctx;

	for (;;)
	{
		HANDLE pipe = nullptr;
		{
			std::lock_guard<std::mutex> lock(engine->mSessionsMutex);
			auto it = engine->mSessions.find(sessionId);
			if (it == engine->mSessions.end())
				return 0; // session reaped/unloaded
			pipe = it->second.stdoutRead;
		}
		if (!pipe)
			return 0; // reaped: read end closed

		char chunk[4096];
		DWORD got = 0;
		if (!ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0)
			return 0; // child gone or handle closed

		std::lock_guard<std::mutex> lock(engine->mSessionsMutex);
		auto it = engine->mSessions.find(sessionId);
		if (it == engine->mSessions.end())
			return 0;
		Entry &entry = it->second;
		entry.stdoutPending.append(chunk, got);
		// FileAppend writes one call as one write; keep whole lines only. A
		// trailing line without '\n' waits for the next read - exactly what a
		// tailing consumer expects.
		for (;;)
		{
			size_t nl = entry.stdoutPending.find('\n');
			if (nl == std::string::npos)
				break;
			std::string line = entry.stdoutPending.substr(0, nl);
			entry.stdoutPending.erase(0, nl + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (!line.empty())
				engine->EnqueueStdout(entry, line);
		}
	}
}

// m061 shape, one truth: the console prints rec.message for kind "stdout" and
// the JSONL stays machine-readable. Runs with mSessionsMutex held (called from
// the reader thread's locked section).
void Engine::EnqueueStdout(Entry &aEntry, const std::string &aLine)
{
	ErrorRecord rec;
	rec.session = aEntry.pub.id;
	rec.thread = (int)aEntry.pid;
	rec.kind = "stdout";
	rec.phase = "runtime";
	rec.message = aLine;
	rec.ts = LocalTimestamp();
	rec.json = "{\"session\":" + std::to_string(aEntry.pub.id) + ",\"ts\":\"" + rec.ts
		+ "\",\"thread\":" + std::to_string(aEntry.pid)
		+ ",\"kind\":\"stdout\",\"message\":\"" + JsonEscape(aLine) + "\"}";
	{
		std::lock_guard<std::mutex> lock(mErrorMutex);
		mErrors.push_back(std::move(rec));
	}
	mErrorCv.notify_one();
}

void Engine::PumpRawFiles()
{
	// Snapshot (id, path, offset) pairs, then read outside the lock: the child
	// keeps appending and must never wait on the host's session lock.
	struct Source
	{
		DWORD id;
		DWORD pid;
		std::wstring path;
		ULONGLONG offset;
	};
	std::vector<Source> sources;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		sources.reserve(mSessions.size());
		for (auto &it : mSessions)
		{
			if (it.second.rawPath.empty())
				continue;
			sources.push_back({it.first, it.second.pid, it.second.rawPath, it.second.rawOffset});
		}
	}

	for (Source &src : sources)
	{
		std::string fresh;
		ULONGLONG newSize = 0;
		if (!ReadFileRange(src.path, src.offset, fresh, newSize))
			continue;

		std::vector<ErrorRecord> records;
		// Only complete lines: a torn tail (child mid-write) waits for the next
		// pump, and the offset stops before it.
		size_t begin = 0;
		for (;;)
		{
			size_t nl = fresh.find('\n', begin);
			if (nl == std::string::npos)
				break;
			std::string line = fresh.substr(begin, nl - begin);
			begin = nl + 1;
			if (line.empty())
				continue;
			// m192: the kernel writes its own (child-internal) thread id into
			// "thread"; the engine's routing vocabulary is the child process
			// id. Stamp the JSON line and the struct field to the same pid, so
			// the JSONL, the console and the UI all see one truth.
			{
				static const char kThreadField[] = "\"thread\":";
				const size_t tp = line.find(kThreadField);
				if (tp != std::string::npos)
				{
					size_t d = tp + sizeof(kThreadField) - 1;
					while (d < line.size() && line[d] >= '0' && line[d] <= '9')
						++d;
					line = line.substr(0, tp) + kThreadField + std::to_string(src.pid)
						+ line.substr(d);
				}
			}
			ErrorRecord rec;
			ParseRecord(line, rec);
			rec.session = src.id;
			rec.thread = (int)src.pid;
			records.push_back(std::move(rec));
		}
		const ULONGLONG consumed = src.offset + begin;

		if (!records.empty() || consumed != src.offset)
		{
			std::lock_guard<std::mutex> lock(mSessionsMutex);
			auto it = mSessions.find(src.id);
			if (it != mSessions.end())
				it->second.rawOffset = consumed;
		}
		if (!records.empty())
		{
			std::lock_guard<std::mutex> lock(mErrorMutex);
			for (auto &rec : records)
				mErrors.push_back(std::move(rec));
			mErrorCv.notify_one();
		}
	}
}

// All callers hold mSessionsMutex.
DWORD Engine::Route(const ErrorRecord &aRecord) const
{
	auto it = mThreadIndex.find((DWORD)aRecord.thread);
	if (it != mThreadIndex.end())
		return it->second;

	// Single-script host run (the --run case): whatever the kernel reported
	// belongs to that session. Records from the per-session raw file already
	// carry their session id, so this only serves stdout records whose pid
	// index entry raced a reaped entry.
	if (mSessions.size() == 1)
		return mSessions.begin()->first;

	// A raw-file record carries its session id, so unattributed records here
	// are rare by construction; newest-first path match is the last resort.
	for (auto riter = mSessions.rbegin(); riter != mSessions.rend(); ++riter)
	{
		const Entry &entry = riter->second;
		if (!entry.started)
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
// flushed: an AI tailing the JSONL must not wait on a buffer the host may never
// close, because the host can be killed - that is the point of requirement 2.
void Engine::WriteLog(const ErrorRecord &aRecord)
{
	std::ofstream *log = &mUnattributedFile;
	std::wstring path;
	Entry *entry = nullptr;

	if (aRecord.session)
	{
		auto it = mSessions.find(aRecord.session);
		if (it != mSessions.end())
		{
			entry = &it->second;
			log = &entry->log;
			path = entry->logFull;
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

	// m106: rotate only per-session files; an unattributed file has no session
	// to grow without bound, so it stays append-only.
	if (entry)
	{
		entry->logBytes += aRecord.json.size() + 1;
		if (entry->logBytes >= mLogCap)
			RotateLog(*entry);
	}
}

// All callers hold mSessionsMutex.
void Engine::RotateLog(Entry &aEntry)
{
	aEntry.log.close();
	const std::wstring oldPath = aEntry.logFull + L".1";
	MoveFileExW(aEntry.logFull.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
	aEntry.log.open(aEntry.logFull.c_str(), std::ios::binary | std::ios::trunc);
	aEntry.logBytes = 0;
	if (!aEntry.log.is_open())
	{
		// Keep the session alive: StartSession already reported the log problem,
		// and the console copy of every record remains the source of truth.
		aEntry.log.open(aEntry.logFull.c_str(), std::ios::binary | std::ios::app);
		return;
	}
	const std::string marker = "{\"session\":" + std::to_string(aEntry.pub.id)
		+ ",\"kind\":\"log\",\"message\":\"log rotated at " + std::to_string(mLogCap)
		+ " bytes; the previous file is kept as .1\"}";
	aEntry.log.write(marker.data(), (std::streamsize)marker.size());
	aEntry.log.put('\n');
	aEntry.log.flush();
	aEntry.logBytes = marker.size() + 1;
}

DWORD Engine::StartSession(const std::wstring &aScriptPath, const std::vector<std::wstring> &aArgs,
	std::string &aError)
{
	if (mKernelPath.empty())
	{
		if (!Load(aError))
		{
			aError = "engine is not loaded: " + aError;
			return 0;
		}
	}

	// The child opens the script path itself (official behaviour: A_ScriptDir,
	// A_ScriptName and #Include all resolve against it). A file that cannot even
	// be opened still gets a session so its record has a home.
	HANDLE probe = CreateFileW(aScriptPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (probe == INVALID_HANDLE_VALUE)
	{
		DWORD sessionId = 0;
		{
			std::lock_guard<std::mutex> lock(mSessionsMutex);
			sessionId = mNextSessionId++;
			Entry entry;
			entry.pub.id = sessionId;
			entry.pub.path = aScriptPath;
			entry.pub.name = StemOf(aScriptPath);
			std::string leaf = std::to_string(sessionId) + "-" + TimestampFileName() + ".jsonl";
			const std::string fileName = entry.pub.name + "-" + leaf;
			entry.pub.logPath = AppTempRelative(kTempLogs, fileName);
			entry.logFull = mLogRoot.empty()
				? WideOf(entry.pub.logPath)
				: mLogRoot + L"\\" + WideOf(fileName);
			CreateDirectoryW(mLogRoot.c_str(), nullptr);
			entry.log.open(entry.logFull.c_str(), std::ios::binary | std::ios::trunc);
			if (!entry.log.is_open())
				aError = "the session log could not be created";
			mSessions[sessionId] = std::move(entry);
		}
		{
			std::lock_guard<std::mutex> lock(mErrorMutex);
			mErrors.push_back(MakeSyntheticRecord(sessionId, "script file could not be opened"));
		}
		mErrorCv.notify_one();

		std::lock_guard<std::mutex> lock(mSessionsMutex);
		Entry &stored = mSessions[sessionId];
		stored.pub.state = SessionState::Finished;
		stored.pub.rc = 2; // CRITICAL_ERROR, as the official engine reports it
		stored.pub.fatal = true;
		return sessionId;
	}
	CloseHandle(probe);

	DWORD sessionId = 0;
	std::wstring rawPath, menuPath;
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
		const std::string fileName = entry.pub.name + "-" + leaf;
		entry.pub.logPath = AppTempRelative(kTempLogs, fileName);
		entry.logFull = mLogRoot.empty()
			? WideOf(entry.pub.logPath) // no cwd captured (Load was skipped): stay relative
			: mLogRoot + L"\\" + WideOf(fileName);
		CreateDirectoryW(mLogRoot.c_str(), nullptr);
		entry.log.open(entry.logFull.c_str(), std::ios::binary | std::ios::trunc);
		if (!entry.log.is_open())
			aError = "the session log could not be created"; // runs anyway: records land on the console

		// Raw error file (kernel patch 2 appends) + menu snapshot target
		// (kernel patch 4 writes on demand), both under the one temp\ root so
		// housekeeping (m177) reaps them with everything else.
		const std::wstring work = AppTempDir(kTempWork);
		CreateDirectoryW(work.c_str(), nullptr);
		const std::wstring stamp = WideOf(std::to_string(sessionId) + "-" + TimestampFileName());
		rawPath = work + L"\\" + WideOf(entry.pub.name) + L"-" + stamp + L"-errors.jsonl";
		menuPath = work + L"\\" + WideOf(entry.pub.name) + L"-" + stamp + L"-menu.json";
		DeleteFileW(rawPath.c_str());
		DeleteFileW(menuPath.c_str());
		entry.rawPath = rawPath;
		entry.menuPath = menuPath;

		mSessions[sessionId] = std::move(entry);
	}

	std::wstring cmd = QuoteArg(mKernelPath) + L" " + QuoteArg(aScriptPath);
	for (const auto &arg : aArgs)
		cmd += L" " + QuoteArg(arg);

	std::vector<wchar_t> envBlock = BuildChildEnvironment(rawPath, menuPath);

	// stdout pipe: the child's FileAppend "*" lands here (m061). The write end
	// is inheritable and handed through STARTF_USESTDHANDLES; the read end
	// stays with us and is served by a blocking reader thread.
	SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
	HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
	if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0))
	{
		stdoutRead = stdoutWrite = nullptr;
	}

	STARTUPINFOW si = {sizeof(si)};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nullptr;
	si.hStdOutput = stdoutWrite;
	si.hStdError = stdoutWrite; // /ErrorStdOut and the loader share the pipe
	PROCESS_INFORMATION pi = {};
	// The env block is Unicode: without CREATE_UNICODE_ENVIRONMENT the loader
	// reads it as ANSI and rejects the interleaved NULs with ERROR_INVALID_PARAMETER.
	const BOOL created = CreateProcessW(mKernelPath.c_str(), cmd.data(), nullptr, nullptr, TRUE,
		CREATE_UNICODE_ENVIRONMENT, envBlock.empty() ? nullptr : envBlock.data(), nullptr, &si, &pi);
	if (stdoutWrite)
		CloseHandle(stdoutWrite); // ours only; the child holds its own

	if (!created)
	{
		const DWORD spawnError = GetLastError();
		if (stdoutRead)
			CloseHandle(stdoutRead);
		{
			std::lock_guard<std::mutex> lock(mErrorMutex);
			mErrors.push_back(MakeSyntheticRecord(sessionId,
				"the interpreter could not be started (win32 error " + std::to_string(spawnError) + ")"));
		}
		mErrorCv.notify_one();

		std::lock_guard<std::mutex> lock(mSessionsMutex);
		Entry &stored = mSessions[sessionId];
		stored.pub.state = SessionState::Finished;
		stored.pub.rc = 2;
		stored.pub.fatal = true;
		return sessionId;
	}

	// Kill-on-close job: the OS reaps every script the moment the host dies -
	// the guarantee the in-process kernel gave for free ("one process image"),
	// rebuilt with one API call.
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
		AssignProcessToJobObject(job, pi.hProcess);
	}

	HANDLE reader = nullptr;
	if (stdoutRead)
	{
		uintptr_t id = _beginthreadex(nullptr, 0, &Engine::StdoutThread,
			new StdoutCtx{this, sessionId}, 0, nullptr);
		reader = reinterpret_cast<HANDLE>(id);
	}

	std::lock_guard<std::mutex> lock(mSessionsMutex);
	Entry &stored = mSessions[sessionId];
	stored.started = true;
	stored.process = pi.hProcess;
	stored.job = job;
	stored.pid = pi.dwProcessId;
	stored.stdoutRead = stdoutRead;
	stored.stdoutThread = reader;
	stored.pub.threadId = pi.dwProcessId; // m192: the "thread" is the child process
	stored.pub.state = SessionState::Running;
	mThreadIndex[pi.dwProcessId] = sessionId;
	// The index entry is never pruned: records can still arrive for a session
	// whose process was reaped, and finished sessions stay in the table anyway.
	return sessionId;
}

int Engine::ValidateFile(const std::wstring &aScriptPath, std::string &aError,
	std::vector<ErrorRecord> &aRecords)
{
	aRecords.clear();
	if (!Load(aError))
		return 1;

	// The kernel's own switch: LoadFromFile runs, InitForExecution never does.
	// m192: a plain child process instead of the fresh-process Main() call the
	// DLL era needed - the process boundary IS the fresh process.
	const std::wstring work = AppTempDir(kTempWork);
	CreateDirectoryW(work.c_str(), nullptr);
	const std::wstring rawPath = work + L"\\validate-" + WideOf(TimestampFileName())
		+ L"-" + WideOf(std::to_string(GetCurrentProcessId())) + L".jsonl";
	DeleteFileW(rawPath.c_str());

	std::vector<wchar_t> envBlock = BuildChildEnvironment(rawPath, rawPath); // menu never asked
	std::wstring cmd = QuoteArg(mKernelPath) + L" /validate " + QuoteArg(aScriptPath);

	SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
	HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
	CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0);
	STARTUPINFOW si = {sizeof(si)};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = stdoutWrite;
	si.hStdError = stdoutWrite;
	PROCESS_INFORMATION pi = {};
	const BOOL created = CreateProcessW(mKernelPath.c_str(), cmd.data(), nullptr, nullptr, TRUE,
		CREATE_UNICODE_ENVIRONMENT, envBlock.data(), nullptr, &si, &pi);
	if (stdoutWrite)
		CloseHandle(stdoutWrite);
	if (!created)
	{
		if (stdoutRead)
			CloseHandle(stdoutRead);
		aError = "the interpreter could not be started";
		return 1;
	}

	// Drain the pipe while the child runs: the pipe buffer is small, and a
	// script full of load errors must not wedge the child on a full pipe.
	std::string output;
	char chunk[4096];
	DWORD got = 0;
	for (;;)
	{
		if (!ReadFile(stdoutRead, chunk, sizeof(chunk), &got, nullptr) || got == 0)
			break;
		output.append(chunk, got);
	}
	CloseHandle(stdoutRead);

	WaitForSingleObject(pi.hProcess, 15000);
	DWORD rc = 2;
	GetExitCodeProcess(pi.hProcess, &rc);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	// Load errors come back through the same JSON records a session would
	// produce - parse them here, without log routing (there is no session).
	std::string blob;
	ULONGLONG size = 0;
	ReadFileRange(rawPath, 0, blob, size);
	DeleteFileW(rawPath.c_str());
	size_t begin = 0;
	while (begin < blob.size())
	{
		size_t nl = blob.find('\n', begin);
		if (nl == std::string::npos)
			break;
		std::string line = blob.substr(begin, nl - begin);
		begin = nl + 1;
		if (line.empty())
			continue;
		ErrorRecord rec;
		ParseRecord(line, rec);
		aRecords.push_back(std::move(rec));
	}
	(void)output; // /ErrorStdOut text duplicates the JSON; the JSON is the truth
	return (int)rc;
}

// All callers hold mSessionsMutex. A session is Finished only once its child
// process has actually been reaped: the UI polls Sessions() and must never be
// told "running" about a process that is already gone.
void Engine::ReapLocked(Entry &aEntry) const
{
	if (!aEntry.started || !aEntry.process || aEntry.pub.state == SessionState::Finished)
		return;
	if (WaitForSingleObject(aEntry.process, 0) != WAIT_OBJECT_0)
		return;
	DWORD rc = 0;
	if (!GetExitCodeProcess(aEntry.process, &rc))
		return; // cannot read the code: keep the state honest instead of guessing

	// Stop the stdout reader first: it blocks on the pipe, and closing the read
	// end makes ReadFile fail and the thread exit. A torn partial line still
	// flushes - it is whatever the script printed last.
	if (aEntry.stdoutRead)
		CloseHandle(aEntry.stdoutRead);
	aEntry.stdoutRead = nullptr;
	if (aEntry.stdoutThread)
	{
		// The thread may still hold mSessionsMutex in its final iteration; it
		// never blocks on it twice, so a bounded wait outside our own lock
		// cannot deadlock. We hold the lock, so keep this short and forgiving.
		WaitForSingleObject(aEntry.stdoutThread, 1000);
		CloseHandle(aEntry.stdoutThread);
		aEntry.stdoutThread = nullptr;
	}

	CloseHandle(aEntry.process);
	aEntry.process = nullptr;
	if (aEntry.job)
	{
		CloseHandle(aEntry.job); // child already gone; the job object dies here
		aEntry.job = nullptr;
	}
	// m132: the host-built tray menu belongs to this session; it must not
	// outlive it (the map lookup is safe because the id is unique forever).
	{
		auto menu = mMenus.find(aEntry.pub.id);
		if (menu != mMenus.end())
		{
			if (menu->second)
				DestroyMenu(menu->second);
			mMenus.erase(menu);
		}
	}
	aEntry.pub.state = SessionState::Finished;
	aEntry.pub.rc = rc;
	// m027: the child is gone, so no more records can be produced by it. Close
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
	// Polling a session reaps it: reading the state is what notices the child is
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
		handle = it->second.process;
	}

	if (!handle)
		return false; // started but unopenable: never report a code we cannot read

	if (WaitForSingleObject(handle, aTimeoutMs) != WAIT_OBJECT_0)
		return false; // still running; keep the entry
	DWORD rc = 0;
	if (!GetExitCodeProcess(handle, &rc))
		return false; // handle lacks query access; never report a guessed code

	std::lock_guard<std::mutex> lock(mSessionsMutex);
	auto it = mSessions.find(aSessionId);
	if (it != mSessions.end())
		ReapLocked(it->second);
	if (aExitCode)
		*aExitCode = rc;
	return true;
}

HWND Engine::FindScriptWindow(DWORD aSessionId) const
{
	DWORD pid = 0;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it == mSessions.end() || !it->second.started)
			return nullptr;
		if (it->second.pub.state == SessionState::Finished)
			return nullptr; // its window is gone; a reused pid must not be followed
		pid = it->second.pid;
	}
	if (!pid)
		return nullptr;

	// Exact class + exact process: a second AHK on this machine has a window of
	// the same class, and a dead child's pid can be recycled - both sides must
	// agree before anything is posted to this window.
	struct Search
	{
		DWORD pid;
		HWND found;
	} search = {pid, nullptr};
	EnumWindows([](HWND aHwnd, LPARAM aParam) -> BOOL {
		Search *search = reinterpret_cast<Search *>(aParam);
		DWORD windowPid = 0;
		GetWindowThreadProcessId(aHwnd, &windowPid);
		if (windowPid != search->pid)
			return TRUE;
		wchar_t className[64] = {0};
		if (GetClassNameW(aHwnd, className, 64) == 0 || wcscmp(className, kScriptWindowClass) != 0)
			return TRUE;
		search->found = aHwnd;
		return FALSE;
	}, reinterpret_cast<LPARAM>(&search));
	return search.found;
}

// m132: the script's live tray menu, rebuilt host-side from the JSON snapshot.
// Read-only: the HMENU built here belongs to THIS process and is destroyed on
// the next request for the same session or when the session goes away.
HMENU Engine::TrayMenu(DWORD aSessionId) const
{
	std::wstring menuPath;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it == mSessions.end() || !it->second.started)
			return nullptr;
		if (it->second.pub.state == SessionState::Finished)
			return nullptr;
		menuPath = it->second.menuPath;
	}
	if (menuPath.empty())
		return nullptr;

	// The snapshot is written by the child's main window when it pumps
	// AHKDECK_DUMP_MENU. A script busy in a long operation cannot pump - give
	// it a moment and then give up for this tick (the next tray open retries).
	HWND window = FindScriptWindow(aSessionId);
	if (window)
	{
		DeleteFileW(menuPath.c_str());
		DWORD_PTR result = 0;
		SendMessageTimeoutW(window, kDeckDumpMenu, 0, 0, SMTO_ABORTIFHUNG, 500, &result);
	}

	std::string blob;
	if (!ReadWholeFile(menuPath, blob) || blob.size() < 8)
	{
		// The child did not answer (busy, or gone): no snapshot this tick.
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mMenus.find(aSessionId);
		if (it != mMenus.end() && it->second)
			DestroyMenu(it->second);
		mMenus.erase(aSessionId);
		return nullptr;
	}

	MenuParser parser;
	// The patch writes {"items":[...]}; parse the array inside the object.
	static const char kItemsKey[] = "\"items\":";
	const size_t itemsAt = blob.find(kItemsKey);
	if (itemsAt == std::string::npos)
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mMenus.find(aSessionId);
		if (it != mMenus.end() && it->second)
			DestroyMenu(it->second);
		mMenus.erase(aSessionId);
		return nullptr;
	}
	parser.p = blob.c_str() + itemsAt + sizeof(kItemsKey) - 1;
	parser.end = blob.c_str() + blob.size();
	std::vector<MenuItemData> items;
	parser.ReadItems(items);

	// An EMPTY items array is a valid answer: a script that did
	// A_TrayMenu.Delete() (m158/m160) owns an empty tray menu, and the deck's
	// UI guard keys on GetMenuItemCount() == 0. Only a missing/broken snapshot
	// is "no menu".
	std::lock_guard<std::mutex> lock(mSessionsMutex);
	auto old = mMenus.find(aSessionId);
	if (old != mMenus.end() && old->second)
		DestroyMenu(old->second);
	HMENU menu = BuildMenuTree(items);
	if (menu)
		mMenus[aSessionId] = menu;
	else
		mMenus.erase(aSessionId);
	return menu;
}

bool Engine::StopSession(DWORD aSessionId)
{
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it == mSessions.end() || !it->second.started)
			return false; // unknown, or already finished (also the load-failure case)
		if (it->second.pub.state == SessionState::Finished)
			return false;
		it->second.pub.state = SessionState::Stopping;
	}

	// The kernel's own close path: WM_CLOSE on the main window runs OnExit and
	// exits cleanly - the same thing clicking the tray Exit item does. A script
	// sitting in a modal/menu loop can accept the post and ignore it, but the
	// job object guarantees the child cannot outlive the host, and a user who
	// really wants it dead still has the task manager (unchanged from the
	// in-process engine, which could also only *ask* a stuck thread).
	if (HWND hwnd = FindScriptWindow(aSessionId))
	{
		DWORD_PTR result = 0;
		if (SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 500, &result))
			return true;
	}

	// No window to close (or it is hung): the job object is the last resort.
	HANDLE job = nullptr;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		auto it = mSessions.find(aSessionId);
		if (it != mSessions.end() && it->second.job)
			job = it->second.job;
	}
	if (job)
	{
		TerminateJobObject(job, 2); // EXIT_CRITICAL, the official fatal code
		return true;
	}
	return false;
}

size_t Engine::StopAll()
{
	std::vector<DWORD> sessionIds;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		for (auto &it : mSessions)
		{
			if (!it.second.started || it.second.pub.state == SessionState::Finished)
				continue;
			it.second.pub.state = SessionState::Stopping;
			sessionIds.push_back(it.first);
		}
	}
	size_t signalled = 0;
	for (DWORD id : sessionIds)
	{
		if (StopSession(id))
			++signalled;
	}
	return signalled;
}

size_t Engine::PendingRecords() const
{
	const_cast<Engine *>(this)->PumpRawFiles();
	std::lock_guard<std::mutex> lock(mErrorMutex);
	return mErrors.size();
}

// Routes one record to its session and writes it to the session's JSONL.
// The caller holds mSessionsMutex. Returns the session that must be asked to
// stop, or 0.
//
// The record keeps its "logged" flag: a tool may have flushed the log before
// the UI got here, and the file must not receive the same line twice.
DWORD Engine::PublishRecord(ErrorRecord &aRecord)
{
	DWORD stopSession = 0;
	if (!aRecord.session)
		aRecord.session = Route(aRecord);
	StampSession(aRecord);
	if (!aRecord.logged)
	{
		WriteLog(aRecord);
		aRecord.logged = true;
	}
	if (aRecord.session && (aRecord.severity == "fail" || aRecord.severity == "critical"))
	{
		auto it = mSessions.find(aRecord.session);
		if (it != mSessions.end())
		{
			it->second.pub.fatal = true;
			// An unhandled failure is the official dialog's "Exit": the script
			// stops. With the dialog patched away nobody would ever see the
			// error but the log - stopping the session is what the user would
			// have clicked, and it is the only way to keep a repeating timer's
			// error storm out of the log an AI is about to read (m011).
			if (it->second.started && it->second.pub.state == SessionState::Running)
			{
				it->second.pub.state = SessionState::Stopping;
				stopSession = it->second.pub.id;
			}
		}
	}
	return stopSession;
}

size_t Engine::FlushPendingLogs()
{
	// Lock order is mSessionsMutex -> mErrorMutex, the same one StartSession
	// uses when it queues a record from inside the session table. Taking them
	// the other way round would deadlock the two against each other.
	//
	// Sessions to stop are collected and acted on AFTER the locks drop:
	// StopSession takes mSessionsMutex itself, and this function holds it.
	PumpRawFiles();
	std::vector<DWORD> toStop;
	size_t written = 0;
	{
		std::lock_guard<std::mutex> lock(mSessionsMutex);
		std::lock_guard<std::mutex> errorLock(mErrorMutex);
		for (auto &rec : mErrors)
		{
			if (rec.logged)
				continue;
			const DWORD stopSession = PublishRecord(rec);
			if (stopSession)
				toStop.push_back(stopSession);
			++written;
		}
	}
	for (DWORD id : toStop)
		StopSession(id);
	return written;
}

bool Engine::PopRecord(ErrorRecord &aRecord, DWORD aTimeoutMs)
{
	// The child appends asynchronously: pump before every look, and while
	// waiting keep the pump alive on a short slice (a record can arrive between
	// the cv wake-up and the next check; the slice keeps it bounded).
	const ULONGLONG deadline = aTimeoutMs == INFINITE
		? ULONGLONG(-1) : GetTickCount64() + aTimeoutMs;
	for (;;)
	{
		PumpRawFiles();

		ErrorRecord rec;
		bool have = false;
		{
			std::unique_lock<std::mutex> lock(mErrorMutex);
			if (!mErrors.empty())
			{
				rec = std::move(mErrors.front());
				mErrors.pop_front();
				have = true;
			}
		}
		if (have)
		{
			// Routing and logging happen here, once: console, journal and (later)
			// the UI all read the same session id and the same JSONL.
			DWORD stopSession = 0;
			{
				std::lock_guard<std::mutex> lock(mSessionsMutex);
				stopSession = PublishRecord(rec);
			}
			if (stopSession)
				StopSession(stopSession);
			aRecord = std::move(rec);
			return true;
		}

		const ULONGLONG now = GetTickCount64();
		if (now >= deadline)
			return false;
		const DWORD slice = deadline == ULONGLONG(-1) ? 50
			: (DWORD)std::min<ULONGLONG>(50, deadline - now);
		std::unique_lock<std::mutex> lock(mErrorMutex);
		mErrorCv.wait_for(lock, std::chrono::milliseconds(slice));
	}
}

} // namespace ahkdeck

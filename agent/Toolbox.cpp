#include "agent/Toolbox.h"

#include "core/TextFile.h"
#include "engine/DeckEngine.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <utility>

namespace ahkdeck
{
namespace agent
{
namespace
{

std::string Utf8Of(const std::wstring &aWide)
{
	if (aWide.empty())
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring WideOf(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), out.data(), n);
	return out;
}

std::wstring LeafOf(const std::wstring &aPath)
{
	const size_t p = aPath.find_last_of(L"\\/");
	return p == std::wstring::npos ? aPath : aPath.substr(p + 1);
}

// A kernel record reaches its session's JSONL when the UI tick drains the
// engine's record queue: the console write and the log write happen together,
// in one place, so the two can never disagree. A tool that reads the log
// cannot depend on that tick - the UI may be busy, and the reader would then
// see a file that is still empty and conclude the script printed nothing
// (m083). So the reader writes the pending records itself.
void WaitForLogDrain()
{
	Engine &engine = Engine::Instance();
	const ULONGLONG start = GetTickCount64();
	int quiet = 0;
	for (;;)
	{
		const size_t written = engine.FlushPendingLogs();
		if (written == 0)
		{
			if (++quiet >= 5)
				break; // nothing new to write for 50 ms
		}
		else
		{
			quiet = 0;
		}
		if (GetTickCount64() - start > 2000)
			break;
		Sleep(10);
	}
}

std::vector<std::string> SplitLines(const std::string &aText);
std::string RedactPaths(const std::string &aJsonl);

bool SamePathI(const std::wstring &aLeft, const std::wstring &aRight)
{
	if (aLeft.size() != aRight.size())
		return false;
	for (size_t i = 0; i < aLeft.size(); ++i)
	{
		const wchar_t l = aLeft[i] == L'/' ? L'\\' : aLeft[i];
		const wchar_t r = aRight[i] == L'/' ? L'\\' : aRight[i];
		if (towlower(l) != towlower(r))
			return false;
	}
	return true;
}

// m062: a second trial of the same path must not inherit the first trial's
// hooks and hotkeys. Stopping the old copy first is what a human does before
// re-running from the tray; doing it here keeps the two runs independent.
void StopCopiesOf(Engine &aEngine, const std::wstring &aPath)
{
	for (const Session &session : aEngine.Sessions())
	{
		if (session.state == SessionState::Finished || !SamePathI(session.path, aPath))
			continue;
		aEngine.StopSession(session.id);
		DWORD ignored = 0;
		aEngine.WaitSession(session.id, 1000, &ignored);
	}
}

// m062: the machine-readable verdict. A finished run is pass/fail by its
// official rc and the kernel's own "fail" record. A run that outlived the
// timeout is resident when its script window still answers Windows messages
// (a hotkey deck, a tray tool) and timeout when it cannot (a busy loop).
const char *VerdictOf(Engine &aEngine, DWORD aSessionId, bool aFinished,
	DWORD aRc, const Session &aSession)
{
	if (aFinished)
		return (aSession.fatal || aRc != 0) ? "fail" : "pass";
	HWND window = aEngine.FindScriptWindow(aSessionId);
	if (window)
	{
		DWORD_PTR answered = 0;
		if (SendMessageTimeoutW(window, WM_NULL, 0, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &answered) != 0)
			return "resident";
	}
	return "timeout";
}

// m159: the verdict says what happened; this says what to do about it. A
// caller (a model especially) should not have to infer the next tool call
// from a label it has never seen. pass returns "" - no next field at all.
const char *NextStepOf(const std::string &aVerdict)
{
	if (aVerdict == "fail")
		return "先用 get_session_output 读完整输出，按 errors[].line 用 patch_editor 修，再运行复验。";
	if (aVerdict == "timeout")
		return "脚本超时且窗口无响应：先 get_session_output 读已有输出，再 stop_script。";
	if (aVerdict == "resident")
		return "脚本常驻：用 get_session_output 读输出；需要结束时 stop_script。";
	return "";
}

// m062: the last ten kernel error records as objects, so a caller does not
// have to regex the prose (or parse the JSONL itself) to learn the line number.
Json ErrorsFromLog(const std::string &aLog)
{
	Json errors = Json::MakeArray();
	const std::vector<std::string> lines = SplitLines(aLog);
	int taken = 0;
	for (int i = (int)lines.size() - 1; i >= 0 && taken < 10; --i)
	{
		Json record;
		std::string parseError;
		if (!Json::Parse(lines[(size_t)i], record, parseError))
			continue;
		if (record["kind"].AsString() != "error")
			continue;
		Json entry = Json::MakeObject();
		entry.Set("phase", record["phase"]);
		// m062: the errors array is a payload like any other - no machine path
		// may reach the caller through it (m010's redaction rule).
		std::string file = record["file"].AsString();
		const size_t slash = file.find_last_of("\\/");
		if (slash != std::string::npos)
			file = file.substr(slash + 1);
		entry.Set("message", Json(RedactPaths(record["message"].AsString())));
		entry.Set("file", Json(file));
		entry.Set("line", Json((double)record["line"].AsInt(0)));
		errors.Push(entry);
		++taken;
	}
	return errors;
}

// m062: where the editing area's text runs from. A saved, unchanged buffer
// runs from its own file (A_ScriptDir and #Include then match the user's run).
// Unsaved edits run from a sibling preview file: same directory, so paths stay
// identical, but the user's file is never touched.
struct EditorRunPath
{
	std::wstring path;
	std::string mode; // "real" | "sibling" | "temp"
};

EditorRunPath ResolveEditorRunPath(const std::wstring &aRealPath,
	const std::string &aText, const std::wstring &aTempDir)
{
	if (!aRealPath.empty())
	{
		TextDocument disk;
		std::string readError;
		if (ReadTextDocument(aRealPath, disk, readError) && disk.utf8 == aText)
			return {aRealPath, "real"};
		const size_t slash = aRealPath.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			return {aRealPath.substr(0, slash) + L"\\AHKDeck-preview.ahk", "sibling"};
	}
	return {aTempDir + L"\\ai-editor.ahk", "temp"};
}

std::string Tail(const std::string &aText, size_t aLimit)
{
	if (aText.size() <= aLimit)
		return aText;
	return "[...truncated...]\n" + aText.substr(aText.size() - aLimit);
}

// m101: line counts without inventing a trailing empty line, so a script that
// ends with "\n" reports the lines the user sees in the editor.
std::vector<std::string> SplitLines(const std::string &aText)
{
	std::vector<std::string> lines;
	size_t pos = 0;
	while (pos < aText.size())
	{
		size_t end = aText.find('\n', pos);
		if (end == std::string::npos)
			end = aText.size();
		std::string line = aText.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(std::move(line));
		if (end == aText.size())
			break;
		pos = end + 1;
	}
	return lines;
}

// m101: what a writing tool changed, in the terms an AI can act on. Common
// prefix/suffix comparison is enough here: the caller wants to know whether
// anything moved and where to re-read, not a full diff.
struct ChangeSummary
{
	bool changed = false;
	int linesBefore = 0;
	int linesAfter = 0;
	int firstChanged = 0; // 1-based, 0 when nothing changed
	int lastChanged = 0;
};

ChangeSummary CompareText(const std::string &aBefore, const std::string &aAfter)
{
	const std::vector<std::string> before = SplitLines(aBefore);
	const std::vector<std::string> after = SplitLines(aAfter);

	ChangeSummary summary;
	summary.linesBefore = (int)before.size();
	summary.linesAfter = (int)after.size();

	size_t prefix = 0;
	while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix])
		++prefix;

	// The suffix must not overlap the prefix, or a pure insertion would claim
	// the inserted block was already there.
	size_t suffix = 0;
	while (suffix < before.size() - prefix && suffix < after.size() - prefix
		&& before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix])
		++suffix;

	if (prefix == before.size() && prefix == after.size())
		return summary; // identical

	summary.changed = true;
	summary.firstChanged = (int)prefix + 1;
	const int lastAfter = (int)(after.size() - suffix);
	summary.lastChanged = lastAfter > summary.firstChanged ? lastAfter : summary.firstChanged;
	return summary;
}

void DescribeChange(Json &aResult, const ChangeSummary &aSummary)
{
	aResult.Set("changed", Json(aSummary.changed));
	aResult.Set("lines_before", Json((double)aSummary.linesBefore));
	aResult.Set("lines_after", Json((double)aSummary.linesAfter));
	aResult.Set("first_changed_line", Json((double)aSummary.firstChanged));
	aResult.Set("last_changed_line", Json((double)aSummary.lastChanged));
}

// m102: static checks with no parser and no execution. The aim is to catch the
// v1 syntax an AI slips into before it costs a run and a session; a false
// positive costs more than it saves, so every rule here is narrow.
struct LintCounts
{
	int error = 0;
	int warning = 0;
};

const size_t kLintPerRule = 10;
const size_t kLintTotal = 40;

void AddLintFinding(Json &aFindings, LintCounts &aCounts, int aLine, const char *aSeverity,
	const char *aRule, const std::string &aMessage)
{
	if (aFindings.Size() >= kLintTotal)
		return;
	Json item = Json::MakeObject();
	item.Set("line", Json((double)aLine));
	item.Set("severity", Json(aSeverity));
	item.Set("rule", Json(aRule));
	item.Set("message", Json(aMessage));
	aFindings.Push(std::move(item));
	if (std::strcmp(aSeverity, "error") == 0)
		++aCounts.error;
	else
		++aCounts.warning;
}

bool IsLintWordChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsLintIdentStart(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

std::string LowerAscii(const std::string &aText)
{
	std::string out = aText;
	for (auto &c : out)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return out;
}

// m102: a patch is worth having only if an ambiguous match is refused, so the
// count stops at two - one hit is a patch, two is a question for the caller.
size_t CountOccurrences(const std::string &aText, const std::string &aNeedle, size_t aLimit)
{
	size_t count = 0;
	size_t pos = 0;
	while (count < aLimit)
	{
		pos = aText.find(aNeedle, pos);
		if (pos == std::string::npos)
			break;
		++count;
		pos += aNeedle.size();
	}
	return count;
}

// Insert after a 1-based line, keeping the document's own line endings. The
// raw-string walk avoids rebuilding the whole text and normalising CRLF to LF.
bool InsertAfterLineRaw(std::string &aText, int aLine, const std::string &aNew, std::string &aError)
{
	const std::vector<std::string> lines = SplitLines(aText);
	if (aLine < 1 || (size_t)aLine > lines.size())
	{
		aError = "line 超出范围（1-" + std::to_string(lines.size()) + "）";
		return false;
	}
	const std::string eol = aText.find("\r\n") != std::string::npos ? "\r\n" : "\n";

	size_t pos = 0;
	for (int current = 1; current < aLine; ++current)
	{
		const size_t nl = aText.find('\n', pos);
		if (nl == std::string::npos)
			break;
		pos = nl + 1;
	}
	const size_t nl = aText.find('\n', pos);
	if (nl == std::string::npos)
		aText += eol + aNew; // the target line has no newline yet
	else
		aText.insert(nl + 1, aNew + eol);
	return true;
}

std::wstring ExeDirectoryForTools()
{
	wchar_t path[MAX_PATH] = {0};
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::wstring();
	std::wstring dir(path, n);
	const size_t slash = dir.find_last_of(L"\\/");
	return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

std::string LeafOfUtf8(const std::string &aPath)
{
	const size_t p = aPath.find_last_of("\\/");
	return p == std::string::npos ? aPath : aPath.substr(p + 1);
}

// m103: validate runs in a helper process. The kernel's validate-only entry is
// its command line ("/validate"), and the in-process COM Main refuses once a
// source file has been loaded - so the deck cannot validate inside itself. The
// helper writes its report to a file rather than a pipe: a pipe would make the
// deck read while the helper writes, and a file removes that deadlock shape.
struct ValidateRun
{
	int rc = -1;        // helper exit code (= the kernel's own code)
	std::string report; // JSONL the helper printed
	std::string error;  // why no report could be produced
};

bool RunValidateProcess(const std::wstring &aScriptPath, ValidateRun &aOut)
{
	aOut = ValidateRun();

	wchar_t exePath[MAX_PATH] = {0};
	if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
	{
		aOut.error = "cannot resolve the host executable path";
		return false;
	}
	std::wstring exeDir(exePath);
	const size_t slash = exeDir.find_last_of(L"\\/");
	exeDir.resize(slash == std::wstring::npos ? 0 : slash);

	const std::wstring tempDir = exeDir + L"\\.temp";
	CreateDirectoryW(tempDir.c_str(), nullptr);

	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t name[96] = {0};
	std::swprintf(name, 96, L"\\validate-%04u%02u%02u-%02u%02u%02u-%lu.out",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		GetCurrentProcessId());
	const std::wstring outPath = tempDir + name;

	SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	const HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (out == INVALID_HANDLE_VALUE)
	{
		aOut.error = "cannot create the validator's report file";
		return false;
	}
	const HANDLE nullIn = CreateFileW(L"NUL", GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

	std::wstring command = L"\"" + std::wstring(exePath) + L"\" --validate \"" + aScriptPath + L"\"";
	std::vector<wchar_t> commandLine(command.begin(), command.end());
	commandLine.push_back(0);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = nullIn == INVALID_HANDLE_VALUE ? out : nullIn;
	si.hStdOutput = out;
	si.hStdError = out;

	PROCESS_INFORMATION pi = {};
	const BOOL started = CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, tempDir.c_str(), &si, &pi);
	CloseHandle(out);
	if (nullIn != INVALID_HANDLE_VALUE)
		CloseHandle(nullIn);

	if (!started)
	{
		aOut.error = "cannot start the validator process";
		DeleteFileW(outPath.c_str());
		return false;
	}
	CloseHandle(pi.hThread);

	const DWORD waited = WaitForSingleObject(pi.hProcess, 15000);
	if (waited == WAIT_TIMEOUT)
	{
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 5000);
		aOut.error = "validator did not finish in 15 s";
	}
	else if (waited == WAIT_OBJECT_0)
	{
		DWORD rc = 0;
		if (GetExitCodeProcess(pi.hProcess, &rc))
			aOut.rc = (int)rc;
	}
	CloseHandle(pi.hProcess);

	if (aOut.error.empty())
	{
		TextDocument document;
		std::string readError;
		if (ReadTextDocument(outPath, document, readError))
			aOut.report = document.utf8;
		else
			aOut.error = "cannot read the validator's report";
	}
	DeleteFileW(outPath.c_str());
	return aOut.error.empty();
}

Json ErrorResult(const std::string &aMessage)
{
	Json result = Json::MakeObject();
	result.Set("ok", Json(false));
	result.Set("error", Json(aMessage));
	return result;
}

Json OkResult()
{
	Json result = Json::MakeObject();
	result.Set("ok", Json(true));
	return result;
}

// m051: portable layout - backups stay next to the exe, never in the source tree.
std::string BackupOf(const std::wstring &aScriptPath, std::string &aDisplay, std::string &aError)
{
	wchar_t exePath[MAX_PATH] = {0};
	const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
	{
		aError = "the exe path could not be resolved; refusing to overwrite without a backup";
		return std::string();
	}
	wchar_t *slash = wcsrchr(exePath, L'\\');
	if (slash)
		*slash = 0;

	SYSTEMTIME st;
	GetLocalTime(&st);
	char stamp[48];
	std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u-%03u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	const std::wstring appDir(exePath);
	const std::wstring backups = appDir + L"\\Backups";
	const std::wstring dir = backups + L"\\" + WideOf(stamp);
	CreateDirectoryW(backups.c_str(), nullptr);
	CreateDirectoryW(dir.c_str(), nullptr);

	const std::wstring target = dir + L"\\" + LeafOf(aScriptPath);
	if (!CopyFileW(aScriptPath.c_str(), target.c_str(), FALSE))
	{
		aError = "the current script could not be backed up";
		return std::string();
	}
	aDisplay = "Backups/" + std::string(stamp) + "/" + Utf8Of(LeafOf(aScriptPath));
	return Utf8Of(target);
}

int ClampTimeout(const Json &aArgs, int aDefault)
{
	int timeoutMs = aArgs["timeout_ms"].IsNumber() ? aArgs["timeout_ms"].AsInt() : aDefault;
	if (timeoutMs < 1000)
		timeoutMs = 1000;
	if (timeoutMs > 20000)
		timeoutMs = 20000;
	return timeoutMs;
}

// m102: script arguments ("commands/xxx.ahk args"). The caller sends JSON
// strings, the engine takes wide chars; both ends are bounded so one tool call
// cannot turn into an unbounded command line.
bool ParseArguments(const Json &aArgs, std::vector<std::wstring> &aOut, std::string &aError)
{
	aOut.clear();
	if (!aArgs.Has("args") || aArgs["args"].IsNull())
		return true;
	const Json &list = aArgs["args"];
	if (!list.IsArray())
	{
		aError = "args 必须是字符串数组";
		return false;
	}
	if (list.Size() > 16)
	{
		aError = "args 最多 16 个";
		return false;
	}
	for (size_t i = 0; i < list.Size(); ++i)
	{
		const Json &item = list[i];
		if (!item.IsString())
		{
			aError = "args 第 " + std::to_string(i + 1) + " 项不是字符串";
			return false;
		}
		const std::string value = item.AsString();
		if (value.size() > 1024)
		{
			aError = "args 第 " + std::to_string(i + 1) + " 项超过 1024 字节";
			return false;
		}
		aOut.push_back(WideOf(value));
	}
	return true;
}

// Rule 9: a machine path must never leave this process. The JSONL on disk keeps
// the kernel's own wording (m010 proved message/extra/file/line byte-for-byte
// against the official dialog, and the pair gate still reads those files), so
// the redaction happens at the tool boundary - the only place where this text is
// handed to somebody else's model. Qoder's own reading of the first MCP run is
// what caught it: the load error quoted
// "file":"E:\\tmp\\...\\.build\\.temp\\ai-editor.ahk" straight to the model.
std::string StripAll(std::string aText, const std::string &aNeedle, const std::string &aMark)
{
	if (aNeedle.empty())
		return aText;
	std::string out;
	out.reserve(aText.size());
	size_t pos = 0;
	for (;;)
	{
		const size_t hit = aText.find(aNeedle, pos);
		if (hit == std::string::npos)
		{
			out.append(aText, pos, std::string::npos);
			return out;
		}
		out.append(aText, pos, hit - pos);
		out += aMark;
		pos = hit + aNeedle.size();
	}
}

std::string LeafOfJsonField(std::string aText, const char *aField)
{
	const std::string key = std::string("\"") + aField + "\":\"";
	size_t pos = 0;
	while ((pos = aText.find(key, pos)) != std::string::npos)
	{
		const size_t start = pos + key.size();
		size_t end = start;
		while (end < aText.size())
		{
			if (aText[end] == '\\' && end + 1 < aText.size())
			{
				end += 2;
				continue;
			}
			if (aText[end] == '"')
				break;
			++end;
		}
		const std::string value = aText.substr(start, end - start);
		std::string plain;
		for (size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] == '\\' && i + 1 < value.size())
				plain += value[++i];
			else
				plain += value[i];
		}
		const size_t slash = plain.find_last_of("\\/");
		const std::string leaf = slash == std::string::npos ? plain : plain.substr(slash + 1);
		aText.replace(start, end - start, leaf);
		pos = start + leaf.size();
	}
	return aText;
}

std::string RedactPaths(const std::string &aJsonl)
{
	const std::string root = Utf8Of(ExeDirectoryForTools());
	std::string out = aJsonl;
	// Three spellings of the same prefix: raw, JSON-escaped (what the payload
	// actually contains), and forward-slash.
	std::string escaped;
	for (const char c : root)
	{
		if (c == '\\')
			escaped += "\\\\";
		else
			escaped += c;
	}
	std::string forward = root;
	for (auto &c : forward)
		if (c == '\\')
			c = '/';

	for (const auto &needle : {escaped, root, forward})
		out = StripAll(out, needle, "<exe>");
	return LeafOfJsonField(std::move(out), "file");
}

} // namespace

void Toolbox::SetPolicy(const AiScriptPolicy &aPolicy)
{
	std::lock_guard<std::mutex> lock(mScriptsMutex);
	mPolicy = aPolicy;
}

void Toolbox::Note(const std::string &aText) const
{
	if (mNoteSink)
		mNoteSink(aText);
}

std::string Toolbox::LiveEditorText() const
{
	return mEditor ? mEditor() : std::string();
}

std::string Toolbox::EditorText()
{
	const std::string live = LiveEditorText();
	if (!mPendingEditor.empty() && live != mPendingEditor)
		return mPendingEditor; // the UI has not drained the edit yet
	mPendingEditor.clear();
	return live;
}

void Toolbox::RefreshScripts()
{
	std::vector<std::wstring> roots;
	std::vector<std::wstring> files;
	{
		std::lock_guard<std::mutex> lock(mScriptsMutex);
		roots = mPolicy.roots;
		files = mPolicy.files;
	}

	// Built off to the side and swapped in under the lock: list_scripts runs on
	// another thread and must never see a half-filled table.
	std::vector<ScriptEntry> found;

	for (const auto &root : roots)
	{
		WIN32_FIND_DATAW data;
		const std::wstring pattern = root + L"\\*.ahk";
		HANDLE find = FindFirstFileW(pattern.c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			found.push_back({std::string(), root + L"\\" + data.cFileName, Utf8Of(data.cFileName)});
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}

	for (const auto &file : files)
	{
		if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
			continue;
		const std::wstring leaf = LeafOf(file);
		bool duplicate = false;
		for (const auto &entry : found)
		{
			if (SamePathI(entry.path, file))
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			found.push_back({std::string(), file, Utf8Of(leaf) + "（用户清单）"});
	}

	std::sort(found.begin(), found.end(),
		[](const ScriptEntry &a, const ScriptEntry &b)
		{
			return _stricmp(a.display.c_str(), b.display.c_str()) < 0;
		});

	std::lock_guard<std::mutex> lock(mScriptsMutex);
	int next = 1;
	for (auto &entry : found)
		entry.id = "S" + std::to_string(next++);
	mScripts = std::move(found);
	mNextScriptId = next;
}

int Toolbox::FindScriptById(const std::string &aId) const
{
	for (size_t i = 0; i < mScripts.size(); ++i)
	{
		if (mScripts[i].id == aId)
			return (int)i;
	}
	return -1;
}

std::string Toolbox::NewScriptId()
{
	return "S" + std::to_string(mNextScriptId++);
}

std::vector<ToolDef> Toolbox::Definitions() const
{
	std::vector<ToolDef> defs;

	// MCP wants real JSON Schema ("properties": {"script": {"type":"string",
	// "description":"..."}}), and OpenAI wants the same shape in `parameters`.
	// Describing a property as a bare string used to be good enough for our own
	// client and Qoder tolerated it, but it is not a schema: a strict client
	// cannot tell the model what to put there. One helper, both dialects.
	auto arg = [](const char *aType, const char *aDescription)
	{
		Json property = Json::MakeObject();
		property.Set("type", Json(aType));
		property.Set("description", Json(aDescription));
		return property;
	};
	// m102: script arguments are an array of strings, not a shell-quoted blob.
	auto argArray = [](const char *aItemType, const char *aDescription)
	{
		Json property = Json::MakeObject();
		property.Set("type", Json("array"));
		Json items = Json::MakeObject();
		items.Set("type", Json(aItemType));
		property.Set("items", std::move(items));
		property.Set("description", Json(aDescription));
		return property;
	};

	{
		ToolDef d;
		d.name = "list_scripts";
		d.description = "列出可操作的脚本（id、名称、字节数、是否在运行）。"
			"任何按 id 操作的工具都要求 id 来自这里：先调用 list_scripts，再用返回的 id；"
			"它每次调用都重新扫描，新建或删除脚本后要再调一次。";
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "read_script";
		d.description = "读取脚本文本、编码与行尾。script 必须是 list_scripts 返回的 id。";
		d.properties.Set("script", arg("string", "脚本 id，来自 list_scripts"));
		d.required.push_back("script");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "edit_editor";
		d.description = "把完整内容写入当前编辑区；只改编辑区、不落盘（不创建文件），"
			"用户检查后自行保存或运行。用户要求写/改脚本时优先使用它。"
			"返回 changed 与改动行号范围，便于只重读变了的部分。";
		d.properties.Set("content", arg("string", "完整的新脚本内容（AutoHotkey v2 语法）"));
		d.required.push_back("content");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "check_editor";
		d.description = "对当前编辑区做纯文本静态检查（不执行、不改编辑区、不落盘）："
			"查 v1 专有命令、v1 命令式写法（如 MsgBox,）、v1 变量引用 %var%、未闭合双引号。"
			"返回 findings（line/severity/rule/message）与 counts；ok=false 表示存在 error 级问题。"
			"注意：这些 error 只是文本启发式、可能误报，不要据此改代码；"
			"语法能否通过一律以 validate_editor 的内核解析结果为准。"
			"运行前先调用它，可以省掉一次必然失败的运行。每类最多 10 条、总共最多 40 条。";
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "validate_editor";
		d.description = "用内核自己的 /validate 模式校验当前编辑区（只解析、不执行、不改编辑区、不落盘）："
			"有语法错时 rc=2，findings 给出 line/severity/message（内核原话，带行号）；"
			"ok=true 表示脚本能被解析、可以运行。比 check_editor 更准（后者只扫 v1 残留文本）；"
			"运行前先 check_editor 快速排查，再用它确认，可省掉一次必然失败的运行。"
			"后台起一个短命进程，耗时约一次运行的零头。";
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "patch_editor";
		d.description = "小补丁编辑当前编辑区（不落盘）：replace / insert_after / delete 按顺序应用。"
			"old_text 必须唯一命中，0 次或多次都整批拒绝、编辑区不变；"
			"preview=true 只返回变更摘要、不改编辑区。返回 applied 与 changed/行号范围。";
		Json items = Json::MakeObject();
		items.Set("type", Json("object"));
		Json itemProps = Json::MakeObject();
		Json opProp = arg("string",
			"replace（替换唯一命中）/ insert_after（在 line 行后插入）/ delete（删除唯一命中）");
		Json opValues = Json::MakeArray();
		opValues.Push(Json("replace"));
		opValues.Push(Json("insert_after"));
		opValues.Push(Json("delete"));
		opProp.Set("enum", std::move(opValues));
		itemProps.Set("op", std::move(opProp));
		itemProps.Set("old_text", arg("string", "replace / delete 要匹配的唯一片段"));
		itemProps.Set("new_text", arg("string", "replace 的替换文本；insert_after 要插入的文本"));
		itemProps.Set("line", arg("integer", "insert_after 的行号（从 1 起）"));
		Json itemRequired = Json::MakeArray();
		itemRequired.Push(Json("op"));
		items.Set("properties", std::move(itemProps));
		items.Set("required", std::move(itemRequired));
		Json editsProp = Json::MakeObject();
		editsProp.Set("type", Json("array"));
		editsProp.Set("items", std::move(items));
		editsProp.Set("description", Json("补丁数组，1-20 项，按顺序应用"));
		d.properties.Set("edits", std::move(editsProp));
		d.properties.Set("preview", arg("boolean", "true 只预览、不改编辑区"));
		d.required.push_back("edits");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "run_editor";
		d.description = "运行当前编辑区内容（无需保存文件），返回会话、退出码和输出；"
			"用于自测和按错误迭代修复。verdict 是 pass/fail/resident/timeout 之一；"
			"errors 是最近最多 10 条内核错误对象（phase/message/file/line），不必再解析散文；"
			"script_dir 说明运行副本的位置：real（编辑区与磁盘一致）、sibling（同目录预览副本，"
			"A_ScriptDir 与保存后一致）、temp。同一脚本再次试跑会先停掉上一份。"
			"脚本不退出时 verdict=resident，用 get_session_output 读输出。"
			"可选 args 作为脚本参数（脚本里用 A_Args 读取）。";
		d.properties.Set("timeout_ms", arg("integer", "最多等待毫秒数，默认 8000，上限 20000"));
		d.properties.Set("args", argArray("string", "脚本参数，最多 16 个，脚本里用 A_Args 读取"));
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "run_script";
		d.description = "在 AHKDeck 宿主内运行脚本并返回会话、退出码与输出（JSONL）。"
			"verdict 是 pass/fail/resident/timeout 之一，errors 是最近最多 10 条内核错误对象。"
			"script 必须是 list_scripts 返回的 id；同一脚本再次运行会先停掉上一份。"
			"脚本不退出时 verdict=resident，先 get_session_output 读已有输出，再决定是否 stop_script。"
			"可选 args 作为脚本参数（脚本里用 A_Args 读取）。";
		d.properties.Set("script", arg("string", "脚本 id，来自 list_scripts"));
		d.properties.Set("timeout_ms", arg("integer", "最多等待毫秒数，默认 8000，上限 20000"));
		d.properties.Set("args", argArray("string", "脚本参数，最多 16 个，脚本里用 A_Args 读取"));
		d.required.push_back("script");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "get_session_output";
		d.description = "读取指定会话已经产生的错误/警告输出。会话仍在运行时（run_script/run_editor 返回 running）"
			"先用它读输出，再决定是否 stop_script。返回 truncated 表示是否还有更早的内容被截掉。"
			"要连续跟读同一个会话时传上次返回的 next_offset 作为 after_offset，只取新增部分；"
			"返回的 rewound=true 表示日志被轮转/截断过，游标已从 0 重新开始。";
		d.properties.Set("session", arg("integer", "会话编号（来自 run_script、run_editor 或界面）"));
		d.properties.Set("max_bytes", arg("integer", "最多返回末尾字节数，默认 32768，上限 65536"));
		d.properties.Set("after_offset", arg("integer",
			"从 JSONL 的第几个字节开始读（用上次返回的 next_offset）；省略则读末尾 max_bytes"));
		d.required.push_back("session");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "stop_script";
		d.description = "停止一个正在运行的会话。停止前先用 get_session_output 读取它已经产生的输出。";
		d.properties.Set("session", arg("integer", "会话编号"));
		d.required.push_back("session");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "list_sessions";
		d.description = "列出本次宿主里所有会话（含已结束的）：编号、脚本名、状态 running/stopping/finished、"
			"退出码 rc、是否出错 fatal。不知道 session 编号时先调用它；它和 list_scripts 不同，"
			"列的是“跑过的这一次”，不是磁盘上的脚本。";
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "stop_all";
		d.description = "把所有还在运行的会话一起停掉（同名脚本的重复副本也只会各停一次）。"
			"返回 stopped（已发送停止的数量）与 still_running（1 秒后仍在的个数）；"
			"需要保留哪个会话的输出，就先 get_session_output 再调它。";
		defs.push_back(std::move(d));
	}

	return defs;
}

Json Toolbox::OpenAiSchema() const
{
	Json tools = Json::MakeArray();
	for (const auto &def : Definitions())
	{
		Json parameters = Json::MakeObject();
		parameters.Set("type", Json("object"));
		parameters.Set("properties", def.properties);
		Json required = Json::MakeArray();
		for (const auto &name : def.required)
			required.Push(Json(name));
		parameters.Set("required", std::move(required));

		Json function = Json::MakeObject();
		function.Set("name", Json(def.name));
		function.Set("description", Json(def.description));
		function.Set("parameters", std::move(parameters));

		Json tool = Json::MakeObject();
		tool.Set("type", Json("function"));
		tool.Set("function", std::move(function));
		tools.Push(std::move(tool));
	}
	return tools;
}

std::string Toolbox::Call(const std::string &aName, const std::string &aArgumentsJson)
{
	std::lock_guard<std::mutex> lock(mCallMutex);
	// m081: cancel is one call's semantics, not a process-wide switch. Without
	// this reset the first cancelled run makes every later run skip its wait
	// loop and report state=running instantly.
	mCancel.store(false);

	Json args;
	std::string parseError;
	if (!aArgumentsJson.empty())
	{
		if (!Json::Parse(aArgumentsJson, args, parseError))
			return ErrorResult("工具参数不是合法 JSON: " + parseError).Dump();
	}
	if (!args.IsObject())
		args = Json::MakeObject();

	if (aName == "list_scripts")
	{
		// Refresh per call, not per turn. The conversation loop enumerates once at
		// the start of a turn; an MCP client has no turn - it may call list_scripts
		// first, and then again after a write_script created a file. Serving the
		// table from a previous turn is how m064's gate found an empty script list
		// while every other tool worked.
		RefreshScripts();
		return ToolListScripts();
	}
	if (aName == "read_script")
		return ToolReadScript(args);
	if (aName == "edit_editor")
		return ToolEditEditor(args);
	if (aName == "check_editor")
		return ToolCheckEditor();
	if (aName == "validate_editor")
		return ToolValidateEditor();
	if (aName == "patch_editor")
		return ToolPatchEditor(args);
	if (aName == "write_script")
		return ToolWriteScript(args); // internal only: not in Definitions() since m056
	if (aName == "run_editor")
		return ToolRunEditor(args);
	if (aName == "run_script")
		return ToolRunScript(args);
	if (aName == "get_session_output")
		return ToolGetSessionOutput(args);
	if (aName == "stop_script")
		return ToolStopScript(args);
	if (aName == "list_sessions")
		return ToolListSessions();
	if (aName == "stop_all")
		return ToolStopAll();
	return ErrorResult("未知工具: " + aName).Dump();
}

std::string Toolbox::ToolListScripts()
{
	Engine &engine = Engine::Instance();
	Json list = Json::MakeArray();
	std::lock_guard<std::mutex> lock(mScriptsMutex);
	for (const auto &entry : mScripts)
	{
		Json item = Json::MakeObject();
		item.Set("id", Json(entry.id));
		item.Set("name", Json(entry.display));

		WIN32_FILE_ATTRIBUTE_DATA data;
		if (GetFileAttributesExW(entry.path.c_str(), GetFileExInfoStandard, &data))
			item.Set("bytes", Json((double)((ULONGLONG)data.nFileSizeHigh << 32 | data.nFileSizeLow)));

		bool running = false;
		for (const auto &session : engine.Sessions())
		{
			if (session.state != SessionState::Finished && SamePathI(session.path, entry.path))
			{
				running = true;
				item.Set("session", Json((double)session.id));
				break;
			}
		}
		item.Set("running", Json(running));
		list.Push(std::move(item));
	}

	Json result = OkResult();
	result.Set("scripts", std::move(list));
	return result.Dump();
}

std::string Toolbox::ToolReadScript(const Json &aArgs)
{
	std::wstring path;
	std::string id;
	std::string display;
	{
		std::lock_guard<std::mutex> lock(mScriptsMutex);
		const int index = FindScriptById(aArgs["script"].AsString());
		if (index < 0)
			return ErrorResult("没有这个脚本 id（先调用 list_scripts）").Dump();
		path = mScripts[(size_t)index].path;
		id = mScripts[(size_t)index].id;
		display = mScripts[(size_t)index].display;
	}

	TextDocument document;
	std::string error;
	if (!ReadTextDocument(path, document, error))
		return ErrorResult("读取失败: " + error).Dump();
	if (document.utf8.size() > 512 * 1024)
		return ErrorResult("脚本超过 512 KB，拒绝读取").Dump();

	Json result = OkResult();
	result.Set("script", Json(id));
	result.Set("name", Json(display));
	result.Set("encoding", Json(TextEncodingName(document.encoding)));
	result.Set("crlf", Json(document.crlf));
	result.Set("content", Json(document.utf8));
	return result.Dump();
}

bool Toolbox::ApplyEditor(const std::string &aContent, const std::string &aNote)
{
	if (!mEditorSink)
		return false;
	mEditorSink(aContent, aNote);
	mPendingEditor = aContent;
	// Give the window a beat to actually show it: a caller that follows with
	// run_editor must not read last tick's buffer. Bounded, because a modal
	// dialog or an open menu can keep the UI from draining its queue at all -
	// in that case EditorText() keeps serving mPendingEditor and says so later.
	for (int i = 0; i < 40 && LiveEditorText() != aContent; ++i)
		Sleep(50);
	return true;
}

std::string Toolbox::ToolEditEditor(const Json &aArgs)
{
	if (!aArgs["content"].IsString())
		return ErrorResult("缺少 content 字符串").Dump();
	const std::string content = aArgs["content"].AsString();
	if (content.size() > 1024 * 1024)
		return ErrorResult("脚本超过 1 MB，拒绝写入编辑区").Dump();

	const std::string before = EditorText();
	if (!ApplyEditor(content, "edit_editor (" + std::to_string(content.size()) + " B，未保存)"))
		return ErrorResult("当前界面没有可写入的编辑区").Dump();

	Json result = OkResult();
	result.Set("bytes", Json((double)content.size()));
	result.Set("saved", Json(false));
	result.Set("message", Json("已写入当前编辑区，尚未保存；请用户检查后保存或运行"));
	DescribeChange(result, CompareText(before, content));
	// m065: the built-in loop's "edit implies run, failure implies forced re-edit"
	// policy (m059/m060) is gone with it. An MCP client calls run_editor itself;
	// the tool never runs something the caller did not ask for.
	return result.Dump();
}

std::string Toolbox::ToolCheckEditor()
{
	const std::vector<std::string> lines = SplitLines(EditorText());

	// Only spellings that cannot work under v2. Anything ambiguous stays out:
	// the caller has to be able to trust an "error" here.
	static const char *kV1Only[] = {
		"#noenv", "setbatchlines", "ifequal", "ifnotequal", "stringreplace",
		"stringsplit", "splashtexton", "setformat", "fileencoding"};
	static const char *kV1Command[] = {
		"msgbox", "gui", "fileread", "winget", "winset", "send",
		"sleep", "run", "progress", "splashimage", "soundbeep"};
	const size_t kOnlyCount = sizeof(kV1Only) / sizeof(kV1Only[0]);
	const size_t kCommandCount = sizeof(kV1Command) / sizeof(kV1Command[0]);

	Json findings = Json::MakeArray();
	LintCounts counts;
	size_t perOnly[sizeof(kV1Only) / sizeof(kV1Only[0])] = {0};
	size_t perCommand[sizeof(kV1Command) / sizeof(kV1Command[0])] = {0};
	size_t percentCount = 0;
	size_t quoteCount = 0;

	bool inBlockComment = false;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		const int lineNo = (int)i + 1;
		const std::string &line = lines[i];

		size_t start = 0;
		while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
			++start;
		const std::string trimmed = line.substr(start);

		if (inBlockComment)
		{
			if (trimmed.find("*/") != std::string::npos)
				inBlockComment = false;
			continue;
		}
		if (trimmed.compare(0, 2, "/*") == 0)
		{
			if (trimmed.find("*/", 2) == std::string::npos)
				inBlockComment = true;
			continue;
		}
		if (trimmed.empty() || trimmed[0] == ';')
			continue;

		const std::string lower = LowerAscii(trimmed);
		size_t tokenLen = 0;
		while (tokenLen < lower.size() && IsLintWordChar(lower[tokenLen]))
			++tokenLen;
		const std::string token = lower.substr(0, tokenLen);

		for (size_t k = 0; k < kOnlyCount; ++k)
		{
			const std::string keyword = kV1Only[k];
			if (lower.compare(0, keyword.size(), keyword) != 0 || perOnly[k] >= kLintPerRule)
				continue;
			if (lower.size() > keyword.size() && IsLintWordChar(lower[keyword.size()]))
				continue;
			++perOnly[k];
			AddLintFinding(findings, counts, lineNo, "error", "v1-only",
				"v1 专有命令 " + trimmed.substr(0, keyword.size()) + "，v2 已移除或改名");
		}

		for (size_t k = 0; k < kCommandCount; ++k)
		{
			if (token != kV1Command[k] || perCommand[k] >= kLintPerRule)
				continue;
			size_t p = tokenLen;
			while (p < trimmed.size() && (trimmed[p] == ' ' || trimmed[p] == '\t'))
				++p;
			if (p >= trimmed.size() || trimmed[p] != ',')
				continue;
			++perCommand[k];
			AddLintFinding(findings, counts, lineNo, "error", "v1-command",
				"v1 命令式写法 " + trimmed.substr(0, p + 1) +
					"，v2 应写成 " + trimmed.substr(0, tokenLen) + "(...)");
		}

		// Character walk: `%name%` and quotes inside strings or comments are
		// not findings, so the scan has to know which state it is in.
		bool inString = false;
		for (size_t j = 0; j < line.size(); ++j)
		{
			const char c = line[j];
			if (c == '`' && j + 1 < line.size())
			{
				++j; // backtick escape: the next character is literal
				continue;
			}
			if (inString)
			{
				if (c == '"')
					inString = false;
				continue;
			}
			if (c == ';')
				break; // comment to end of line
			if (c == '"')
			{
				inString = true;
				continue;
			}
			if (c != '%' || percentCount >= kLintPerRule)
				continue;
			size_t end = j + 1;
			if (end >= line.size() || !IsLintIdentStart(line[end]))
				continue;
			while (end < line.size() && IsLintWordChar(line[end]))
				++end;
			if (end >= line.size() || line[end] != '%')
				continue;
			++percentCount;
			AddLintFinding(findings, counts, lineNo, "warning", "v1-percent",
				"v1 变量引用 " + line.substr(j, end - j + 1) + "，v2 直接写变量名");
			j = end;
		}
		if (inString && quoteCount < kLintPerRule)
		{
			++quoteCount;
			AddLintFinding(findings, counts, lineNo, "warning", "quote",
				"这一行的双引号没有闭合");
		}
	}

	Json result = OkResult();
	result.Set("ok", Json(counts.error == 0));
	result.Set("lines", Json((double)lines.size()));
	result.Set("findings", std::move(findings));
	Json totals = Json::MakeObject();
	totals.Set("error", Json((double)counts.error));
	totals.Set("warning", Json((double)counts.warning));
	totals.Set("total", Json((double)(counts.error + counts.warning)));
	result.Set("counts", std::move(totals));
	return result.Dump();
}

std::string Toolbox::ToolValidateEditor()
{
	const std::string text = EditorText();
	if (text.size() > 1024 * 1024)
		return ErrorResult("编辑区超过 1 MB，拒绝校验").Dump();

	const std::wstring dir = ExeDirectoryForTools() + L"\\.temp";
	CreateDirectoryW(dir.c_str(), nullptr);
	const std::wstring path = dir + L"\\validate-editor.ahk";
	std::size_t written = 0;
	std::string error;
	if (!WriteTextDocument(path, text, TextEncoding::Utf8NoBom, true, &written, error))
		return ErrorResult("写入临时脚本失败: " + error).Dump();

	ValidateRun run;
	if (!RunValidateProcess(path, run))
		return ErrorResult(run.error.empty() ? "校验进程失败" : run.error).Dump();

	int rc = run.rc;
	int errorCount = 0;
	int warningCount = 0;
	std::string summaryError;
	Json findings = Json::MakeArray();
	for (const auto &line : SplitLines(run.report))
	{
		Json record;
		std::string parseError;
		if (!Json::Parse(line, record, parseError) || !record.IsObject())
			continue;
		if (record.Has("ok") && record.Has("rc"))
		{
			rc = record["rc"].AsInt(rc);
			const std::string note = record["error"].AsString();
			if (!note.empty())
				summaryError = note;
			continue;
		}
		Json finding = Json::MakeObject();
		finding.Set("line", Json((double)record["line"].AsInt(0)));
		finding.Set("severity", Json(record["severity"].AsString()));
		finding.Set("phase", Json(record["phase"].AsString()));
		finding.Set("message", Json(record["message"].AsString()));
		if (!record["extra"].AsString().empty())
			finding.Set("extra", Json(record["extra"].AsString()));
		// Rule 9: the report names a file, never a machine path - the record's
		// file is exactly the temporary script this tool wrote.
		const std::string file = LeafOfUtf8(record["file"].AsString());
		if (!file.empty())
			finding.Set("file", Json(file));
		if (record["severity"].AsString() == "warn")
			++warningCount;
		else
			++errorCount;
		findings.Push(std::move(finding));
	}

	Json result = Json::MakeObject();
	result.Set("ok", Json(rc == 0));
	result.Set("rc", Json((double)rc));
	if (!summaryError.empty())
		result.Set("error", Json(summaryError));
	result.Set("findings", std::move(findings));
	Json counts = Json::MakeObject();
	counts.Set("error", Json((double)errorCount));
	counts.Set("warning", Json((double)warningCount));
	result.Set("counts", std::move(counts));
	result.Set("bytes", Json((double)written));

	Note(rc == 0 ? "validate_editor 通过（语法可解析）"
		: "validate_editor rc=" + std::to_string(rc) + "（" + std::to_string(errorCount) + " 个错误）");
	return result.Dump();
}

std::string Toolbox::ToolPatchEditor(const Json &aArgs)
{
	const Json &edits = aArgs["edits"];
	if (!edits.IsArray() || edits.Size() == 0)
		return ErrorResult("edits 必须是非空数组").Dump();
	if (edits.Size() > 20)
		return ErrorResult("edits 最多 20 项").Dump();

	const std::string before = EditorText();
	std::string working = before;
	for (size_t i = 0; i < edits.Size(); ++i)
	{
		const Json &edit = edits[i];
		const std::string where = "第 " + std::to_string(i + 1) + " 项：";
		const std::string op = edit["op"].IsString() ? edit["op"].AsString() : std::string();

		if (op == "replace" || op == "delete")
		{
			const std::string oldText = edit["old_text"].IsString()
				? edit["old_text"].AsString() : std::string();
			if (oldText.empty())
				return ErrorResult(where + "old_text 不能为空").Dump();
			const size_t hits = CountOccurrences(working, oldText, 2);
			if (hits == 0)
				return ErrorResult(where + "old_text 在编辑区没有命中").Dump();
			if (hits > 1)
				return ErrorResult(where + "old_text 命中不止一处，请写更长、唯一的片段").Dump();
			const size_t at = working.find(oldText);
			if (op == "delete")
				working.erase(at, oldText.size());
			else
			{
				if (!edit["new_text"].IsString())
					return ErrorResult(where + "replace 需要 new_text 字符串").Dump();
				working.replace(at, oldText.size(), edit["new_text"].AsString());
			}
		}
		else if (op == "insert_after")
		{
			if (!edit["line"].IsNumber())
				return ErrorResult(where + "insert_after 需要 line 行号").Dump();
			if (!edit["new_text"].IsString() || edit["new_text"].AsString().empty())
				return ErrorResult(where + "insert_after 需要非空 new_text").Dump();
			std::string insertError;
			if (!InsertAfterLineRaw(working, edit["line"].AsInt(0), edit["new_text"].AsString(),
					insertError))
				return ErrorResult(where + insertError).Dump();
		}
		else
		{
			return ErrorResult(where + "未知 op（可用 replace / insert_after / delete）").Dump();
		}
	}

	if (working.size() > 1024 * 1024)
		return ErrorResult("补丁后脚本超过 1 MB，拒绝").Dump();

	const bool preview = aArgs["preview"].IsBool() && aArgs["preview"].AsBool();
	if (!preview && !ApplyEditor(working, "patch_editor（" + std::to_string(edits.Size()) + " 处）"))
		return ErrorResult("当前界面没有可写入的编辑区").Dump();

	Json result = OkResult();
	result.Set("applied", Json(!preview));
	result.Set("edits", Json((double)edits.Size()));
	DescribeChange(result, CompareText(before, working));
	return result.Dump();
}

std::string Toolbox::ToolWriteScript(const Json &aArgs)
{
	if (!aArgs["content"].IsString())
		return ErrorResult("缺少 content 字符串").Dump();
	const std::string content = aArgs["content"].AsString();
	if (content.size() > 1024 * 1024)
		return ErrorResult("脚本超过 1 MB，拒绝写入").Dump();

	std::wstring target;
	std::string error;
	{
		std::lock_guard<std::mutex> lock(mScriptsMutex);
		if (aArgs["script"].IsString())
		{
			const int index = FindScriptById(aArgs["script"].AsString());
			if (index < 0)
				return ErrorResult("没有这个脚本 id（先调用 list_scripts）").Dump();
			target = mScripts[(size_t)index].path;
		}
		else if (aArgs["name"].IsString())
		{
			if (!ResolveAiScriptPath(mPolicy, WideOf(aArgs["name"].AsString()), true, target, error))
				return ErrorResult("拒绝写入: " + error).Dump();
		}
		else
		{
			return ErrorResult("需要 script（覆盖）或 name（新建）").Dump();
		}
	}

	const bool existed = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
	TextEncoding encoding = TextEncoding::Utf8NoBom;
	bool crlf = true;
	std::string before;
	std::string backupDisplay;
	if (existed)
	{
		TextDocument old;
		std::string readError;
		if (ReadTextDocument(target, old, readError))
		{
			encoding = old.encoding;
			crlf = old.crlf;
			before = old.utf8;
		}
		std::string backupError;
		(void)BackupOf(target, backupDisplay, backupError);
		if (!backupError.empty())
			return ErrorResult("拒绝覆盖（备份失败）: " + backupError).Dump();
	}

	std::size_t written = 0;
	if (!WriteTextDocument(target, content, encoding, crlf, &written, error))
		return ErrorResult("写入失败: " + error).Dump();

	std::string id;
	std::string display;
	{
		std::lock_guard<std::mutex> lock(mScriptsMutex);
		int index = -1;
		for (size_t i = 0; i < mScripts.size(); ++i)
		{
			if (SamePathI(mScripts[i].path, target))
			{
				index = (int)i;
				break;
			}
		}
		if (index < 0)
		{
			mScripts.push_back({NewScriptId(), target, Utf8Of(LeafOf(target))});
			index = (int)mScripts.size() - 1;
		}
		id = mScripts[(size_t)index].id;
		display = mScripts[(size_t)index].display;
	}

	Note("write_script " + display + " (" + std::to_string(written) + " B)");
	if (mWriteSink)
		mWriteSink(Utf8Of(target)); // the window reloads it when the user has it open

	Json result = OkResult();
	result.Set("script", Json(id));
	result.Set("name", Json(display));
	result.Set("bytes", Json((double)written));
	if (!backupDisplay.empty())
		result.Set("backup", Json(backupDisplay));
	DescribeChange(result, CompareText(before, content));
	return result.Dump();
}

std::string Toolbox::ToolRunEditor(const Json &aArgs)
{
	const std::string text = EditorText();
	if (text.size() > 1024 * 1024)
		return ErrorResult("编辑区超过 1 MB，拒绝运行").Dump();

	Engine &engine = Engine::Instance();
	if (!engine.IsLoaded())
		return ErrorResult("引擎未加载，无法运行").Dump();

	std::vector<std::wstring> args;
	std::string argsError;
	if (!ParseArguments(aArgs, args, argsError))
		return ErrorResult(argsError).Dump();

	const std::wstring tempDir = ExeDirectoryForTools() + L"\\.temp";
	CreateDirectoryW(tempDir.c_str(), nullptr);
	const std::wstring realPath = mEditorPath ? mEditorPath() : std::wstring();
	const EditorRunPath target = ResolveEditorRunPath(realPath, text, tempDir);
	const std::wstring path = target.path;
	StopCopiesOf(engine, path);
	std::size_t written = 0;
	std::string error;
	if (!WriteTextDocument(path, text, TextEncoding::Utf8NoBom, true, &written, error))
		return ErrorResult("写入临时脚本失败: " + error).Dump();

	std::string startError;
	const DWORD sessionId = engine.StartSession(path, args, startError);
	if (!sessionId)
		return ErrorResult("启动失败: " + startError).Dump();

	const int timeoutMs = ClampTimeout(aArgs, 8000);
	DWORD rc = 0;
	bool finished = false;
	const ULONGLONG started = GetTickCount64();
	while (!mCancel.load())
	{
		if (engine.WaitSession(sessionId, 50, &rc))
		{
			finished = true;
			break;
		}
		if (GetTickCount64() - started > (ULONGLONG)timeoutMs)
			break;
	}
	// The UI thread owns PopRecord()/WriteLog(); the record of this run is
	// written when the UI drains the queue, so wait for that before reading
	// the file back (m083).
	WaitForLogDrain();

	Session session;
	std::string logText;
	std::string output;
	if (engine.GetSession(sessionId, session))
	{
		const std::wstring root = engine.LogRoot();
		const size_t slash = session.logPath.find_last_of("\\/");
		const std::string leaf = slash == std::wstring::npos
			? session.logPath : session.logPath.substr(slash + 1);
		if (!root.empty() && !leaf.empty())
		{
			TextDocument document;
			std::string readError;
			if (ReadTextDocument(root + L"\\" + WideOf(leaf), document, readError))
			{
				logText = document.utf8;
				output = RedactPaths(Tail(logText, 32768));
			}
		}
	}

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	result.Set("state", Json(finished ? "finished" : "running"));
	result.Set("bytes", Json((double)written));
	if (finished)
	{
		result.Set("rc", Json((double)rc));
		if (session.fatal)
			result.Set("fatal", Json(true));
	}
	const std::string verdict = VerdictOf(engine, sessionId, finished, rc, session);
	result.Set("verdict", Json(verdict));
	// m159: an actionable next step, so a failed run names the tool to call
	// next instead of leaving the caller to guess from the label.
	const char *next = NextStepOf(verdict);
	if (*next)
		result.Set("next", Json(next));
	result.Set("script_dir", Json(target.mode));
	result.Set("errors", ErrorsFromLog(logText));
	result.Set("output", Json(output));

	Note("run_editor " + std::string(finished ? "rc=" + std::to_string((unsigned long)rc)
		: "仍在运行 (session " + std::to_string((unsigned long)sessionId) + ")")
		+ " verdict=" + verdict);
	return result.Dump();
}

std::string Toolbox::ToolRunScript(const Json &aArgs)
{
	std::wstring path;
	std::string display;
	{
		std::lock_guard<std::mutex> lock(mScriptsMutex);
		const int index = FindScriptById(aArgs["script"].AsString());
		if (index < 0)
			return ErrorResult("没有这个脚本 id（先调用 list_scripts）").Dump();
		path = mScripts[(size_t)index].path;
		display = mScripts[(size_t)index].display;
	}

	Engine &engine = Engine::Instance();
	if (!engine.IsLoaded())
		return ErrorResult("引擎未加载，无法运行").Dump();

	std::vector<std::wstring> args;
	std::string argsError;
	if (!ParseArguments(aArgs, args, argsError))
		return ErrorResult(argsError).Dump();

	const int timeoutMs = ClampTimeout(aArgs, 8000);
	StopCopiesOf(engine, path);
	std::string startError;
	const DWORD sessionId = engine.StartSession(path, args, startError);
	if (!sessionId)
		return ErrorResult("启动失败: " + startError).Dump();

	DWORD rc = 0;
	bool finished = false;
	const ULONGLONG started = GetTickCount64();
	while (!mCancel.load())
	{
		if (engine.WaitSession(sessionId, 50, &rc))
		{
			finished = true;
			break;
		}
		if (GetTickCount64() - started > (ULONGLONG)timeoutMs)
			break;
	}

	// The UI thread owns PopRecord()/WriteLog(); give it a beat to flush the
	// JSONL before reading the file back.
	WaitForLogDrain();

	Session session;
	std::string logText;
	std::string output;
	if (engine.GetSession(sessionId, session))
	{
		const std::wstring root = engine.LogRoot();
		const size_t slash = session.logPath.find_last_of("\\/");
		const std::string leaf = slash == std::wstring::npos
			? session.logPath : session.logPath.substr(slash + 1);
		if (!root.empty() && !leaf.empty())
		{
			TextDocument document;
			std::string error;
			if (ReadTextDocument(root + L"\\" + WideOf(leaf), document, error))
			{
				logText = document.utf8;
				output = RedactPaths(Tail(logText, 32768));
			}
		}
	}

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	result.Set("state", Json(finished ? "finished" : "running"));
	if (finished)
	{
		result.Set("rc", Json((double)rc));
		if (session.fatal)
			result.Set("fatal", Json(true));
	}
	const std::string verdict = VerdictOf(engine, sessionId, finished, rc, session);
	result.Set("verdict", Json(verdict));
	// m159: same next-step hint as run_editor, so both run tools answer alike.
	const char *next = NextStepOf(verdict);
	if (*next)
		result.Set("next", Json(next));
	result.Set("errors", ErrorsFromLog(logText));
	result.Set("output", Json(output));

	Note("run_script " + display + " " + (finished ? "rc=" + std::to_string((unsigned long)rc)
		: "仍在运行 (session " + std::to_string((unsigned long)sessionId) + ")")
		+ " verdict=" + verdict);
	return result.Dump();
}

std::string Toolbox::ToolGetSessionOutput(const Json &aArgs)
{
	const int sessionId = aArgs["session"].AsInt(0);
	if (sessionId <= 0)
		return ErrorResult("缺少 session 编号").Dump();

	// m101: the callers own their context budget, so the tail size is theirs to
	// choose. The default is the old fixed 32 KB; the flag and the byte count
	// let a caller tell "nothing more" from "there is more above".
	size_t limit = 32768;
	if (aArgs["max_bytes"].IsNumber())
	{
		const int requested = aArgs["max_bytes"].AsInt(0);
		limit = requested < 64 ? 64 : requested > 65536 ? 65536 : (size_t)requested;
	}

	Engine &engine = Engine::Instance();
	Session session;
	if (!engine.GetSession((DWORD)sessionId, session))
		return ErrorResult("没有这个会话").Dump();

	const std::wstring root = engine.LogRoot();
	const size_t slash = session.logPath.find_last_of("\\/");
	const std::string leaf = slash == std::wstring::npos
		? session.logPath : session.logPath.substr(slash + 1);

	// m106: the cursor is a raw byte offset, not a seq. Kernel records carry seq
	// only when the output sink is involved; host-synthesised records have none,
	// so filtering by seq would silently drop lines.
	const bool hasCursor = aArgs["after_offset"].IsNumber();
	size_t after = 0;
	if (hasCursor)
	{
		const int requested = aArgs["after_offset"].AsInt(0);
		after = requested < 0 ? 0 : (size_t)requested;
	}

	std::string output;
	size_t bytes = 0;
	size_t total = 0;
	size_t from = 0;
	size_t next = 0;
	bool truncated = false;
	bool rewound = false;
	bool rotated = false;
	if (!root.empty() && !leaf.empty())
	{
		WIN32_FILE_ATTRIBUTE_DATA old;
		rotated = GetFileAttributesExW((root + L"\\" + WideOf(leaf + ".1")).c_str(),
			GetFileExInfoStandard, &old) != 0;

		TextDocument document;
		std::string error;
		if (ReadTextDocument(root + L"\\" + WideOf(leaf), document, error))
		{
			total = document.utf8.size();
			if (hasCursor)
			{
				if (after > total)
				{
					// Rotated or truncated since the last read: restart, never fail.
					rewound = true;
					after = 0;
				}
				from = after;
				const size_t available = total - from;
				size_t end = total;
				if (available > limit)
				{
					end = from + limit;
					// A cursor must resume on a whole line: advance to the next
					// newline instead of handing back half a JSON record.
					const size_t newline = document.utf8.find('\n', end);
					if (newline != std::string::npos && newline - end <= 4096)
						end = newline + 1;
					truncated = true;
				}
				bytes = end - from;
				next = end;
				output = RedactPaths(document.utf8.substr(from, bytes));
			}
			else
			{
				truncated = total > limit;
				bytes = truncated ? limit : total;
				next = total;
				output = RedactPaths(Tail(document.utf8, limit));
			}
		}
	}

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	result.Set("state", Json(Engine::StateName(session.state)));
	result.Set("rc", Json((double)session.rc));
	result.Set("fatal", Json(session.fatal));
	result.Set("truncated", Json(truncated));
	result.Set("bytes", Json((double)bytes));
	result.Set("total_bytes", Json((double)total));
	result.Set("from_offset", Json((double)from));
	result.Set("next_offset", Json((double)next));
	result.Set("rewound", Json(rewound));
	result.Set("log_rotated", Json(rotated));
	result.Set("output", Json(output));
	return result.Dump();
}

std::string Toolbox::ToolStopScript(const Json &aArgs)
{
	const int sessionId = aArgs["session"].AsInt(0);
	if (sessionId <= 0)
		return ErrorResult("缺少 session 编号").Dump();

	Engine &engine = Engine::Instance();
	const bool stopped = engine.StopSession((DWORD)sessionId);
	Note("stop_script session " + std::to_string(sessionId)
		+ (stopped ? " 已发送停止" : " 未在运行"));

	Json result = OkResult();
	result.Set("stopped", Json(stopped));
	return result.Dump();
}

// m106: the deck's session table, not the disk. An AI that lost track of a
// session id can ask here instead of guessing; the name is a leaf (rule 9).
std::string Toolbox::ToolListSessions()
{
	Engine &engine = Engine::Instance();
	Json items = Json::MakeArray();
	const std::vector<Session> sessions = engine.Sessions();
	for (const auto &session : sessions)
	{
		Json item = Json::MakeObject();
		item.Set("session", Json((double)session.id));
		const std::string name = session.name.empty()
			? Utf8Of(LeafOf(session.path)) : session.name;
		item.Set("name", Json(name));
		item.Set("state", Json(Engine::StateName(session.state)));
		item.Set("rc", Json((double)session.rc));
		item.Set("fatal", Json(session.fatal));
		item.Set("thread", Json((double)session.threadId));
		items.Push(std::move(item));
	}

	Json result = OkResult();
	result.Set("sessions", std::move(items));
	result.Set("count", Json((double)sessions.size()));
	return result.Dump();
}

// m106: one call to end every live session. The deck already has StopSession
// (one path to stop a thread); this only loops it, then waits one second to
// report what is actually gone. A blocked thread can legitimately still be
// alive after the wait - that is what still_running is for.
std::string Toolbox::ToolStopAll()
{
	Engine &engine = Engine::Instance();
	std::vector<DWORD> ids;
	for (const auto &session : engine.Sessions())
	{
		if (session.state == SessionState::Finished)
			continue;
		if (engine.StopSession(session.id))
			ids.push_back(session.id);
	}

	int still = 0;
	for (DWORD id : ids)
	{
		DWORD ignored = 0;
		engine.WaitSession(id, 1000, &ignored);
		Session session;
		if (engine.GetSession(id, session) && session.state != SessionState::Finished)
			++still;
	}

	Json result = OkResult();
	result.Set("stopped", Json((double)ids.size()));
	result.Set("still_running", Json((double)still));
	Note("stop_all 已发送停止 " + std::to_string(ids.size())
		+ " 个会话，1 秒后仍在 " + std::to_string(still) + " 个");
	return result.Dump();
}

} // namespace agent
} // namespace ahkdeck

#include "agent/Toolbox.h"

#include "core/AppPaths.h"
#include "core/TextFile.h"
#include "core/Version.h"
#include "engine/DeckEngine.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
//
// m175: a line number is only useful if the caller can see the line it points
// at. The kernel already knows the text of that line, the function that was
// running and the frames that led there (it prints all three in its own error
// dialog), so the record carries them through verbatim - the caller then has
// file:line:source plus a call chain, and never has to guess whether "line 11"
// means the buffer it just wrote or some #Include'd file.
Json DiagnosticsFromLog(const std::string &aLog, const char *aKind, int aMax,
	const std::string *aRunText = nullptr, const std::string &aRunLeaf = std::string())
{
	Json list = Json::MakeArray();
	const std::vector<std::string> lines = SplitLines(aLog);
	// The caller's own copy of what it just ran, when there is one. The kernel's
	// `source` is the line object it executed, which is not always the line as
	// written: a fat-arrow function compiles to a synthesised `Return (expr)`,
	// so `source` reads "Return  (1 / 0)" for `f := () => (1 / 0)`. Where the
	// record is about the file we just wrote, that file's own line wins.
	std::vector<std::string> runLines;
	if (aRunText && !aRunText->empty())
		runLines = SplitLines(*aRunText);
	int taken = 0;
	for (int i = (int)lines.size() - 1; i >= 0 && taken < aMax; --i)
	{
		Json record;
		std::string parseError;
		if (!Json::Parse(lines[(size_t)i], record, parseError))
			continue;
		if (record["kind"].AsString() != aKind)
			continue;
		Json entry = Json::MakeObject();
		entry.Set("kind", Json(aKind));
		entry.Set("phase", record["phase"]);
		entry.Set("severity", record["severity"]);
		// m062: the errors array is a payload like any other - no machine path
		// may reach the caller through it (m010's redaction rule).
		std::string file = record["file"].AsString();
		const size_t slash = file.find_last_of("\\/");
		if (slash != std::string::npos)
			file = file.substr(slash + 1);
		const int line = record["line"].AsInt(0);
		entry.Set("message", Json(RedactPaths(record["message"].AsString())));
		const std::string extra = record["extra"].AsString();
		if (!extra.empty())
			entry.Set("extra", Json(RedactPaths(extra)));
		entry.Set("file", Json(file));
		entry.Set("line", Json((double)line));
		const std::string func = record["func"].AsString();
		if (!func.empty())
			entry.Set("func", Json(func));
		if (!runLines.empty() && !aRunLeaf.empty() && file == aRunLeaf
			&& line >= 1 && line <= (int)runLines.size())
		{
			std::string text = runLines[(size_t)line - 1];
			while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
				text.pop_back();
			entry.Set("source", Json(RedactPaths(text)));
		}
		else
		{
			const std::string source = record["source"].AsString();
			if (!source.empty())
				entry.Set("source", Json(RedactPaths(source)));
		}
		// Multi-line, top frame first, already leaf-only when it left the kernel.
		// CRLF -> LF so a caller that splits on '\n' does not see stray '\r'.
		const std::string stack = record["stack"].AsString();
		if (!stack.empty())
		{
			std::string folded = RedactPaths(stack);
			for (size_t p = 0; (p = folded.find("\r\n", p)) != std::string::npos;)
				folded.replace(p, 2, "\n");
			entry.Set("stack", Json(folded));
		}
		list.Push(entry);
		++taken;
	}
	return list;
}

Json ErrorsFromLog(const std::string &aLog, const std::string *aRunText = nullptr,
	const std::string &aRunLeaf = std::string())
{
	return DiagnosticsFromLog(aLog, "error", 10, aRunText, aRunLeaf);
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

	const std::wstring tempDir = TempDirUnder(exeDir, kTempWork);

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
// m177: one subdirectory of the temp\ root, so the retention pass owns them.
std::string BackupOf(const std::wstring &aScriptPath, std::string &aDisplay, std::string &aError)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char stamp[48];
	std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u-%03u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	const std::wstring backups = AppTempDir(kTempBackups);
	if (backups.empty())
	{
		aError = "the exe path could not be resolved; refusing to overwrite without a backup";
		return std::string();
	}
	const std::wstring dir = backups + L"\\" + WideOf(stamp);
	CreateDirectoryW(dir.c_str(), nullptr);

	const std::wstring target = dir + L"\\" + LeafOf(aScriptPath);
	if (!CopyFileW(aScriptPath.c_str(), target.c_str(), FALSE))
	{
		aError = "the current script could not be backed up";
		return std::string();
	}
	aDisplay = AppTempRelative(kTempBackups, std::string(stamp) + "/" +
		Utf8Of(LeafOf(aScriptPath)));
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

		// m211: folder layout - <root>\<名>\<名>.ahk, what a zip package
		// installs. A flat file with the same stem wins, so one name is never
		// two rows here either.
		const std::wstring dirPattern = root + L"\\*";
		HANDLE dirFind = FindFirstFileW(dirPattern.c_str(), &data);
		if (dirFind != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					continue;
				const std::wstring name = data.cFileName;
				if (name == L"." || name == L"..")
					continue;
				const std::wstring flat = root + L"\\" + name + L".ahk";
				if (GetFileAttributesW(flat.c_str()) != INVALID_FILE_ATTRIBUTES)
					continue;
				const std::wstring entry = root + L"\\" + name + L"\\"
					+ name + L".ahk";
				if (GetFileAttributesW(entry.c_str()) != INVALID_FILE_ATTRIBUTES)
					found.push_back({std::string(), entry, Utf8Of(name + L".ahk")});
			} while (FindNextFileW(dirFind, &data));
			FindClose(dirFind);
		}
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
			"有语法错时 rc=2，findings 给出 line（行号）、source（该行原文）、message（内核原话）、"
			"extra（内核的 Specifically 片段）与 severity；ok=true 表示脚本能被解析、可以运行。"
			"比 check_editor 更准（后者只扫 v1 残留文本）；"
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
			"errors 是最近最多 10 条内核错误对象，每条带 line（行号）、source（该行原文）、"
			"message（内核原话）、func（出错时正在执行的函数）与 stack（调用链，顶层在前，"
			"含每帧的文件名/行号/函数名/该帧原文），file 只是文件名（叶子）；"
			"warnings 是 #Warn 类告警（最多 5 条），同一形状。不必再解析 output 里的 JSONL。"
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
			"先用它读输出，再决定是否 stop_script。output 里是内核 JSONL 原文；"
			"errors/warnings 是同一批记录解析后的对象（line/source/func/stack 齐全），"
			"常驻脚本事后才报的错看这里，不必自己解析 output。"
			"返回 truncated 表示是否还有更早的内容被截掉。"
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
	// m174: measurement, not editing. The caller just wrote a §10 layout and
	// needs the geometry it actually produced. A screenshot cannot be diffed and
	// window-spy text cannot be parsed back, so the numbers come out as data.
	{
		ToolDef d;
		d.name = "inspect_ui";
		d.description = "读出界面结构：按目标进程枚举顶层窗口与全部子控件，"
			"返回类名、文字、客户区坐标、屏幕坐标、样式位、DPI。客户区坐标相对顶层窗口客户区，"
			"与开发指南 §10 坐标表同口径，可直接和源码里的 x16 y76 w388 h242 对照，不用手工减标题栏。"
			"目标三选一：session（本宿主的脚本会话）、hwnd（任何窗口句柄）、pid（任何进程）。"
			"传 items=true 时读控件里的内容：① 七类控件（ListView/ListBox/ComboBox/TreeView/Tab/"
			"StatusBar/Header）给 items{kind,count,rows,...}；② 所有控件按类给 state：勾选状态、"
			"当前选中项、滑块/进度/上下控件的位置与范围、日期时间、热键、ListView 勾选行等。"
			"这些都不在窗口文字里，只能问控件本身；读不到时给 items_error 说明原因。"
			"窗口是异步创建的：刚 run_script 完可能还没建好，用 wait_ms 等一会儿；返回空表就重试，"
			"或先 get_session_output 看脚本是不是先报错了。只读：不激活目标窗口、不改任何属性、不发输入。"
			"核对布局、查 1px 内缩、确认列表里到底是哪几行时用它。";
		d.properties.Set("session", arg("integer", "会话编号，来自 run_script、run_editor 或 list_sessions；与 hwnd、pid 三选一"));
		d.properties.Set("hwnd", arg("string", "窗口句柄（十六进制如 0x001307A8，或十进制）；给它就只看这一个窗口，含隐藏窗口"));
		d.properties.Set("pid", arg("integer", "进程编号；给枚举它的顶层窗口，可用于任何第三方程序"));
		d.properties.Set("scope", arg("string", "tree（默认，含全部子控件）或 windows（只要顶层窗口）"));
		d.properties.Set("max", arg("integer", "最多返回多少个控件，默认 200，上限 500；超出时 truncated=true"));
		d.properties.Set("wait_ms", arg("integer", "等窗口出现的最长毫秒数，默认 1500，上限 5000"));
		d.properties.Set("items", arg("boolean", "true 时读取 ListView/ListBox 的条目（行数 + 每行每列文字），默认 false"));
		d.properties.Set("rows", arg("integer", "items=true 时最多返回多少行，默认 50，上限 200"));
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "ui_pixel";
		d.description = "按屏幕坐标取一个像素的颜色（RRGGBB），坐标与 inspect_ui 的 screen 同口径。"
			"宿主里单点取色约 16ms，所以只给单点、不提供批量扫描。"
			"硬件加速或 DRM 内容所在的窗口可能取到黑图，这时给空 rgb 和 reason。";
		d.properties.Set("x", arg("integer", "屏幕坐标 x"));
		d.properties.Set("y", arg("integer", "屏幕坐标 y"));
		d.required.push_back("x");
		d.required.push_back("y");
		defs.push_back(std::move(d));
	}
{
	ToolDef d;
	d.name = "capture_ui";
	d.description = "把会话自己的窗口截成一张 BMP，返回文件路径（相对 AHKDeck.exe 所在目录；"
		"要读图就按 exe 位置拼成绝对路径）。"
		"用 PrintWindow 让窗口自己绘制，所以窗口被别的程序挡住、甚至不在前台也能截到——"
		"这台机器的前台常被别的应用占着，屏幕截图在这种时候只会拍到挡住它的那个窗口。"
		"看界面效果优先用它，不要自己去截屏。"
		"和 inspect_ui 分工：inspect_ui 给坐标与文字（精确、省 token），它给像素"
		"（看间距、字号渲染、圆角、颜色层次）。改完 UI 用它验收。"
		"window 选第几个可见窗口（序号与 inspect_ui 的 windows 一致）；"
		"rect 用客户区坐标只截一块，可直接取 inspect_ui 里控件的 client，省 token。"
		"返回 print_window=false 表示退回了屏幕拷贝（图里可能是遮挡物）；全黑时带 note 说明原因。";
	d.properties.Set("session", arg("integer", "会话编号，来自 run_script、run_editor 或 list_sessions"));
	d.properties.Set("window", arg("integer", "第几个可见顶层窗口，默认 0；序号与 inspect_ui 返回的 windows 一致"));
	d.properties.Set("rect", arg("string",
		"只截一块，客户区坐标 \"x,y,w,h\"（与 inspect_ui 的 client 同口径）；省略则截整个窗口"));
	d.properties.Set("wait_ms", arg("integer", "等窗口出现的最长毫秒数，默认 1500，上限 5000"));
	d.properties.Set("stable", arg("integer",
		"连续相同帧数：≥2 时反复重截，直到画面连续 N 帧不再变化（防「截到重绘中间态」），默认 0 不启用"));
	d.properties.Set("stable_timeout", arg("integer", "stable 生效时的最长等待毫秒数，默认 3000，上限 10000"));
	d.required.push_back("session");
	defs.push_back(std::move(d));
}
	{
		ToolDef d;
		d.name = "send_input";
		d.description = "给会话自己的窗口注入输入：PostMessage 直发鼠标/滚轮/键盘消息，"
			"不碰真实光标键盘、不抢前台、失败不落系统状态。消息序列与真实输入一致，"
			"控件端区分不出来源——这正是验证交互所需要的。"
			"为什么需要它：回调用错（对象当函数调用，K32）、右键菜单挂错消息（K45/K46）、"
			"滚轮焦点错位（K29），这些 bug 截图全是常态，只有把消息真的送进控件才暴露。"
			"action 取值：click / rclick / dblclick（x,y 客户区坐标，与 inspect_ui 的 client 同口径，"
			"省略取客户区中心）；wheel（delta 每格 ±120，负值向下）；key（Enter / Esc / Tab / Space / "
			"PageUp / PageDown / Home / End / Up / Down / Left / Right / Delete / Backspace / Insert / F1..F12）；"
			"text（逐字符 WM_CHAR，喂输入框）；move（仅移动悬停点）。"
			"坐标按「最深的非透明子控件」命中（与真实点击同规则）：Text 色块没加 +0x100 时"
			"会像真实点击一样落在父窗口上（K19）。key/text 送焦点控件。"
			"典型闭环：run_editor → send_input 点按钮/滚列表/右键 → capture_ui + inspect_ui 看结果"
			" → get_session_output 查有没有报错弹窗/进程退出。";
		d.properties.Set("session", arg("integer", "会话编号，来自 run_script、run_editor 或 list_sessions"));
		d.properties.Set("action", arg("string", "click / rclick / dblclick / wheel / key / text / move"));
		d.properties.Set("window", arg("integer", "第几个可见顶层窗口，默认 0；序号与 inspect_ui 返回的 windows 一致"));
		d.properties.Set("x", arg("integer", "客户区坐标 x（顶层窗口客户区，inspect_ui 的 client 口径）；省略取中心"));
		d.properties.Set("y", arg("integer", "客户区坐标 y；省略取中心"));
		d.properties.Set("delta", arg("integer", "wheel 的滚动量，一格 ±120，正=向上，默认 -120"));
		d.properties.Set("key", arg("string", "key 动作的键名，见 description"));
		d.properties.Set("text", arg("string", "text 动作要输入的文字"));
		d.required.push_back("session");
		d.required.push_back("action");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "ui_diff";
		d.description = "逐像素比对两张 capture_ui 截图，回答「改完之后画面到底变了哪里」："
			"差异像素数、占比、变化区域包围盒、按变化量排序的行段（bands）、最多 5 个样本点。"
			"把像素级回归从「每次手写一次性 BMP 解析探针」变成一行断言："
			"排序后表头有没有位移（bbox 的 x 有没有变）、滚动后有没有行级裁字/残影"
			"（等宽等距的 band）、闪没闪（band 是不是一大片）。"
			"a / b 传 capture_ui 返回的 path（exe 相对路径）；两张图尺寸不同会直接报错"
			"（先确认窗口没被缩放）。threshold 是逐像素三通道差值和的门槛，"
			"0=任何变化都算，调大可以忽略抗锯齿噪声。**改 UI 前后各 capture_ui 一次再 ui_diff，"
			"就是最硬的验收证据**。";
		d.properties.Set("a", arg("string", "第一张截图的 path（capture_ui 返回值）"));
		d.properties.Set("b", arg("string", "第二张截图的 path（capture_ui 返回值）"));
		d.properties.Set("threshold", arg("integer", "逐像素变化门槛（三通道差值和），0-255，默认 0"));
		d.properties.Set("rows", arg("integer", "最多返回多少条 band，默认 10，上限 50"));
		d.required.push_back("a");
		d.required.push_back("b");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "lookup_help";
		// m202: the kernel version is one parameter, not a typed-out number.
		// The 2.0.30 above is different on purpose - that is the handbook the
		// bundled help-index.json was generated from, not the running kernel.
		d.description = "查 AutoHotkey v2 的函数、内建变量与对象的官方语法：签名、每个参数的类型和含义、"
			"返回值、示例。数据来自随包的 help-index.json（356 页，取自官方 2.0.30 手册，内核为 "
			+ std::string(ahkdeck::KernelVersionString()) + "），"
			"不是凭记忆生成的，所以和你正在跑的内核一致。"
			"**写 AHK 代码前，只要不确定函数名、参数顺序、某个参数留空会怎样，就先查它。**"
			"凭记忆写出来的 API 常常语法合法、语义错（参数位置颠倒、选项名拼错、把格式串当初始值），"
			"validate_editor 和 run_editor 都拦不住这类问题。"
			"name 是完整标题，可能是合页名（\"Send / SendText / SendEvent / SendInput / SendPlay\"、"
			"\"Run / RunWait\"），这时看 aliases 知道自己查的是其中哪一个。"
			"返回的 exact=true 表示按名字或别名精确命中；exact=false 只是模糊命中，先读候选再决定用哪个。"
			"hits 为空时换一个词：用英文函数名或 A_ 开头的变量名，中文描述查不到。"
			"默认不回示例代码（省 token），需要时传 full=true。只读，不改任何文件。";
		d.properties.Set("query", arg("string",
			"要查的名字，英文，例如 StrSplit、A_Clipboard、ListView、WinExist"));
		d.properties.Set("limit", arg("integer",
			"最多返回几条，默认 3，上限 10；模糊命中时调大看候选"));
		d.properties.Set("full", arg("boolean",
			"true 时带上示例代码与完整参数说明；默认 false 只给签名和精简说明"));
		d.required.push_back("query");
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
	if (aName == "inspect_ui")
		return ToolInspectUi(args);
	if (aName == "ui_pixel")
		return ToolPixel(args);
	if (aName == "capture_ui")
		return ToolCaptureUi(args);
	if (aName == "send_input")
		return ToolSendInput(args);
	if (aName == "ui_diff")
		return ToolUiDiff(args);
	if (aName == "lookup_help")
		return ToolLookupHelp(args);
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

	const std::wstring dir = AppTempDir(kTempWork);
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
		// m175: the text of the line the kernel is complaining about. Without it
		// "line 37" is a number the caller has to go count lines to interpret -
		// and the editor buffer, the temp copy and the file on disk are three
		// different things that happen to share a line numbering.
		const std::string source = record["source"].AsString();
		if (!source.empty())
			finding.Set("source", Json(source));
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

	const std::wstring tempDir = AppTempDir(kTempWork);
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
	const std::string runLeaf = LeafOfUtf8(Utf8Of(path));
	const Json errors = ErrorsFromLog(logText, &text, runLeaf);
	const Json warnings = DiagnosticsFromLog(logText, "warn", 5, &text, runLeaf);
	result.Set("script_dir", Json(target.mode));
	result.Set("errors", errors);
	if (warnings.Size() > 0)
		result.Set("warnings", warnings);
	// m175: a resident script that already logged a runtime error is not
	// "everything is fine, keep reading output" - the failing pseudo-thread is
	// gone and whatever the caller wanted to verify almost certainly did not
	// happen. Say where it broke instead of leaving it to the verdict label.
	const char *next = NextStepOf(verdict);
	if (errors.Size() > 0)
		next = "已产生运行时错误：errors[].line 是出错行，source 是该行原文，func/stack 给出所在函数与调用链；"
			"用 patch_editor 改完再 run_editor 复验，不要只读 output。";
	if (*next)
		result.Set("next", Json(next));
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
	// m175: a resident script fails long after run_editor returned, so the only
	// record of it is in this tail. Handing back the same error objects (line,
	// source, func, stack) saves the caller from parsing the JSONL by hand to
	// answer "which line of my script just died".
	const Json errors = ErrorsFromLog(output);
	if (errors.Size() > 0)
		result.Set("errors", errors);
	const Json warnings = DiagnosticsFromLog(output, "warn", 5);
	if (warnings.Size() > 0)
		result.Set("warnings", warnings);
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

// m174: the session's window tree as measurement data. Three facts shape it:
//   * a session owns its windows by PROCESS (m203). Since m192 the kernel is a
//     separate AutoHotkey64.exe child and Session::threadId - a legacy name -
//     holds that child's pid, so two sessions are two processes and pid tells
//     them apart; CollectTopWindows lists exactly that process's windows. The
//     deck's own FLTK window belongs to this process and never appears here -
//     no class-name allow-list needed.
//   * handles and style bits are 64-bit values: they travel as hex strings,
//     because a JSON number is a double and would round a handle silently.
//   * a control's client position is relative to the TOP-LEVEL client area,
//     which is the §10 layout table's own origin (x16 y76). Nothing to
//     subtract by hand and no title-bar height to guess.
namespace
{

struct UiWalk
{
	Json items = Json::MakeArray();
	int budget = 200;
	int count = 0;
	bool truncated = false;
	HWND root = nullptr;
	// m205: entry reading. `process` is the target's handle, opened once by the
	// caller and closed after the walk; null means the owner could not be opened
	// (a list box still reads, a list view cannot). `entries` is the request.
	HANDLE process = nullptr;
	bool entries = false;
	int rows = 50;
};

std::string HexOf(ULONGLONG aValue)
{
	char text[32];
	std::snprintf(text, sizeof(text), "0x%08llX", (unsigned long long)aValue);
	return std::string(text);
}

Json BoxOf(int aX, int aY, int aW, int aH)
{
	Json box = Json::MakeObject();
	box.Set("x", Json((double)aX));
	box.Set("y", Json((double)aY));
	box.Set("w", Json((double)aW));
	box.Set("h", Json((double)aH));
	return box;
}

std::string ClassOf(HWND aHwnd)
{
	wchar_t text[256] = {0};
	const int n = GetClassNameW(aHwnd, text, 256);
	return n > 0 ? Utf8Of(std::wstring(text, (size_t)n)) : std::string();
}

std::string TextOf(HWND aHwnd)
{
	wchar_t text[1024] = {0};
	const int n = GetWindowTextW(aHwnd, text, 1024);
	if (n > 0)
		return Utf8Of(std::wstring(text, (size_t)n));
	// GetWindowTextW reads the caption field only: a Static's text lives there,
	// an Edit's body does not. WM_GETTEXT reaches the body, but it is a
	// synchronous send to another thread - a script stuck in a loop would hang
	// this call, so it has to time out. Same 40ms the standalone tool used.
	DWORD_PTR copied = 0;
	if (!SendMessageTimeoutW(aHwnd, WM_GETTEXT, (WPARAM)1024, (LPARAM)text,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &copied) ||
		copied == 0)
		return std::string();
	return Utf8Of(text);
}

// Same lookup WindowFrame.cpp uses: the host still has to run on systems whose
// SDK predates GetDpiForWindow, so the import stays dynamic and 96 is the
// honest answer when the system has no per-window DPI at all.
UINT DpiOf(HWND aHwnd)
{
	using Fn = UINT(WINAPI *)(HWND);
	static Fn fn = []() -> Fn
	{
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		return user32 ? (Fn)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
	}();
	return fn ? fn(aHwnd) : 96;
}

Json NodeOf(HWND aHwnd, HWND aRoot, int aDepth)
{
	RECT rc = {0, 0, 0, 0};
	GetWindowRect(aHwnd, &rc);
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;

	Json node = Json::MakeObject();
	node.Set("hwnd", Json(HexOf((ULONGLONG)(ULONG_PTR)aHwnd)));
	node.Set("depth", Json((double)aDepth));
	node.Set("class", Json(ClassOf(aHwnd)));
	node.Set("text", Json(TextOf(aHwnd)));
	if (aHwnd == aRoot)
	{
		RECT client = {0, 0, 0, 0};
		GetClientRect(aHwnd, &client);
		node.Set("client", BoxOf(0, 0, client.right, client.bottom));
	}
	else
	{
		POINT origin = {rc.left, rc.top};
		ScreenToClient(aRoot, &origin);
		node.Set("client", BoxOf(origin.x, origin.y, w, h));
	}
	node.Set("screen", BoxOf(rc.left, rc.top, w, h));
	node.Set("style", Json(HexOf((ULONGLONG)(ULONG_PTR)GetWindowLongPtrW(aHwnd, GWL_STYLE))));
	node.Set("exstyle", Json(HexOf((ULONGLONG)(ULONG_PTR)GetWindowLongPtrW(aHwnd, GWL_EXSTYLE))));
	node.Set("visible", Json(IsWindowVisible(aHwnd) != FALSE));
	node.Set("enabled", Json(IsWindowEnabled(aHwnd) != FALSE));
	return node;
}

// ------------------------------------------------------ m205: 条目，不是窗口
//
// A ListView row is not a child window: GetWindow(GW_CHILD) never returns one
// and GetWindowText has nothing to say, which is why `text` comes back empty
// for the four list-bearing official scripts. The owner answers these through
// control messages instead, and a message that carries a pointer only works if
// the buffer is in the OWNER's address space - so each read goes through one
// VirtualAllocEx block in the target process: LVITEMW and the text buffer sit
// in the same block, and the struct has to be rewritten before every call
// because it holds the pointer.
const int kEntryTextChars = 512;
const int kEntryMaxColumns = 16;

size_t EntryTextLength(const wchar_t *aText, size_t aMax)
{
	size_t n = 0;
	while (n < aMax && aText[n])
		++n;
	return n;
}

class RemoteArena
{
public:
	explicit RemoteArena(HANDLE aProcess) : mProcess(aProcess) {}
	~RemoteArena() { Release(); }
	RemoteArena(const RemoteArena &) = delete;
	RemoteArena &operator=(const RemoteArena &) = delete;

	bool Reserve(size_t aBytes)
	{
		Release();
		mBase = VirtualAllocEx(mProcess, nullptr, aBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		return mBase != nullptr;
	}

	bool Write(size_t aOffset, const void *aSrc, size_t aBytes)
	{
		SIZE_T done = 0;
		return WriteProcessMemory(mProcess, (BYTE *)mBase + aOffset, aSrc, aBytes, &done) && done == aBytes;
	}

	bool Read(size_t aOffset, void *aDst, size_t aBytes)
	{
		SIZE_T done = 0;
		return ReadProcessMemory(mProcess, (const BYTE *)mBase + aOffset, aDst, aBytes, &done) && done == aBytes;
	}

	LPARAM Remote(size_t aOffset) const { return (LPARAM)((BYTE *)mBase + aOffset); }

	void Release()
	{
		if (mBase)
			VirtualFreeEx(mProcess, mBase, 0, MEM_RELEASE);
		mBase = nullptr;
	}

private:
	HANDLE mProcess = nullptr;
	void *mBase = nullptr;
};

// Every send carries a 100ms budget for the same reason TextOf() does: this is
// another thread, and a script stuck in a loop must not hang the tool.
bool ReadEntries(HWND aHwnd, HANDLE aProcess, bool aListView, int aMaxRows, Json &aOut)
{
	const size_t cellBytes = (size_t)(kEntryTextChars + 1) * sizeof(wchar_t);
	// LVM_* messages are NOT marshaled, so the struct and its text buffer have to
	// live in the owner's address space. LB_GETTEXT *is* marshaled by the system
	// (same treatment as WM_GETTEXT), so a local buffer is what works there -
	// measured: the remote buffer returned count=2 and two empty strings.
	RemoteArena arena(aProcess);
	if (aListView && !arena.Reserve(sizeof(LVITEMW) + cellBytes))
		return false;

	DWORD_PTR answer = 0;
	if (!SendMessageTimeoutW(aHwnd, aListView ? LVM_GETITEMCOUNT : LB_GETCOUNT, 0, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &answer))
		return false;
	const int count = (int)answer;

	// A list can have columns but no visible header (-Hdr in our own scripts):
	// the header control still exists, so its item count is the column count.
	int columns = 1;
	if (aListView)
	{
		DWORD_PTR header = 0;
		if (SendMessageTimeoutW(aHwnd, LVM_GETHEADER, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &header) && header)
		{
			DWORD_PTR found = 0;
			if (SendMessageTimeoutW((HWND)(ULONG_PTR)header, HDM_GETITEMCOUNT, 0, 0,
					SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &found))
				columns = (int)found;
		}
		columns = columns < 1 ? 1 : (columns > kEntryMaxColumns ? kEntryMaxColumns : columns);
	}

	Json rows = Json::MakeArray();
	int shown = 0;
	wchar_t text[kEntryTextChars + 1] = {0};
	for (int row = 0; row < count && row < aMaxRows; ++row)
	{
		Json cells = Json::MakeArray();
		if (aListView)
		{
			for (int col = 0; col < columns; ++col)
			{
				LVITEMW item = {};
				item.mask = LVIF_TEXT;
				item.iItem = row;
				item.iSubItem = col;
				item.pszText = (LPWSTR)arena.Remote(sizeof(LVITEMW));
				item.cchTextMax = kEntryTextChars;
				if (!arena.Write(0, &item, sizeof(item)))
					break;
				DWORD_PTR copied = 0;
				SendMessageTimeoutW(aHwnd, LVM_GETITEMTEXTW, (WPARAM)row, arena.Remote(0),
					SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied);
				text[0] = 0;
				if (!arena.Read(sizeof(LVITEMW), text, cellBytes))
					break;
				text[kEntryTextChars] = 0;
				cells.Push(Json(Utf8Of(std::wstring(text, EntryTextLength(text, kEntryTextChars)))));
			}
		}
		else
		{
			wchar_t local[kEntryTextChars + 1] = {0};
			DWORD_PTR copied = 0;
			if (!SendMessageTimeoutW(aHwnd, LB_GETTEXT, (WPARAM)row, (LPARAM)local,
					SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) || copied == (DWORD_PTR)-1)
				break;
			local[kEntryTextChars] = 0;
			cells.Push(Json(Utf8Of(std::wstring(local, EntryTextLength(local, kEntryTextChars)))));
		}
		rows.Push(std::move(cells));
		++shown;
	}

	Json out = Json::MakeObject();
	out.Set("kind", Json(aListView ? "listview" : "listbox"));
	out.Set("count", Json((double)count));
	if (aListView)
		out.Set("columns", Json((double)columns));
	out.Set("rows", std::move(rows));
	out.Set("truncated", Json(shown < count));
	if (aListView)
	{
		// Selection and the check-box column are states on the rows themselves,
		// not rows of text: LVM_GETITEMSTATE returns the bits directly, so no
		// remote buffer is needed here.
		DWORD_PTR selected = 0;
		if (SendMessageTimeoutW(aHwnd, LVM_GETSELECTEDCOUNT, 0, 0,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &selected))
			out.Set("selected_count", Json((double)selected));
		Json checked = Json::MakeArray();
		for (int row = 0; row < shown; ++row)
		{
			DWORD_PTR state = 0;
			if (!SendMessageTimeoutW(aHwnd, LVM_GETITEMSTATE, (WPARAM)row, LVIS_STATEIMAGEMASK,
					SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &state))
				break;
			if ((state & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2))
				checked.Push(Json((double)row));
		}
		if (checked.Size() > 0)
			out.Set("checked_rows", std::move(checked));
	}
	aOut = std::move(out);
	return true;
}

Json EntriesObject(const char *aKind, int aCount, int aShown, Json &aRows, int aColumns)
{
	Json out = Json::MakeObject();
	out.Set("kind", Json(aKind));
	out.Set("count", Json((double)aCount));
	if (aColumns > 0)
		out.Set("columns", Json((double)aColumns));
	out.Set("rows", std::move(aRows));
	out.Set("truncated", Json(aShown < aCount));
	return out;
}

// ComboBox answers with CB_* messages, and like LB_* the system marshals the
// buffer for us - so this one needs no remote memory either.
bool ReadComboItems(HWND aHwnd, int aMaxRows, Json &aOut)
{
	DWORD_PTR total = 0;
	if (!SendMessageTimeoutW(aHwnd, CB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &total))
		return false;
	const int count = (int)total;
	Json rows = Json::MakeArray();
	int shown = 0;
	wchar_t local[kEntryTextChars + 1] = {0};
	for (int index = 0; index < count && index < aMaxRows; ++index)
	{
		DWORD_PTR copied = 0;
		if (!SendMessageTimeoutW(aHwnd, CB_GETLBTEXT, (WPARAM)index, (LPARAM)local,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) || copied == (DWORD_PTR)-1)
			break;
		local[kEntryTextChars] = 0;
		Json cells = Json::MakeArray();
		cells.Push(Json(Utf8Of(std::wstring(local, EntryTextLength(local, kEntryTextChars)))));
		rows.Push(std::move(cells));
		++shown;
	}
	aOut = EntriesObject("combobox", count, shown, rows, 0);
	return true;
}

// TVM_GETITEMW is not marshaled either, so TVITEMW and the text share the same
// remote block as the list view's. `levels` exists because a tree without its
// shape is just an unordered pile of words: the row order is a depth-first walk,
// and level 0 is the root row.
bool ReadTreeBranch(HWND aHwnd, RemoteArena &aArena, HTREEITEM aItem, int aLevel,
	int &aRemaining, Json &aRows, Json &aLevels)
{
	while (aItem && aRemaining > 0)
	{
		TVITEMW item = {};
		item.mask = TVIF_TEXT | TVIF_HANDLE;
		item.hItem = aItem;
		item.pszText = (LPWSTR)aArena.Remote(sizeof(TVITEMW));
		item.cchTextMax = kEntryTextChars;
		if (!aArena.Write(0, &item, sizeof(item)))
			return false;
		DWORD_PTR copied = 0;
		if (!SendMessageTimeoutW(aHwnd, TVM_GETITEMW, 0, aArena.Remote(0),
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) || !copied)
			return false;
		wchar_t text[kEntryTextChars + 1] = {0};
		if (!aArena.Read(sizeof(TVITEMW), text, (kEntryTextChars + 1) * sizeof(wchar_t)))
			return false;
		text[kEntryTextChars] = 0;
		Json cells = Json::MakeArray();
		cells.Push(Json(Utf8Of(std::wstring(text, EntryTextLength(text, kEntryTextChars)))));
		aRows.Push(std::move(cells));
		aLevels.Push(Json((double)aLevel));
		--aRemaining;

		DWORD_PTR child = 0;
		if (SendMessageTimeoutW(aHwnd, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)aItem,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &child) && child)
		{
			if (!ReadTreeBranch(aHwnd, aArena, (HTREEITEM)(ULONG_PTR)child, aLevel + 1,
					aRemaining, aRows, aLevels))
				return false;
		}
		DWORD_PTR next = 0;
		if (!SendMessageTimeoutW(aHwnd, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)aItem,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &next))
			return false;
		aItem = (HTREEITEM)(ULONG_PTR)next;
	}
	return true;
}

bool ReadTreeItems(HWND aHwnd, HANDLE aProcess, int aMaxRows, Json &aOut)
{
	RemoteArena arena(aProcess);
	if (!arena.Reserve(sizeof(TVITEMW) + (kEntryTextChars + 1) * sizeof(wchar_t)))
		return false;
	DWORD_PTR total = 0;
	if (!SendMessageTimeoutW(aHwnd, TVM_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &total))
		return false;
	DWORD_PTR root = 0;
	if (!SendMessageTimeoutW(aHwnd, TVM_GETNEXTITEM, TVGN_ROOT, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &root))
		return false;

	Json rows = Json::MakeArray();
	Json levels = Json::MakeArray();
	int remaining = aMaxRows;
	if (root && !ReadTreeBranch(aHwnd, arena, (HTREEITEM)(ULONG_PTR)root, 0, remaining, rows, levels))
		return false;

	Json out = EntriesObject("treeview", (int)total, aMaxRows - remaining, rows, 0);
	out.Set("levels", std::move(levels));
	DWORD_PTR caret = 0;
	if (SendMessageTimeoutW(aHwnd, TVM_GETNEXTITEM, TVGN_CARET, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &caret) && caret)
	{
		TVITEMW item = {};
		item.mask = TVIF_TEXT | TVIF_HANDLE;
		item.hItem = (HTREEITEM)(ULONG_PTR)caret;
		item.pszText = (LPWSTR)arena.Remote(sizeof(TVITEMW));
		item.cchTextMax = kEntryTextChars;
		if (arena.Write(0, &item, sizeof(item)))
		{
			DWORD_PTR copied = 0;
			if (SendMessageTimeoutW(aHwnd, TVM_GETITEMW, 0, arena.Remote(0),
					SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) && copied)
			{
				wchar_t text[kEntryTextChars + 1] = {0};
				if (arena.Read(sizeof(TVITEMW), text, sizeof(text)))
				{
					text[kEntryTextChars] = 0;
					out.Set("selected", Json(Utf8Of(std::wstring(text, EntryTextLength(text, kEntryTextChars)))));
				}
			}
		}
	}
	aOut = std::move(out);
	return true;
}

bool ReadTabItems(HWND aHwnd, HANDLE aProcess, int aMaxRows, Json &aOut)
{
	RemoteArena arena(aProcess);
	if (!arena.Reserve(sizeof(TCITEMW) + (kEntryTextChars + 1) * sizeof(wchar_t)))
		return false;
	DWORD_PTR total = 0;
	if (!SendMessageTimeoutW(aHwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &total))
		return false;
	const int count = (int)total;
	Json rows = Json::MakeArray();
	int shown = 0;
	wchar_t text[kEntryTextChars + 1] = {0};
	for (int index = 0; index < count && index < aMaxRows; ++index)
	{
		TCITEMW item = {};
		item.mask = TCIF_TEXT;
		item.pszText = (LPWSTR)arena.Remote(sizeof(TCITEMW));
		item.cchTextMax = kEntryTextChars;
		if (!arena.Write(0, &item, sizeof(item)))
			break;
		DWORD_PTR copied = 0;
		if (!SendMessageTimeoutW(aHwnd, TCM_GETITEMW, (WPARAM)index, arena.Remote(0),
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) || !copied)
			break;
		text[0] = 0;
		if (!arena.Read(sizeof(TCITEMW), text, (kEntryTextChars + 1) * sizeof(wchar_t)))
			break;
		text[kEntryTextChars] = 0;
		Json cells = Json::MakeArray();
		cells.Push(Json(Utf8Of(std::wstring(text, EntryTextLength(text, kEntryTextChars)))));
		rows.Push(std::move(cells));
		++shown;
	}
	aOut = EntriesObject("tab", count, shown, rows, 0);
	return true;
}

// One row per part: a status bar's text lives in the control, not in the window
// caption, which is why a Gui's "就绪" line was invisible before this.
bool ReadStatusParts(HWND aHwnd, HANDLE aProcess, int aMaxRows, Json &aOut)
{
	RemoteArena arena(aProcess);
	if (!arena.Reserve((kEntryTextChars + 1) * sizeof(wchar_t)))
		return false;
	DWORD_PTR parts = 0;
	if (!SendMessageTimeoutW(aHwnd, SB_GETPARTS, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &parts))
		return false;
	const int count = (int)parts;
	Json rows = Json::MakeArray();
	int shown = 0;
	wchar_t text[kEntryTextChars + 1] = {0};
	for (int index = 0; index < count && index < aMaxRows; ++index)
	{
		DWORD_PTR copied = 0;
		if (!SendMessageTimeoutW(aHwnd, SB_GETTEXTW, (WPARAM)index, arena.Remote(0),
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied))
			break;
		text[0] = 0;
		if (!arena.Read(0, text, (kEntryTextChars + 1) * sizeof(wchar_t)))
			break;
		text[kEntryTextChars] = 0;
		Json cells = Json::MakeArray();
		cells.Push(Json(Utf8Of(std::wstring(text, EntryTextLength(text, kEntryTextChars)))));
		rows.Push(std::move(cells));
		++shown;
	}
	aOut = EntriesObject("statusbar", count, shown, rows, 0);
	return true;
}

// --------------------------------------------------- 当前值（不是一行文字）
//
// A check-box's tick, a slider's position and a tab's current page are not rows:
// they are one number or one state per control. They live in the control, not in
// the window caption, and nearly all of them come back as the message's return
// value - no remote buffer at all. Only the two calendar controls answer through
// a SYSTEMTIME pointer, which does have to be in the owner's address space.
std::string DateOf(const SYSTEMTIME &aTime)
{
	char text[32] = {0};
	std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u",
		(unsigned)aTime.wYear, (unsigned)aTime.wMonth, (unsigned)aTime.wDay,
		(unsigned)aTime.wHour, (unsigned)aTime.wMinute, (unsigned)aTime.wSecond);
	return std::string(text);
}

bool Ask(HWND aHwnd, UINT aMsg, WPARAM aW, LPARAM aL, DWORD_PTR &aOut)
{
	aOut = 0;
	return SendMessageTimeoutW(aHwnd, aMsg, aW, aL,
		SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &aOut) != 0;
}

// DTM_GETSYSTEMTIME returns GDT_VALID (0) on success and GDT_ERROR (-1) on
// failure; MCM_GETCURSEL returns a BOOL. Both answer "did it work" differently,
// so the caller says which convention applies.
bool ReadSystemTime(HWND aHwnd, HANDLE aProcess, UINT aMsg, bool aWantsNonZero, std::string &aOut)
{
	if (!aProcess)
		return false;
	RemoteArena arena(aProcess);
	if (!arena.Reserve(sizeof(SYSTEMTIME)))
		return false;
	const SYSTEMTIME zero = {};
	if (!arena.Write(0, &zero, sizeof(zero)))
		return false;
	DWORD_PTR result = 0;
	if (!Ask(aHwnd, aMsg, 0, arena.Remote(0), result))
		return false;
	if (aWantsNonZero ? result == 0 : result == (DWORD_PTR)-1)
		return false;
	SYSTEMTIME value = {};
	if (!arena.Read(0, &value, sizeof(value)))
		return false;
	aOut = DateOf(value);
	return true;
}

std::string HotkeyOf(WORD aHotkey)
{
	const BYTE vk = LOBYTE(aHotkey);
	const BYTE mods = HIBYTE(aHotkey);
	std::string out;
	if (mods & HOTKEYF_CONTROL) out += "^";
	if (mods & HOTKEYF_SHIFT) out += "+";
	if (mods & HOTKEYF_ALT) out += "!";
	if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
		out += (char)vk;
	else
	{
		char text[16] = {0};
		std::snprintf(text, sizeof(text), "vk%02X", (unsigned)vk);
		out += text;
	}
	return out;
}

// One uniform `state` object regardless of control class: callers do not have
// to learn a new shape for every kind, and an unreadable value simply leaves the
// field out instead of lying about it.
Json StateOf(HWND aHwnd, const std::string &aClass, HANDLE aProcess)
{
	Json state = Json::MakeObject();
	DWORD_PTR v = 0;
	bool any = false;
	const auto put = [&](const char *aKey, double aNumber)
	{
		state.Set(aKey, Json(aNumber));
		any = true;
	};
	const LONG_PTR style = GetWindowLongPtrW(aHwnd, GWL_STYLE);

	if (lstrcmpiA(aClass.c_str(), "Button") == 0)
	{
		// BS_TYPEMASK is the low nibble: 2/3 = check box, 4/9 = radio,
		// 6/11 = three-state. A push button has 0 and no check state to report.
		const LONG_PTR type = style & 0xF;
		if (type >= 2 && type <= 11 && Ask(aHwnd, BM_GETCHECK, 0, 0, v))
			put("checked", (double)v);
	}
	else if (lstrcmpiA(aClass.c_str(), "ComboBox") == 0)
	{
		if (Ask(aHwnd, CB_GETCURSEL, 0, 0, v) && (int)v >= 0)
			put("selected", (double)v);
	}
	else if (lstrcmpiA(aClass.c_str(), "ListBox") == 0)
	{
		if (Ask(aHwnd, LB_GETCURSEL, 0, 0, v) && (int)v >= 0)
			put("selected", (double)v);
		if ((style & LBS_MULTIPLESEL) && Ask(aHwnd, LB_GETSELCOUNT, 0, 0, v))
			put("selected_count", (double)v);
	}
	else if (lstrcmpiA(aClass.c_str(), "SysTabControl32") == 0)
	{
		if (Ask(aHwnd, TCM_GETCURSEL, 0, 0, v) && (int)v >= 0)
			put("selected", (double)v);
	}
	else if (lstrcmpiA(aClass.c_str(), "msctls_trackbar32") == 0)
	{
		if (Ask(aHwnd, TBM_GETPOS, 0, 0, v)) put("pos", (double)v);
		if (Ask(aHwnd, TBM_GETRANGEMIN, 0, 0, v)) put("min", (double)v);
		if (Ask(aHwnd, TBM_GETRANGEMAX, 0, 0, v)) put("max", (double)v);
	}
	else if (lstrcmpiA(aClass.c_str(), "msctls_progress32") == 0)
	{
		if (Ask(aHwnd, PBM_GETPOS, 0, 0, v)) put("pos", (double)v);
		if (Ask(aHwnd, PBM_GETRANGE, 0, 0, v)) put("min", (double)(short)LOWORD(v));
		if (Ask(aHwnd, PBM_GETRANGE, 1, 0, v)) put("max", (double)(short)LOWORD(v));
	}
	else if (lstrcmpiA(aClass.c_str(), "msctls_updown32") == 0)
	{
		// UDM_GETPOS keeps an error flag in the high word; a nonzero one means
		// the value is not usable, so it must not be reported as a position.
		if (Ask(aHwnd, UDM_GETPOS, 0, 0, v) && HIWORD(v) == 0)
			put("pos", (double)(short)LOWORD(v));
	}
	else if (lstrcmpiA(aClass.c_str(), "msctls_hotkey32") == 0)
	{
		if (Ask(aHwnd, HKM_GETHOTKEY, 0, 0, v))
		{
			state.Set("hotkey", Json(HotkeyOf((WORD)v)));
			any = true;
		}
	}
	else if (lstrcmpiA(aClass.c_str(), "SysDateTimePick32") == 0)
	{
		std::string date;
		if (ReadSystemTime(aHwnd, aProcess, DTM_GETSYSTEMTIME, false, date))
		{
			state.Set("date", Json(date));
			any = true;
		}
	}
	else if (lstrcmpiA(aClass.c_str(), "SysMonthCal32") == 0)
	{
		std::string date;
		if (ReadSystemTime(aHwnd, aProcess, MCM_GETCURSEL, true, date))
		{
			state.Set("date", Json(date));
			any = true;
		}
	}
	return any ? state : Json();
}

// A ListView's header is a child window of its own, and its column titles are
// items too. Cheap now that HDM_* shares the same remote-buffer path as TCITEMW.
bool ReadHeaderItems(HWND aHwnd, HANDLE aProcess, int aMaxRows, Json &aOut)
{
	RemoteArena arena(aProcess);
	if (!arena.Reserve(sizeof(HDITEMW) + (kEntryTextChars + 1) * sizeof(wchar_t)))
		return false;
	DWORD_PTR total = 0;
	if (!SendMessageTimeoutW(aHwnd, HDM_GETITEMCOUNT, 0, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &total))
		return false;
	const int count = (int)total;
	Json rows = Json::MakeArray();
	int shown = 0;
	wchar_t text[kEntryTextChars + 1] = {0};
	for (int index = 0; index < count && index < aMaxRows; ++index)
	{
		HDITEMW item = {};
		item.mask = HDI_TEXT;
		item.pszText = (LPWSTR)arena.Remote(sizeof(HDITEMW));
		item.cchTextMax = kEntryTextChars;
		if (!arena.Write(0, &item, sizeof(item)))
			break;
		DWORD_PTR copied = 0;
		if (!SendMessageTimeoutW(aHwnd, HDM_GETITEMW, (WPARAM)index, arena.Remote(0),
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) || !copied)
			break;
		text[0] = 0;
		if (!arena.Read(sizeof(HDITEMW), text, (kEntryTextChars + 1) * sizeof(wchar_t)))
			break;
		text[kEntryTextChars] = 0;
		Json cells = Json::MakeArray();
		cells.Push(Json(Utf8Of(std::wstring(text, EntryTextLength(text, kEntryTextChars)))));
		rows.Push(std::move(cells));
		++shown;
	}
	aOut = EntriesObject("header", count, shown, rows, 0);
	return true;
}

// Only the controls whose content is invisible to GetWindow(GW_CHILD) are listed
// here. Everything else is already described by the node itself, and an entry
// read is a cross-process round trip per row - not something to do unasked.
void AttachEntries(HWND aHwnd, Json &aNode, HANDLE aProcess, int aRows)
{
	const std::string cls = ClassOf(aHwnd);
	Json state = StateOf(aHwnd, cls, aProcess);
	if (!state.IsNull())
		aNode.Set("state", std::move(state));

	Json entries;
	bool known = true;
	bool ok = false;
	if (lstrcmpiA(cls.c_str(), "SysListView32") == 0)
		ok = ReadEntries(aHwnd, aProcess, true, aRows, entries);
	else if (lstrcmpiA(cls.c_str(), "ListBox") == 0)
		ok = ReadEntries(aHwnd, aProcess, false, aRows, entries);
	else if (lstrcmpiA(cls.c_str(), "ComboBox") == 0)
		ok = ReadComboItems(aHwnd, aRows, entries);
	else if (lstrcmpiA(cls.c_str(), "SysTreeView32") == 0)
		ok = ReadTreeItems(aHwnd, aProcess, aRows, entries);
	else if (lstrcmpiA(cls.c_str(), "SysTabControl32") == 0)
		ok = ReadTabItems(aHwnd, aProcess, aRows, entries);
	else if (lstrcmpiA(cls.c_str(), "msctls_statusbar32") == 0)
		ok = ReadStatusParts(aHwnd, aProcess, aRows, entries);
	else if (lstrcmpiA(cls.c_str(), "SysHeader32") == 0)
		ok = ReadHeaderItems(aHwnd, aProcess, aRows, entries);
	else
		known = false;

	if (!known)
		return;
	if (ok)
		aNode.Set("items", std::move(entries));
	else
		aNode.Set("items_error", Json("读不到条目：控件无响应，或目标进程不可读写"));
}

// Parent before child, siblings in z order - the same order EnumChildWindows
// uses, written out so `depth` means something and nesting stays readable.
void WalkChildren(HWND aParent, UiWalk &aWalk, int aDepth)
{
	for (HWND child = GetWindow(aParent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
	{
		if (aWalk.count >= aWalk.budget)
		{
			aWalk.truncated = true;
			return;
		}
		Json node = NodeOf(child, aWalk.root, aDepth);
		if (aWalk.entries)
			AttachEntries(child, node, aWalk.process, aWalk.rows);
		++aWalk.count;
		aWalk.items.Push(std::move(node));
		if (aDepth < 8)
			WalkChildren(child, aWalk, aDepth + 1);
	}
}

// m203: a session is a child process since m192, and Session::threadId (a
// legacy name) holds that child's pid - EnumThreadWindows(pid) lists nothing,
// which blinded inspect_ui/capture_ui on every machine. Top-level windows now
// come from EnumWindows filtered by GetWindowThreadProcessId: cross-process,
// but read-only, the same shape session_gate F4 already asserts.
struct WindowScan
{
	DWORD pid = 0;
	std::vector<HWND> *out = nullptr;
};

BOOL CALLBACK CollectSessionWindow(HWND aHwnd, LPARAM aParam)
{
	WindowScan *scan = reinterpret_cast<WindowScan *>(aParam);
	DWORD pid = 0;
	GetWindowThreadProcessId(aHwnd, &pid);
	if (pid == scan->pid)
		scan->out->push_back(aHwnd);
	return TRUE;
}

void CollectTopWindows(DWORD aPid, std::vector<HWND> &aOut)
{
	WindowScan scan;
	scan.pid = aPid;
	scan.out = &aOut;
	EnumWindows(CollectSessionWindow, (LPARAM)&scan);
}

// Writes a top-down 24bpp DIB as a bottom-up BMP. BMP needs no codec and every
// image reader - a model's included - opens it, which is why the capture tool
// carries no PNG encoder. aStride is the padded row stride of aPixels.
bool WriteBmp24(const std::wstring &aPath, int aWidth, int aHeight,
	const unsigned char *aPixels, int aStride)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	const int rowBytes = aWidth * 3;
	const int outStride = ((rowBytes + 3) / 4) * 4;
	const DWORD imageBytes = (DWORD)outStride * (DWORD)aHeight;

	BITMAPFILEHEADER header = {};
	header.bfType = 0x4D42; // "BM"
	header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	header.bfSize = header.bfOffBits + imageBytes;

	BITMAPINFOHEADER info = {};
	info.biSize = sizeof(BITMAPINFOHEADER);
	info.biWidth = aWidth;
	info.biHeight = aHeight;
	info.biPlanes = 1;
	info.biBitCount = 24;
	info.biCompression = BI_RGB;
	info.biSizeImage = imageBytes;

	DWORD written = 0;
	bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) != FALSE
		&& WriteFile(file, &info, sizeof(info), &written, nullptr) != FALSE;
	if (ok)
	{
		// BMP rows run bottom-up; the DIB we read runs top-down.
		std::vector<unsigned char> row((size_t)outStride, 0);
		for (int y = aHeight - 1; y >= 0 && ok; --y)
		{
			std::memcpy(row.data(), aPixels + (size_t)y * (size_t)aStride,
				(size_t)rowBytes);
			ok = WriteFile(file, row.data(), (DWORD)outStride, &written, nullptr) != FALSE;
		}
	}
	CloseHandle(file);
	return ok;
}

} // namespace

std::string Toolbox::ToolInspectUi(const Json &aArgs)
{
	// m205: three ways in. `session` keeps its original meaning (a script session
	// of this host). `hwnd` and `pid` widen the same tool to any other program -
	// the tree is the same question either way, only the entry point differs, and
	// an AI writing a script that drives another program needs that tree.
	HWND single = nullptr;
	DWORD pid = 0;
	DWORD sessionId = 0;
	if (aArgs.Has("hwnd"))
	{
		const std::string raw = aArgs["hwnd"].IsString()
			? aArgs["hwnd"].AsString()
			: std::to_string(aArgs["hwnd"].AsInt(0));
		single = (HWND)(ULONG_PTR)wcstoull(WideOf(raw).c_str(), nullptr, 0);
		if (!single || !IsWindow(single))
			return ErrorResult("hwnd 不是一个存在的窗口: " + raw).Dump();
		GetWindowThreadProcessId(single, &pid);
	}
	else if (aArgs.Has("pid") && aArgs["pid"].IsNumber())
	{
		pid = (DWORD)aArgs["pid"].AsInt(0);
		if (pid == 0)
			return ErrorResult("pid 必须是正整数").Dump();
	}
	else
	{
		if (!aArgs.Has("session") || !aArgs["session"].IsNumber())
			return ErrorResult("需要 session（本宿主的脚本会话）、hwnd 或 pid 之一").Dump();
		sessionId = (DWORD)aArgs["session"].AsInt(0);
		Session session;
		if (!Engine::Instance().GetSession(sessionId, session))
			return ErrorResult("未知会话: " + std::to_string(sessionId)).Dump();
		if (session.threadId == 0)
			return ErrorResult("会话还没有脚本进程（脚本可能连加载都没走到）").Dump();
		pid = session.threadId;
	}

	const bool tree = aArgs["scope"].AsString() != "windows";
	int max = aArgs["max"].AsInt(200);
	max = max < 1 ? 1 : (max > 500 ? 500 : max);
	int waitMs = aArgs["wait_ms"].AsInt(1500);
	waitMs = waitMs < 0 ? 0 : (waitMs > 5000 ? 5000 : waitMs);
	const bool wantItems = aArgs["items"].AsBool(false);
	int rows = aArgs["rows"].AsInt(50);
	rows = rows < 1 ? 1 : (rows > 200 ? 200 : rows);

	// Entries need PROCESS_VM_* on the owner. A refused open is not a failure for
	// the tree - another integrity level, or the process is already gone - it only
	// means `items` stays absent and the tree is still worth returning.
	HANDLE target = wantItems
		? OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
			FALSE, pid)
		: nullptr;

	// The window is created asynchronously by the script process: a run that has
	// already returned can still be milliseconds away from its first visible
	// Gui. Answering with an empty list at that moment reads as "my window never
	// appeared", which is the wrong diagnosis most of the time. An explicit hwnd
	// is already there, so it skips the wait.
	std::vector<HWND> tops;
	if (single)
	{
		tops.push_back(single);
	}
	else
	{
		const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)waitMs;
		for (;;)
		{
			tops.clear();
			CollectTopWindows(pid, tops);
			bool visible = false;
			for (HWND hwnd : tops)
			{
				if (IsWindowVisible(hwnd))
				{
					visible = true;
					break;
				}
			}
			if (visible || GetTickCount64() >= deadline)
				break;
			Sleep(60);
		}
	}

	Json windows = Json::MakeArray();
	int shown = 0;
	int total = 0;
	bool truncated = false;
	for (HWND hwnd : tops)
	{
		// Hidden top-level windows are noise here: a script usually parks its
		// real window in one and keeps a hidden one behind it for hotkeys. An
		// explicit hwnd is the caller's own choice, so it is always returned -
		// "hide to tray" is exactly when one asks about a hidden window.
		if (!single && !IsWindowVisible(hwnd))
			continue;

		Json item = NodeOf(hwnd, hwnd, 0);
		item.Set("dpi", Json((double)DpiOf(hwnd)));
		if (wantItems && !target)
			item.Set("items_error", Json("打不开目标进程（权限不足或已退出），只返回了窗口结构"));
		else if (wantItems)
			AttachEntries(hwnd, item, target, rows);
		if (tree)
		{
			UiWalk walk;
			walk.budget = max - total;
			walk.root = hwnd;
			walk.process = target;
			walk.entries = wantItems;
			walk.rows = rows;
			if (walk.budget > 0)
				WalkChildren(hwnd, walk, 1);
			total += walk.count;
			truncated = truncated || walk.truncated;
			item.Set("controls", std::move(walk.items));
		}
		windows.Push(std::move(item));
		++shown;
	}
	if (target)
		CloseHandle(target);

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	if (!sessionId)
		result.Set("pid", Json((double)pid));
	result.Set("windows", std::move(windows));
	result.Set("count", Json((double)shown));
	result.Set("controls", Json((double)total));
	result.Set("truncated", Json(truncated));
	if (shown == 0)
		result.Set("note", Json(sessionId
			? "该会话目前没有可见窗口：窗口可能还没建好，也可能脚本已经退出。"
			: "目标进程目前没有可见顶层窗口：pid 可能不对，或窗口还没建好。"));
	return result.Dump();
}

// ------------------------------------------------------- m187: capture_ui
//
// inspect_ui answers "where is it and what does it say"; this answers "what does
// it look like". They are complements: coordinates cannot show a 1px gap, a font
// that renders blurry or an anti-aliased corner, and pixels cannot be read as a
// field/value table.
//
// PrintWindow is the whole point. A screen copy only ever returns whatever sits
// on top, and on this machine the foreground is routinely another application,
// so a helper that needs the window in front is useless for iterating on a Gui.
// Measured on a script-owned Gui fully covered by another window: PrintWindow
// returned byte-identical pixels to the uncovered capture, while a screen copy
// of the very same moment returned the covering window.
// ------------------------------------------------- m208/m209 shared capture core
//
// capture_ui did its DC / PrintWindow / GetDIBits dance inline. ui_diff reuses
// exactly those pixels, so the dance moved into this helper and both tools call
// it. Same contract as the original inline code: PrintWindow first (window-
// owned pixels, independent of z-order), screen BitBlt as the fallback.
static bool CaptureWindowPixels(HWND hwnd, int aW, int aH,
	std::vector<unsigned char> &aPixels, int &aStride, bool &aFromScreen)
{
	aFromScreen = false;
	HDC src = GetWindowDC(hwnd);
	if (!src)
		return false;
	HDC mem = CreateCompatibleDC(src);
	HBITMAP bitmap = CreateCompatibleBitmap(src, aW, aH);
	if (!mem || !bitmap)
	{
		if (bitmap)
			DeleteObject(bitmap);
		if (mem)
			DeleteDC(mem);
		ReleaseDC(hwnd, src);
		return false;
	}

	HGDIOBJ old = SelectObject(mem, bitmap);
	BOOL painted = PrintWindow(hwnd, mem, 2);
	if (!painted)
	{
		painted = BitBlt(mem, 0, 0, aW, aH, src, 0, 0, SRCCOPY);
		aFromScreen = painted != FALSE;
	}

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = aW;
	info.bmiHeader.biHeight = -aH; // top-down: row 0 is the top of the window
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 24;
	info.bmiHeader.biCompression = BI_RGB;
	aStride = ((aW * 3 + 3) / 4) * 4;
	info.bmiHeader.biSizeImage = (DWORD)aStride * (DWORD)aH;
	aPixels.assign((size_t)aStride * (size_t)aH + 4, 0);
	const int lines = GetDIBits(mem, bitmap, 0, (UINT)aH, aPixels.data(), &info,
		DIB_RGB_COLORS);

	SelectObject(mem, old);
	DeleteObject(bitmap);
	DeleteDC(mem);
	ReleaseDC(hwnd, src);
	return painted != FALSE && lines == aH;
}

// ui_diff reads back what capture_ui wrote, so only the one BMP flavor
// WriteBmp24 produces has to parse: 24-bit, BI_RGB, positive (bottom-up) height.
// Rows are flipped to top-down on the way in so a row index doubles as the
// client-space y coordinate.
static bool ReadBmp24File(const std::wstring &aPath,
	std::vector<unsigned char> &aPixels, int &aW, int &aH, int &aStride)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	BITMAPFILEHEADER fh = {};
	BITMAPINFOHEADER ih = {};
	DWORD got = 0;
	bool ok = ReadFile(file, &fh, sizeof(fh), &got, nullptr) && got == sizeof(fh)
		&& ReadFile(file, &ih, sizeof(ih), &got, nullptr) && got == sizeof(ih);
	if (ok)
		ok = fh.bfType == 0x4D42 && ih.biBitCount == 24
			&& ih.biCompression == BI_RGB && ih.biWidth > 0 && ih.biHeight > 0
			&& ih.biWidth <= 8192 && ih.biHeight <= 8192;
	if (!ok)
	{
		CloseHandle(file);
		return false;
	}

	aW = ih.biWidth;
	aH = ih.biHeight;
	aStride = ((aW * 3 + 3) / 4) * 4;
	const DWORD rowBytes = (DWORD)aW * 3;
	aPixels.assign((size_t)aStride * (size_t)aH, 0);
	std::vector<unsigned char> row((size_t)aStride, 0);
	LARGE_INTEGER pos = {};
	for (int y = 0; ok && y < aH; ++y)
	{
		pos.QuadPart = (LONGLONG)fh.bfOffBits + (LONGLONG)(aH - 1 - y) * aStride;
		ok = SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)
			&& ReadFile(file, row.data(), rowBytes, &got, nullptr)
			&& got == rowBytes;
		if (ok)
			std::memcpy(aPixels.data() + (size_t)y * (size_t)aStride,
				row.data(), rowBytes);
	}
	CloseHandle(file);
	return ok;
}

// Tool results only ever carry exe-relative paths (no drive letters); ui_diff
// takes such a path back, so it resolves the same way every other consumer of
// these paths does.
static std::wstring ResolveToolPath(const std::string &aPathUtf8)
{
	const std::wstring wide = WideOf(aPathUtf8);
	if (wide.size() >= 2 && (wide[1] == L':' || (wide[0] == L'\\' && wide[1] == L'\\')))
		return wide;
	const std::wstring dir = ExeDirectoryForTools();
	return dir.empty() ? wide : dir + L"\\" + wide;
}

std::string Toolbox::ToolCaptureUi(const Json &aArgs)
{
	if (!aArgs.Has("session") || !aArgs["session"].IsNumber())
		return ErrorResult("session 是必需参数（来自 run_script、run_editor 或 list_sessions）").Dump();

	const DWORD sessionId = (DWORD)aArgs["session"].AsInt(0);
	Session session;
	if (!Engine::Instance().GetSession(sessionId, session))
		return ErrorResult("未知会话: " + std::to_string(sessionId)).Dump();
	if (session.threadId == 0)
		return ErrorResult("会话还没有脚本进程（脚本可能连加载都没走到）").Dump();

	int waitMs = aArgs["wait_ms"].AsInt(1500);
	waitMs = waitMs < 0 ? 0 : (waitMs > 5000 ? 5000 : waitMs);

	// Same wait rule as inspect_ui: the Gui is created asynchronously, and an
	// empty answer at that moment reads as "my window never appeared".
	std::vector<HWND> tops;
	const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)waitMs;
	for (;;)
	{
		tops.clear();
		CollectTopWindows(session.threadId, tops);
		bool anyVisible = false;
		for (HWND hwnd : tops)
		{
			if (IsWindowVisible(hwnd))
			{
				anyVisible = true;
				break;
			}
		}
		if (anyVisible || GetTickCount64() >= deadline)
			break;
		Sleep(60);
	}

	// Hidden top-level windows are noise: a script often keeps one behind the
	// real Gui for hotkeys, and capturing it would return an empty rectangle.
	std::vector<HWND> visible;
	Json list = Json::MakeArray();
	for (HWND hwnd : tops)
	{
		if (!IsWindowVisible(hwnd))
			continue;
		RECT rc = {};
		GetWindowRect(hwnd, &rc);
		Json item = Json::MakeObject();
		item.Set("index", Json((double)visible.size()));
		item.Set("class", Json(ClassOf(hwnd)));
		item.Set("text", Json(TextOf(hwnd)));
		item.Set("screen", BoxOf(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top));
		list.Push(std::move(item));
		visible.push_back(hwnd);
	}
	if (visible.empty())
	{
		Json result = ErrorResult("该会话目前没有可见窗口：窗口可能还没建好，也可能脚本已经退出");
		result.Set("windows", std::move(list));
		return result.Dump();
	}

	const int index = aArgs["window"].AsInt(0);
	if (index < 0 || (size_t)index >= visible.size())
	{
		Json result = ErrorResult("window 越界：本次只有 "
			+ std::to_string(visible.size()) + " 个可见窗口（序号从 0 开始）");
		result.Set("windows", std::move(list));
		return result.Dump();
	}
	HWND hwnd = visible[(size_t)index];

	RECT win = {};
	GetWindowRect(hwnd, &win);
	const int winW = win.right - win.left;
	const int winH = win.bottom - win.top;
	if (winW <= 0 || winH <= 0)
		return ErrorResult("窗口尺寸为 0，没有可截的内容").Dump();

	// rect is CLIENT coordinates - the same frame inspect_ui reports - so a
	// caller can lift a control's box straight out of inspect_ui and pass it in.
	POINT origin = {0, 0};
	ClientToScreen(hwnd, &origin);
	int cropX = 0;
	int cropY = 0;
	int cropW = winW;
	int cropH = winH;
	if (aArgs.Has("rect"))
	{
		const std::string rect = aArgs["rect"].AsString();
		// Parsed by hand: sscanf is banned at this warning level, and this keeps
		// the accepted form exactly "x,y,w,h" with no locale surprises.
		int parts[4] = {0, 0, 0, 0};
		int count = 0;
		const char *cursor = rect.c_str();
		while (count < 4)
		{
			char *end = nullptr;
			const long value = std::strtol(cursor, &end, 10);
			if (end == cursor)
				break;
			parts[count++] = (int)value;
			cursor = end;
			while (*cursor == ' ' || *cursor == ',')
				++cursor;
		}
		int rw = parts[2];
		int rh = parts[3];
		if (count != 4 || rw <= 0 || rh <= 0)
			return ErrorResult("rect 需要 \"x,y,w,h\" 四个整数（客户区坐标，与 inspect_ui 的 client 同口径）").Dump();
		cropX = (origin.x - win.left) + parts[0];
		cropY = (origin.y - win.top) + parts[1];
		if (cropX < 0 || cropY < 0 || cropX >= winW || cropY >= winH)
			return ErrorResult("rect 落在窗口之外").Dump();
		cropW = rw;
		cropH = rh;
		if (cropX + cropW > winW)
			cropW = winW - cropX;
		if (cropY + cropH > winH)
			cropH = winH - cropY;
	}
	if ((long long)cropW * (long long)cropH * 3LL > 96LL * 1024 * 1024)
		return ErrorResult("要截的区域太大（超过 96 MB 像素）：用 rect 只截关心的那一块").Dump();

	// m208/m209 moved the DC / PrintWindow / GetDIBits dance into
	// CaptureWindowPixels() so ui_diff can share it verbatim.
	std::vector<unsigned char> pixels;
	int stride = 0;
	bool fromScreen = false;
	if (!CaptureWindowPixels(hwnd, winW, winH, pixels, stride, fromScreen))
		return ErrorResult("这一帧没有取到像素（窗口可能正在销毁，或它拒绝 WM_PRINT）").Dump();

	// m210: a frame taken mid-repaint reads as a defect that is not one - the
	// scroll probes raced exactly this several times before it existed. With
	// stable>=2 the frame is re-captured until it repeats that many times in a
	// row (or the timeout hits, and the last frame is returned as-is).
	int stableFrames = aArgs["stable"].AsInt(0);
	stableFrames = stableFrames < 0 ? 0 : (stableFrames > 20 ? 20 : stableFrames);
	if (stableFrames > 1)
	{
		int stableTimeout = aArgs["stable_timeout"].AsInt(3000);
		stableTimeout = stableTimeout < 0 ? 0 : (stableTimeout > 10000 ? 10000 : stableTimeout);
		const ULONGLONG stableDeadline = GetTickCount64() + (ULONGLONG)stableTimeout;
		int consecutive = 1;
		while (consecutive < stableFrames && GetTickCount64() < stableDeadline)
		{
			Sleep(60);
			std::vector<unsigned char> cur;
			int curStride = 0;
			bool screen2 = false;
			if (!CaptureWindowPixels(hwnd, winW, winH, cur, curStride, screen2))
				break;
			if (cur == pixels)
			{
				++consecutive;
			}
			else
			{
				consecutive = 1;
				pixels.swap(cur);
			}
		}
	}

	const std::wstring work = AppTempDir(kTempWork);
	if (work.empty())
		return ErrorResult("取不到临时目录").Dump();
	const std::wstring shots = work + L"\\shots";
	CreateDirectoryW(shots.c_str(), nullptr);

	SYSTEMTIME st = {};
	GetLocalTime(&st);
	char leaf[96];
	std::snprintf(leaf, sizeof(leaf),
		"capture-s%u-w%d-%04u%02u%02u-%02u%02u%02u-%03u.bmp",
		(unsigned)sessionId, index, st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	const std::wstring path = shots + L"\\" + WideOf(std::string(leaf));

	bool written = false;
	if (cropW == winW && cropH == winH)
	{
		written = WriteBmp24(path, cropW, cropH, pixels.data(), stride);
	}
	else
	{
		const int cropStride = ((cropW * 3 + 3) / 4) * 4;
		std::vector<unsigned char> cropped((size_t)cropStride * (size_t)cropH, 0);
		for (int y = 0; y < cropH; ++y)
			std::memcpy(cropped.data() + (size_t)y * (size_t)cropStride,
				pixels.data() + (size_t)(cropY + y) * (size_t)stride
					+ (size_t)cropX * 3,
				(size_t)cropW * 3);
		written = WriteBmp24(path, cropW, cropH, cropped.data(), cropStride);
	}
	if (!written)
		return ErrorResult("写出截图文件失败").Dump();

	// A hardware-accelerated surface paints nothing through WM_PRINT and comes
	// back black. Saying so beats handing over a black rectangle that the caller
	// reads as "the window is empty".
	bool allBlack = true;
	for (int y = 0; y < cropH && allBlack; y += 4)
	{
		for (int x = 0; x < cropW; x += 4)
		{
			const unsigned char *p = pixels.data()
				+ (size_t)(cropY + y) * (size_t)stride + (size_t)(cropX + x) * 3;
			if (p[0] || p[1] || p[2])
			{
				allBlack = false;
				break;
			}
		}
	}

	WIN32_FILE_ATTRIBUTE_DATA attr = {};
	const bool haveAttr = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
		&attr) != FALSE;

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	result.Set("window", Json((double)index));
	result.Set("title", Json(TextOf(hwnd)));
	result.Set("path", Json(AppTempRelative(kTempWork, std::string("shots/") + leaf)));
	result.Set("format", Json("bmp"));
	result.Set("width", Json((double)cropW));
	result.Set("height", Json((double)cropH));
	if (haveAttr)
		result.Set("bytes", Json((double)((ULONGLONG)attr.nFileSizeHigh << 32
			| attr.nFileSizeLow)));
	result.Set("print_window", Json(!fromScreen));
	result.Set("windows", std::move(list));
	if (allBlack)
		result.Set("note", Json("整张图全黑：该窗口可能是硬件加速（D3D/DirectComposition）表面，"
			"不响应 WM_PRINT；这不是窗口真的空着。可改截它的父窗口，或用 inspect_ui 看结构。"));
	else if (fromScreen)
		result.Set("note", Json("这个窗口不响应 WM_PRINT，已退回屏幕拷贝；"
			"若此时窗口被遮挡，图里会是遮挡它的窗口内容。"));
	return result.Dump();
}

// ------------------------------------------------------- m208: send_input
//
// 采集侧（inspect_ui / capture_ui / ui_pixel）只回答「长什么样」；K32 一类的问题
// 只有把消息真的送进控件才暴露——回调用错、右键菜单挂错消息、滚轮焦点错位，
// 截图全是常态。注入用 PostMessage 直发，不碰真实光标键盘：不抢前台（这台机器
// 的前台常被占用）、目标是本宿主会话自己的窗口所以不受 UIPI 限制、失败不落任何
// 系统状态。消息序列与真实输入一致（DOWN→UP；滚轮 wParam 带 delta），控件端
// 无法区分来源，这正是验证所需要的。
static int VkOfName(const char *aName)
{
	static const struct { const char *name; int vk; } table[] = {
		{ "Enter", VK_RETURN }, { "Esc", VK_ESCAPE }, { "Escape", VK_ESCAPE },
		{ "Tab", VK_TAB }, { "Space", VK_SPACE },
		{ "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
		{ "Home", VK_HOME }, { "End", VK_END },
		{ "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
		{ "Delete", VK_DELETE }, { "Backspace", VK_BACK }, { "Insert", VK_INSERT },
	};
	for (const auto &e : table)
		if (_stricmp(e.name, aName) == 0)
			return e.vk;
	if (aName[0] == 'F' && aName[1] >= '1' && aName[1] <= '9')
	{
		const int n = std::atoi(aName + 1);
		if (n >= 1 && n <= 12)
			return VK_F1 + n - 1;
	}
	return 0;
}

std::string Toolbox::ToolSendInput(const Json &aArgs)
{
	if (!aArgs.Has("session") || !aArgs["session"].IsNumber())
		return ErrorResult("session 是必需参数（来自 run_script、run_editor 或 list_sessions）").Dump();
	const DWORD sessionId = (DWORD)aArgs["session"].AsInt(0);
	Session session;
	if (!Engine::Instance().GetSession(sessionId, session))
		return ErrorResult("未知会话: " + std::to_string(sessionId)).Dump();
	if (session.threadId == 0)
		return ErrorResult("会话还没有脚本进程（脚本可能连加载都没走到）").Dump();

	const std::string action = aArgs["action"].AsString();
	if (action != "click" && action != "rclick" && action != "dblclick"
		&& action != "wheel" && action != "key" && action != "text" && action != "move")
		return ErrorResult("action 取值: click / rclick / dblclick / wheel / key / text / move").Dump();

	std::vector<HWND> tops;
	CollectTopWindows(session.threadId, tops);
	std::vector<HWND> visible;
	for (HWND hwnd : tops)
		if (IsWindowVisible(hwnd))
			visible.push_back(hwnd);
	if (visible.empty())
		return ErrorResult("该会话目前没有可见窗口：窗口可能还没建好，也可能脚本已经退出").Dump();
	const int index = aArgs["window"].AsInt(0);
	if (index < 0 || (size_t)index >= visible.size())
		return ErrorResult("window 越界：本次只有 "
			+ std::to_string(visible.size()) + " 个可见窗口（序号从 0 开始）").Dump();
	HWND top = visible[(size_t)index];

	RECT client = {};
	GetClientRect(top, &client);
	int x = aArgs["x"].AsInt(-1);
	int y = aArgs["y"].AsInt(-1);
	if (x < 0)
		x = (client.right - client.left) / 2;
	if (y < 0)
		y = (client.bottom - client.top) / 2;
	if (x >= client.right || y >= client.bottom)
		return ErrorResult("坐标落在客户区之外（用 inspect_ui 的 client 口径，左上角为 0,0）").Dump();

	// Same rule a real click follows: the point picks the deepest non-transparent
	// child, so a Text block WITHOUT SS_NOTIFY (K19) correctly lands on its
	// parent instead of magically reaching the control.
	POINT pt = { x, y };
	HWND target = RealChildWindowFromPoint(top, pt);
	if (!target)
		target = top;
	POINT tpt = pt;
	MapWindowPoints(top, target, &tpt, 1);
	POINT origin = { 0, 0 };
	ClientToScreen(top, &origin);
	const POINT screenPt = { origin.x + x, origin.y + y };

	int posted = 0;
	HWND keyTarget = target;
	Json resultExtra;
	if (action == "click" || action == "dblclick" || action == "rclick")
	{
		const bool right = action == "rclick";
		const UINT down = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
		const UINT up = right ? WM_RBUTTONUP : WM_LBUTTONUP;
		const WPARAM wk = right ? MK_RBUTTON : MK_LBUTTON;
		const LPARAM lp = MAKELPARAM(tpt.x, tpt.y);
		PostMessageW(target, down, wk, lp);
		++posted;
		Sleep(30);
		PostMessageW(target, up, 0, lp);
		++posted;
		if (action == "dblclick")
		{
			Sleep(30);
			PostMessageW(target, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
			++posted;
			Sleep(30);
			PostMessageW(target, WM_LBUTTONUP, 0, lp);
			++posted;
		}
	}
	else if (action == "wheel")
	{
		int delta = aArgs["delta"].AsInt(-120);
		delta = delta < -1200 ? -1200 : (delta > 1200 ? 1200 : delta);
		if (!delta)
			return ErrorResult("delta 不能为 0（一个滚轮格是 ±120）").Dump();
		// WM_MOUSEWHEEL's lParam is screen coordinates, not client ones.
		PostMessageW(target, WM_MOUSEWHEEL,
			MAKEWPARAM(0, (WORD)(short)delta), MAKELPARAM(screenPt.x, screenPt.y));
		++posted;
	}
	else if (action == "move")
	{
		PostMessageW(target, WM_MOUSEMOVE, 0, MAKELPARAM(tpt.x, tpt.y));
		++posted;
	}
	else // key / text go to the focused control, like real input does
	{
		const DWORD guiThread = GetWindowThreadProcessId(top, nullptr);
		HWND focus = nullptr;
		bool focusAttached = false;
		GUITHREADINFO ti = {};
		ti.cbSize = sizeof(ti);
		if (GetGUIThreadInfo(guiThread, &ti) && ti.hwndFocus)
			focus = ti.hwndFocus;
		if (!focus)
		{
			// 兜底路径 = 真实打字的等价物：临时挂进目标线程的输入队列，
			// 读不到焦点就把焦点给坐标命中的那个控件（用户"点进去再打字"）。
			const DWORD hostThread = GetCurrentThreadId();
			if (AttachThreadInput(hostThread, guiThread, TRUE))
			{
				focus = GetFocus();
				if (!focus && SetFocus(target))
				{
					focus = GetFocus();
					focusAttached = focus != nullptr;
				}
				AttachThreadInput(hostThread, guiThread, FALSE);
			}
		}
		if (!focus)
			return ErrorResult("查不到该会话的焦点控件，挂入输入队列也没拿到："
				"先用 action=click 点一下要输入的控件，再发 key / text").Dump();
		keyTarget = focus;
		Json focusInfo = Json::MakeObject();
		focusInfo.Set("class", Json(ClassOf(keyTarget)));
		focusInfo.Set("text", Json(TextOf(keyTarget)));
		focusInfo.Set("set_by_tool", Json(focusAttached));
		resultExtra = std::move(focusInfo);
		if (action == "key")
		{
			if (!aArgs.Has("key"))
				return ErrorResult("key 需要键名：Enter / Esc / Tab / Space / PageUp / PageDown /"
					" Home / End / Up / Down / Left / Right / Delete / Backspace / Insert / F1..F12").Dump();
			const std::string key = aArgs["key"].AsString();
			const int vk = VkOfName(key.c_str());
			if (!vk)
				return ErrorResult("不认识的键名: " + key).Dump();
			PostMessageW(keyTarget, WM_KEYDOWN, (WPARAM)vk, 0x00000001);
			++posted;
			Sleep(30);
			PostMessageW(keyTarget, WM_KEYUP, (WPARAM)vk, 0xC0000001);
			++posted;
		}
		else
		{
			if (!aArgs.Has("text"))
				return ErrorResult("text 需要一段要输入的文字").Dump();
			const std::wstring wide = WideOf(aArgs["text"].AsString());
			if (wide.empty())
				return ErrorResult("text 不能为空").Dump();
			for (wchar_t ch : wide)
			{
				PostMessageW(keyTarget, WM_CHAR, (WPARAM)ch, 0x00000001);
				++posted;
			}
		}
	}

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	result.Set("action", Json(action));
	result.Set("posted", Json((double)posted));
	result.Set("point", BoxOf(x, y, 0, 0));
	Json tgt = Json::MakeObject();
	tgt.Set("kind", Json(keyTarget == top ? "window" : "control"));
	tgt.Set("class", Json(ClassOf(keyTarget)));
	tgt.Set("text", Json(TextOf(keyTarget)));
	result.Set("target", std::move(tgt));
	if (resultExtra.IsObject())
		result.Set("focus", std::move(resultExtra));
	// K46 in action: a ListView swallows WM_RBUTTONUP inside its DragDetect
	// loop, so a right-click arriving there may never produce the UP. The menu
	// must be raised on the DOWN - saying it here saves the caller a mystery.
	if (action == "rclick" && ClassOf(target).find("SysListView32") != std::string::npos)
		result.Set("note", Json("目标是 ListView：右键抬起会被它的 DragDetect 循环吞掉（K46），"
			"脚本里的菜单应挂在按下（WM_RBUTTONDOWN）时弹。消息本身已按真实序列发出。"));
	return result.Dump();
}

// ------------------------------------------------------- m209: ui_diff
//
// Pixel-level regressions (a caption that shifted 16px after sorting, rows
// clipped every 4th line, a flash after scrolling) were verified by hand-rolled
// throwaway BMP parsers every single time. Reading back two capture_ui shots
// and reporting count / bounding box / worst row bands turns those checks into
// one call an AI can assert against.
std::string Toolbox::ToolUiDiff(const Json &aArgs)
{
	if (!aArgs.Has("a") || !aArgs.Has("b"))
		return ErrorResult("a 与 b 都是必需参数（capture_ui 返回的 path）").Dump();
	std::vector<unsigned char> pa, pb;
	int wa = 0, ha = 0, sa = 0, wb = 0, hb = 0, sb = 0;
	if (!ReadBmp24File(ResolveToolPath(aArgs["a"].AsString()), pa, wa, ha, sa))
		return ErrorResult("读不了 a："
			+ aArgs["a"].AsString() + "（应为 capture_ui 返回的 BMP 相对路径）").Dump();
	if (!ReadBmp24File(ResolveToolPath(aArgs["b"].AsString()), pb, wb, hb, sb))
		return ErrorResult("读不了 b："
			+ aArgs["b"].AsString() + "（应为 capture_ui 返回的 BMP 相对路径）").Dump();
	if (wa != wb || ha != hb)
	{
		Json result = ErrorResult("两张图尺寸不同，没法逐像素比：先确认两次截图之间窗口没被缩放");
		result.Set("a", BoxOf(wa, ha, 0, 0));
		result.Set("b", BoxOf(wb, hb, 0, 0));
		return result.Dump();
	}

	int threshold = aArgs["threshold"].AsInt(0);
	threshold = threshold < 0 ? 0 : (threshold > 255 ? 255 : threshold);
	int rowsLimit = aArgs["rows"].AsInt(10);
	rowsLimit = rowsLimit < 1 ? 1 : (rowsLimit > 50 ? 50 : rowsLimit);

	long long changed = 0;
	int minX = wa, minY = ha, maxX = -1, maxY = -1;
	std::vector<long long> rowCount((size_t)ha, 0);
	std::vector<std::pair<int, int>> samples;
	for (int y = 0; y < ha; ++y)
	{
		const unsigned char *ra = pa.data() + (size_t)y * (size_t)sa;
		const unsigned char *rb = pb.data() + (size_t)y * (size_t)sb;
		for (int x = 0; x < wa; ++x)
		{
			const unsigned char *u = ra + (size_t)x * 3;
			const unsigned char *v = rb + (size_t)x * 3;
			const int d = std::abs(u[0] - v[0]) + std::abs(u[1] - v[1])
				+ std::abs(u[2] - v[2]);
			if (d <= threshold)
				continue;
			++changed;
			++rowCount[(size_t)y];
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
			if ((int)samples.size() < 5)
				samples.push_back({ x, y });
		}
	}

	Json result = OkResult();
	result.Set("width", Json((double)wa));
	result.Set("height", Json((double)ha));
	result.Set("same", Json(changed == 0));
	if (!changed)
	{
		result.Set("note", Json("两张图逐像素一致（threshold="
			+ std::to_string(threshold) + "）"));
		return result.Dump();
	}
	result.Set("diff_pixels", Json((double)changed));
	const double total = (double)wa * (double)ha;
	char ratio[32];
	std::snprintf(ratio, sizeof(ratio), "%.4f", (double)changed / total);
	result.Set("diff_ratio", Json(ratio));
	result.Set("bbox", BoxOf(minX, minY, maxX - minX + 1, maxY - minY + 1));

	// Contiguous row bands, worst first. This is what turned "some rows look
	// clipped" into a fact: the clipped rows showed up as evenly spaced bands
	// (every 4th row), and a repaint ghost showed up as one solid band.
	std::vector<std::pair<long long, std::pair<int, int>>> bands; // count, {y0,y1}
	for (int y = 0; y < ha;)
	{
		if (!rowCount[(size_t)y])
		{
			++y;
			continue;
		}
		const int y0 = y;
		long long sum = 0;
		while (y < ha && rowCount[(size_t)y])
		{
			sum += rowCount[(size_t)y];
			++y;
		}
		bands.push_back({ sum, { y0, y - 1 } });
	}
	std::sort(bands.begin(), bands.end(),
		[](const auto &u, const auto &v) { return u.first > v.first; });
	Json bandList = Json::MakeArray();
	const size_t bandCount = bands.size() < (size_t)rowsLimit
		? bands.size() : (size_t)rowsLimit;
	for (int i = 0; i < bandCount; ++i)
	{
		Json b = Json::MakeObject();
		b.Set("y0", Json((double)bands[(size_t)i].second.first));
		b.Set("y1", Json((double)bands[(size_t)i].second.second));
		b.Set("changed", Json((double)bands[(size_t)i].first));
		bandList.Push(std::move(b));
	}
	result.Set("bands", std::move(bandList));
	result.Set("band_total", Json((double)bands.size()));
	Json sampleList = Json::MakeArray();
	for (const auto &s : samples)
	{
		Json p = Json::MakeObject();
		p.Set("x", Json((double)s.first));
		p.Set("y", Json((double)s.second));
		sampleList.Push(std::move(p));
	}
	result.Set("samples", std::move(sampleList));
	result.Set("note", Json("bands 按变化量降序只列前几条；要全貌把 rows 调大。"
		"等宽等距的 band 是行级缺陷（裁字/残影），一整片大 band 是整块重绘或位移。"));
	return result.Dump();
}

std::string Toolbox::ToolPixel(const Json &aArgs)
{
	if (!aArgs.Has("x") || !aArgs.Has("y"))
		return ErrorResult("x 与 y 都是必需参数（屏幕坐标）").Dump();
	const int x = aArgs["x"].AsInt(0);
	const int y = aArgs["y"].AsInt(0);

	HDC screen = GetDC(nullptr);
	if (!screen)
		return ErrorResult("取不到屏幕 DC").Dump();
	const COLORREF color = GetPixel(screen, x, y);
	ReleaseDC(nullptr, screen);

	Json result = OkResult();
	result.Set("x", Json((double)x));
	result.Set("y", Json((double)y));
	if (color == CLR_INVALID)
	{
		result.Set("rgb", Json(""));
		result.Set("reason", Json("坐标在屏幕外，或该区域取不到像素"));
		return result.Dump();
	}
	char text[16];
	std::snprintf(text, sizeof(text), "%02X%02X%02X",
		(unsigned)GetRValue(color), (unsigned)GetGValue(color), (unsigned)GetBValue(color));
	result.Set("rgb", Json(std::string(text)));
	return result.Dump();
}

// --------------------------------------------------------------- m177: help
//
// The deck ships the AutoHotkey 2.0.30 help text as an index (docs\help-index.json,
// built by tools/make_help_index.py from AutoHotkey.chm). This is the only way a
// caller can confirm a signature: the chm itself is compressed HTML that no model
// can read, and the vendored markdown mirror stops at 2.0.18.
//
// The whole design is about answering one question cheaply. A wrong API is the
// expensive failure mode - it parses, it runs, it does the wrong thing - and no
// amount of validation catches "the third parameter is a format string, not the
// initial value". So the tool is an exact-match-first lookup with a fuzzy
// fallback, and every field is bounded so a five-hit answer stays near 4 KB.

namespace
{
// Leading/trailing whitespace would silently turn " StrSplit " into a miss, and
// a caller that pastes a name out of a sentence always brings a space with it.
std::string HelpTrim(const std::string &aText)
{
	const size_t begin = aText.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return std::string();
	const size_t end = aText.find_last_not_of(" \t\r\n");
	return aText.substr(begin, end - begin + 1);
}

// Second-stage cap, applied at query time on top of the generator's own limit.
// Truncating on a word boundary matters here: a description cut mid-token reads
// as a typo'd identifier to whoever is copying it.
std::string HelpSlice(const std::string &aText, size_t aLimit)
{
	if (aText.size() <= aLimit)
		return aText;
	size_t cut = aText.rfind(' ', aLimit);
	if (cut == std::string::npos || cut < aLimit / 2)
		cut = aLimit;
	return aText.substr(0, cut) + "...";
}

// Rebuilds an entry with only the fields this caller asked for. Json has no
// Erase, and mutating the cached array would poison the next caller, so the
// narrowing has to go through a fresh object.
Json NarrowHelpEntry(const Json &aEntry, bool aFull)
{
	Json out = Json::MakeObject();
	out.Set("name", aEntry["name"]);
	out.Set("syntax", aEntry["syntax"]);
	const Json &aliases = aEntry["aliases"];
	if (aliases.IsArray())
		out.Set("aliases", aliases);
	out.Set("summary", aEntry["summary"]);

	const Json &params = aEntry["params"];
	if (params.IsArray())
	{
		Json kept = Json::MakeArray();
		for (size_t i = 0; i < params.Size(); ++i)
		{
			const Json &p = params[i];
			Json item = Json::MakeObject();
			item.Set("name", p["name"]);
			item.Set("type", p["type"]);
			const std::string desc = p["desc"].AsString();
			if (!desc.empty())
				item.Set("desc", Json(aFull ? desc : HelpSlice(desc, 140)));
			kept.Push(item);
		}
		if (kept.Size() > 0)
			out.Set("params", kept);
	}

	const std::string returns = aEntry["returns"].AsString();
	if (!returns.empty())
		out.Set("returns", Json(returns));
	// The example is the single largest field (~360 B) and the least often
	// needed, so it is opt-in.
	if (aFull)
	{
		const std::string example = aEntry["example"].AsString();
		if (!example.empty())
			out.Set("example", Json(example));
	}
	return out;
}
} // namespace

void Toolbox::EnsureHelpIndexLoaded()
{
	std::lock_guard<std::mutex> guard(mHelpMutex);
	if (mHelpLoaded)
		return;
	mHelpLoaded = true;

	const std::wstring path = ExeDirectoryForTools() + L"\\docs\\help-index.json";
	TextDocument document;
	std::string error;
	if (!ReadTextDocument(path, document, error))
	{
		mHelpError = "读不到 docs\\help-index.json（" + error +
			"）：这份索引随程序分发，缺了说明安装不完整";
		return;
	}
	Json root;
	if (!Json::Parse(document.utf8, root, error))
	{
		mHelpError = "docs\\help-index.json 解析失败（" + error + "）";
		return;
	}
	mHelpEntries = root["entries"];
	if (!mHelpEntries.IsArray() || mHelpEntries.Size() == 0)
	{
		mHelpError = "docs\\help-index.json 里没有 entries；用 tools/make_help_index.py 重新生成";
		mHelpEntries = Json::MakeArray();
	}
}

std::string Toolbox::ToolLookupHelp(const Json &aArgs)
{
	const std::string query = HelpTrim(aArgs["query"].AsString());
	if (query.empty())
		return ErrorResult("query 不能为空：填英文函数名或内建变量名，例如 StrSplit、A_Clipboard").Dump();

	int limit = aArgs["limit"].AsInt(3);
	if (limit < 1)
		limit = 1;
	if (limit > 10)
		limit = 10;
	const bool full = aArgs["full"].AsBool(false);

	EnsureHelpIndexLoaded();
	if (!mHelpError.empty())
		return ErrorResult(mHelpError).Dump();

	const std::string needle = LowerAscii(query);

	// Four rungs, and the order is the answer's honesty: a name hit means the
	// caller used a real identifier, a summary hit means it is fishing. Mixing
	// them would let a fuzzy match look authoritative, which is exactly the
	// failure this tool exists to prevent.
	std::vector<size_t> exact;
	std::vector<size_t> aliased;
	std::vector<size_t> partial;
	std::vector<size_t> inSummary;
	for (size_t i = 0; i < mHelpEntries.Size(); ++i)
	{
		const Json &entry = mHelpEntries[i];
		const std::string name = LowerAscii(entry["name"].AsString());
		if (name == needle)
		{
			exact.push_back(i);
			continue;
		}
		bool aliasHit = false;
		const Json &aliases = entry["aliases"];
		for (size_t a = 0; a < aliases.Size() && !aliasHit; ++a)
			aliasHit = LowerAscii(aliases[a].AsString()) == needle;
		if (aliasHit)
		{
			aliased.push_back(i);
			continue;
		}
		if (name.find(needle) != std::string::npos)
		{
			partial.push_back(i);
			continue;
		}
		if (LowerAscii(entry["summary"].AsString()).find(needle) != std::string::npos)
			inSummary.push_back(i);
	}

	Json hits = Json::MakeArray();
	size_t shown = 0;
	// Summary matches are only offered when nothing better exists: "String" as a
	// search word would otherwise bury StrSplit under forty pages that merely
	// mention the word.
	const std::vector<size_t> *order[4] = {&exact, &aliased, &partial, &inSummary};
	size_t total = 0;
	for (int rung = 0; rung < 4; ++rung)
	{
		const std::vector<size_t> &bucket = *order[rung];
		if (bucket.empty())
			continue;
		if (rung == 3 && total > 0)
			break; // a real hit already answered the question
		total += bucket.size();
		for (size_t i = 0; i < bucket.size() && shown < (size_t)limit; ++i)
		{
			hits.Push(NarrowHelpEntry(mHelpEntries[bucket[i]], full));
			++shown;
		}
	}

	Json result = OkResult();
	result.Set("query", Json(query));
	result.Set("exact", Json(!exact.empty() || !aliased.empty()));
	result.Set("total", Json((double)total));
	result.Set("hits", std::move(hits));
	if (shown == 0)
		result.Set("hint", Json("没有匹配。用英文函数名或 A_ 开头的变量名（StrSplit、A_Clipboard），"
			"中文描述查不到；也可以只给名字的开头几个字母，例如 Clip。"));
	else if (total > shown)
		result.Set("hint", Json("还有 " + std::to_string(total - shown) +
			" 条没返回，调大 limit 或把 query 写全"));
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

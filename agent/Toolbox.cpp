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

std::string Tail(const std::string &aText, size_t aLimit)
{
	if (aText.size() <= aLimit)
		return aText;
	return "[...truncated...]\n" + aText.substr(aText.size() - aLimit);
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

	{
		ToolDef d;
		d.name = "list_scripts";
		d.description = "列出可操作的脚本（id、名称、字节数、是否在运行）。";
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "read_script";
		d.description = "读取脚本文本、编码与行尾。";
		d.properties.Set("script", arg("string", "脚本 id，来自 list_scripts"));
		d.required.push_back("script");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "edit_editor";
		d.description = "把完整内容写入当前编辑区；不创建文件、不落盘，用户检查后自行保存或运行。"
			"用户要求写/改脚本时优先使用它。";
		d.properties.Set("content", arg("string", "完整的新脚本内容（AutoHotkey v2 语法）"));
		d.required.push_back("content");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "run_editor";
		d.description = "运行当前编辑区内容（无需保存文件），返回会话、退出码和输出；"
			"用于自测和按错误迭代修复。";
		d.properties.Set("timeout_ms", arg("integer", "最多等待毫秒数，默认 8000，上限 20000"));
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "run_script";
		d.description = "在 AHKDeck 宿主内运行脚本并返回会话、退出码与输出（JSONL）。";
		d.properties.Set("script", arg("string", "脚本 id"));
		d.properties.Set("timeout_ms", arg("integer", "最多等待毫秒数，默认 8000，上限 20000"));
		d.required.push_back("script");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "get_session_output";
		d.description = "读取指定会话已经产生的错误/警告输出。";
		d.properties.Set("session", arg("integer", "会话编号（来自 run_script 或界面）"));
		d.required.push_back("session");
		defs.push_back(std::move(d));
	}
	{
		ToolDef d;
		d.name = "stop_script";
		d.description = "停止一个正在运行的会话。";
		d.properties.Set("session", arg("integer", "会话编号"));
		d.required.push_back("session");
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

std::string Toolbox::ToolEditEditor(const Json &aArgs)
{
	if (!aArgs["content"].IsString())
		return ErrorResult("缺少 content 字符串").Dump();
	const std::string content = aArgs["content"].AsString();
	if (content.size() > 1024 * 1024)
		return ErrorResult("脚本超过 1 MB，拒绝写入编辑区").Dump();

	if (mEditorSink)
		mEditorSink(content, "edit_editor (" + std::to_string(content.size()) + " B，未保存)");
	else
		return ErrorResult("当前界面没有可写入的编辑区").Dump();

	mPendingEditor = content;
	// Give the window a beat to actually show it: a caller that follows with
	// run_editor must not read last tick's buffer. Bounded, because a modal
	// dialog or an open menu can keep the UI from draining its queue at all -
	// in that case EditorText() keeps serving mPendingEditor and says so later.
	for (int i = 0; i < 40 && LiveEditorText() != content; ++i)
		Sleep(50);

	Json result = OkResult();
	result.Set("bytes", Json((double)content.size()));
	result.Set("saved", Json(false));
	result.Set("message", Json("已写入当前编辑区，尚未保存；请用户检查后保存或运行"));
	// m065: the built-in loop's "edit implies run, failure implies forced re-edit"
	// policy (m059/m060) is gone with it. An MCP client calls run_editor itself;
	// the tool never runs something the caller did not ask for.
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
	std::string backupDisplay;
	if (existed)
	{
		TextDocument old;
		std::string readError;
		if (ReadTextDocument(target, old, readError))
		{
			encoding = old.encoding;
			crlf = old.crlf;
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

	const std::wstring dir = ExeDirectoryForTools() + L"\\.temp";
	CreateDirectoryW(dir.c_str(), nullptr);
	const std::wstring path = dir + L"\\ai-editor.ahk";
	std::size_t written = 0;
	std::string error;
	if (!WriteTextDocument(path, text, TextEncoding::Utf8NoBom, true, &written, error))
		return ErrorResult("写入临时脚本失败: " + error).Dump();

	std::string startError;
	const DWORD sessionId = engine.StartSession(path, std::vector<std::wstring>(), startError);
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
				output = RedactPaths(Tail(document.utf8, 32768));
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
	result.Set("output", Json(output));

	Note("run_editor " + std::string(finished ? "rc=" + std::to_string((unsigned long)rc)
		: "仍在运行 (session " + std::to_string((unsigned long)sessionId) + ")"));
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

	const int timeoutMs = ClampTimeout(aArgs, 8000);
	std::string startError;
	const DWORD sessionId = engine.StartSession(path, std::vector<std::wstring>(), startError);
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
				output = RedactPaths(Tail(document.utf8, 32768));
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
	result.Set("output", Json(output));

	Note("run_script " + display + " " + (finished ? "rc=" + std::to_string((unsigned long)rc)
		: "仍在运行 (session " + std::to_string((unsigned long)sessionId) + ")"));
	return result.Dump();
}

std::string Toolbox::ToolGetSessionOutput(const Json &aArgs)
{
	const int sessionId = aArgs["session"].AsInt(0);
	if (sessionId <= 0)
		return ErrorResult("缺少 session 编号").Dump();

	Engine &engine = Engine::Instance();
	Session session;
	if (!engine.GetSession((DWORD)sessionId, session))
		return ErrorResult("没有这个会话").Dump();

	const std::wstring root = engine.LogRoot();
	const size_t slash = session.logPath.find_last_of("\\/");
	const std::string leaf = slash == std::wstring::npos
		? session.logPath : session.logPath.substr(slash + 1);

	std::string output;
	if (!root.empty() && !leaf.empty())
	{
		TextDocument document;
		std::string error;
		if (ReadTextDocument(root + L"\\" + WideOf(leaf), document, error))
			output = RedactPaths(Tail(document.utf8, 32768));
	}

	Json result = OkResult();
	result.Set("session", Json((double)sessionId));
	result.Set("state", Json(Engine::StateName(session.state)));
	result.Set("rc", Json((double)session.rc));
	result.Set("fatal", Json(session.fatal));
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

} // namespace agent
} // namespace ahkdeck

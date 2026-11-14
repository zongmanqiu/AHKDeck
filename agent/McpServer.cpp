#include "agent/McpServer.h"

#include "core/Version.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ahkdeck
{
namespace agent
{
namespace mcp
{
namespace
{

// What we will answer `initialize` with. Anything newer than the newest entry
// is answered with our newest known version (the client decides to disconnect,
// not us).
const char *const kSupportedVersions[] = {
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};

constexpr size_t kMaxLine = 8u * 1024u * 1024u; // one frame; a bigger line is a bug or an attack

std::mutex gStdoutMutex;

void WriteFrame(const std::string &aText)
{
	std::lock_guard<std::mutex> lock(gStdoutMutex);
	const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out == nullptr || out == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(out, aText.c_str(), (DWORD)aText.size(), &written, nullptr);
}

void WriteErrorLog(const std::function<void(const std::string &)> &aLog, const std::string &aText)
{
	// stderr, UTF-8 bytes, one line - never stdout, which is the protocol.
	const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err != nullptr && err != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(err, aText.c_str(), (DWORD)aText.size(), &written, nullptr);
	}
	if (aLog)
		aLog(aText);
}

Json IdOf(const Json &aRequest)
{
	const Json &id = aRequest["id"];
	return id.IsNull() ? Json() : id;
}

std::string ResultFrame(const Json &aId, Json aResult)
{
	Json frame = Json::MakeObject();
	frame.Set("jsonrpc", Json("2.0"));
	frame.Set("id", aId);
	frame.Set("result", std::move(aResult));
	return frame.Dump() + "\n";
}

std::string ErrorFrame(const Json &aId, int aCode, const std::string &aMessage)
{
	Json error = Json::MakeObject();
	error.Set("code", Json((double)aCode));
	error.Set("message", Json(aMessage));
	Json frame = Json::MakeObject();
	frame.Set("jsonrpc", Json("2.0"));
	frame.Set("id", aId);
	frame.Set("error", std::move(error));
	return frame.Dump() + "\n";
}

} // namespace

Json McpToolArray(const std::vector<ToolDef> &aDefs)
{
	Json tools = Json::MakeArray();
	for (const auto &def : aDefs)
	{
		Json schema = Json::MakeObject();
		schema.Set("type", Json("object"));
		schema.Set("properties", def.properties);
		Json required = Json::MakeArray();
		for (const auto &name : def.required)
			required.Push(Json(name));
		schema.Set("required", std::move(required));

		Json tool = Json::MakeObject();
		tool.Set("name", Json(def.name));
		tool.Set("description", Json(def.description));
		tool.Set("inputSchema", std::move(schema));
		tools.Push(std::move(tool));
	}
	return tools;
}

namespace
{

// m081/m083: the reader/executor split.
//
// The endpoint has to answer two different clocks at once. A request frame must
// be executed strictly one at a time (Toolbox::Call serializes on its own
// mutex, and the tool contract depends on that), but a `notifications/cancelled`
// that arrives *while* a long `run_editor` is in flight has to be acted on
// immediately - otherwise the client can never stop a call it lost patience
// with. A single thread cannot do both: while it sits inside `tools/call` it is
// not reading stdin, so the cancel line stays in the pipe and the run loop
// waits out its whole timeout.
//
// So: the reader thread owns stdin and nothing else. It splits frames, hands
// request frames to the executor through a queue, and applies notifications on
// the spot. The executor thread runs on this function's own thread and stays
// exactly as serial as it was before.
class FrameQueue
{
public:
	void Push(std::string aLine)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mFrames.push_back(std::move(aLine));
		}
		mCv.notify_one();
	}

	bool Pop(std::string &aLine, const std::atomic<bool> &aRunning)
	{
		std::unique_lock<std::mutex> lock(mMutex);
		mCv.wait_for(lock, std::chrono::milliseconds(20), [&] {
			return !mFrames.empty() || mReaderDone || !aRunning.load();
		});
		if (!aRunning.load())
			return false; // shutting down: never start another tool call
		if (mFrames.empty())
			return false;
		aLine = std::move(mFrames.front());
		mFrames.pop_front();
		return true;
	}

	void MarkReaderDone()
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mReaderDone = true;
		}
		mCv.notify_all();
	}

	bool ReaderDone()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mReaderDone;
	}

private:
	std::mutex mMutex;
	std::condition_variable mCv;
	std::deque<std::string> mFrames;
	bool mReaderDone = false;
};

} // namespace

void ServeStdio(Toolbox &aToolbox, const std::atomic<bool> &aRunning,
	const std::function<void(const std::string &)> &aLog)
{
	const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	if (in == nullptr || in == INVALID_HANDLE_VALUE)
	{
		WriteErrorLog(aLog, "mcp: no stdin handle, MCP endpoint not started\n");
		return;
	}

	WriteErrorLog(aLog, "mcp: AHKDeck MCP endpoint ready on stdio\n");

	FrameQueue queue;

	// The reader thread: stdin in, request frames out, notifications applied
	// here and now.
	std::thread reader([&] {
		std::string pending;
		// B1: an oversize frame must be dropped as a whole. Clearing the buffer
		// without a dropping state leaves the rest of that frame behind, and the
		// leftovers are parsed as frames of their own - after which every frame
		// boundary in the session is wrong (a burst of -32700 until reconnect).
		bool discarding = false;
		char chunk[65536];
		while (aRunning.load())
		{
			DWORD available = 0;
			if (PeekNamedPipe(in, nullptr, 0, nullptr, &available, nullptr) && available == 0)
			{
				// Poll instead of blocking so aRunning stays authoritative; the deck
				// must be able to close this endpoint when the window exits.
				Sleep(20);
				continue;
			}

			DWORD got = 0;
			if (!ReadFile(in, chunk, sizeof(chunk), &got, nullptr) || got == 0)
			{
				WriteErrorLog(aLog, "mcp: client closed stdin, endpoint stopped\n");
				break;
			}
			pending.append(chunk, got);

			for (;;)
			{
				const size_t newline = pending.find('\n');
				if (newline == std::string::npos)
				{
					if (!discarding && pending.size() > kMaxLine)
					{
						discarding = true;
						WriteErrorLog(aLog, "mcp: oversize frame dropped\n");
					}
					if (discarding)
						pending.clear();
					break;
				}
				std::string line = pending.substr(0, newline);
				pending.erase(0, newline + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (discarding)
				{
					// Swallow the rest of the dropped frame, up to its newline.
					discarding = false;
					continue;
				}
				if (line.empty())
					continue;

				Json request;
				std::string parseError;
				if (!Json::Parse(line, request, parseError))
				{
					// The executor answers -32700 so the error keeps its place in
					// the response order.
					queue.Push(std::move(line));
					continue;
				}
				if (!request.Has("id"))
				{
					// A notification has no answer, and it may arrive while the
					// executor is busy inside a tool call - that is exactly the
					// case notifications/cancelled exists for. Handle it here.
					if (request["method"].AsString() == "notifications/cancelled")
						aToolbox.Cancel();
					continue;
				}
				queue.Push(std::move(line));
			}
		}
		queue.MarkReaderDone();
	});

	// The executor thread: this one. Requests run one at a time, in order.
	std::string line;
	while (true)
	{
		if (!queue.Pop(line, aRunning))
		{
			if (!aRunning.load() || queue.ReaderDone())
				break;
			continue;
		}

		Json request;
		std::string parseError;
		if (!Json::Parse(line, request, parseError))
		{
			WriteFrame(ErrorFrame(Json(), -32700, "JSON 解析失败: " + parseError));
			continue;
		}
		const std::string method = request["method"].AsString();
		const Json id = IdOf(request);
		if (method.empty())
		{
			// A response to something we sent (we send none) - ignore quietly.
			continue;
		}

		if (method == "initialize")
		{
			const std::string requested = request["params"]["protocolVersion"].AsString();
			std::string version = kSupportedVersions[0];
			for (const char *known : kSupportedVersions)
			{
				if (requested == known)
				{
					version = requested;
					break;
				}
			}
			Json capabilities = Json::MakeObject();
			capabilities.Set("tools", Json::MakeObject());
			Json serverInfo = Json::MakeObject();
			serverInfo.Set("name", Json("ahkdeck"));
			serverInfo.Set("title", Json("AHKDeck - AutoHotkey v2 script deck"));
			serverInfo.Set("version", Json(VersionString()));
			Json result = Json::MakeObject();
			result.Set("protocolVersion", Json(version));
			result.Set("capabilities", std::move(capabilities));
			result.Set("serverInfo", std::move(serverInfo));
			// m159: instructions is a server-level InitializeResult field
			// (MCP 2024-11-05+; all three versions we accept are newer). The
			// write -> check -> validate -> run -> read -> fix loop used to be
			// only a hint inside one tool description; here every client hands
			// it to the model before the first tool call, so the loop no longer
			// depends on the prompt or on the model remembering to check.
			result.Set("instructions", Json(
				"AHKDeck 脚本工具按固定闭环使用："
				"1) 先 list_scripts 取脚本 id，按 id 操作的工具只接受这里返回的 id。"
				"2) 写/改脚本用 edit_editor（只改编辑区、不落盘）或 patch_editor（小改）。"
				"3) 运行前先 check_editor 快速排查，但它的 error 只是文本启发式、可能误报，"
				"不要据此改代码；语法能否通过一律以 validate_editor 为准"
				"（内核 /validate 只解析不执行，ok=true 才可运行）。"
				"4) 运行用 run_editor（编辑区）或 run_script（磁盘脚本）；"
				"verdict 为 pass/fail/resident/timeout 之一，errors 是结构化的内核原话："
				"line（行号）、source（该行原文）、message（为什么错）、func（出错时在哪个函数）、"
				"stack（调用链，顶层在前），warnings 是 #Warn 类告警；"
				"next 字段直接给出下一步该调哪个工具。"
				"5) 失败时看 errors[0].source 就是那一行代码，按 errors[].line 用 patch_editor 修，"
				"再运行复验，直到 verdict=pass。同一行出现在不同位置时用 stack 区分是热键、"
				"定时器还是 GUI 回调进来的。"
				"verdict=resident 表示脚本常驻：用 get_session_output 读输出，需要结束时 stop_script。"
				"6) 界面类脚本跑起来之后，inspect_ui 能按会话量出顶层窗口与全部子控件的类名、文字、"
				"客户区坐标（与开发指南 §10 坐标表同口径）、样式位与 DPI，ui_pixel 取单点颜色；"
				"核对布局是否落在源码写的 x,y,w,h 时用它，比截图可靠。"
				"7) 不确定某个 AHK 函数的签名、参数顺序或参数含义时先 lookup_help 查——"
				"它查的是随包的 2.0.30 官方帮助索引，和你正在跑的内核同版本。"
				"凭记忆写 API 会写出语法合法但语义错的代码（参数位置颠倒、选项名拼错、"
				"把格式串当初始值），validate_editor 与 run_editor 都拦不住这类问题。"
				"工具输出只给脚本 id 与文件名，不含本机绝对路径。"));
			WriteErrorLog(aLog, "mcp: initialize from protocolVersion=" + requested + "\n");
			WriteFrame(ResultFrame(id, std::move(result)));
			continue;
		}

		if (method == "tools/list")
		{
			Json result = Json::MakeObject();
			result.Set("tools", McpToolArray(aToolbox.Definitions()));
			WriteFrame(ResultFrame(id, std::move(result)));
			continue;
		}

		if (method == "tools/call")
		{
			const Json &params = request["params"];
			const std::string name = params["name"].AsString();
			if (name.empty())
			{
				WriteFrame(ErrorFrame(id, -32602, "tools/call 缺少 params.name"));
				continue;
			}
			const Json &arguments = params["arguments"];
			const std::string argumentsJson = arguments.IsObject() ? arguments.Dump() : std::string();

			const std::string toolResult = aToolbox.Call(name, argumentsJson);

			Json content = Json::MakeArray();
			Json block = Json::MakeObject();
			block.Set("type", Json("text"));
			block.Set("text", Json(toolResult));
			content.Push(std::move(block));

			Json result = Json::MakeObject();
			result.Set("content", std::move(content));
			Json parsed;
			std::string ignored;
			const bool failed = Json::Parse(toolResult, parsed, ignored)
				? !parsed["ok"].AsBool(true)
				: true;
			result.Set("isError", Json(failed));

			WriteErrorLog(aLog, "mcp: tools/call " + name + (failed ? " -> error\n" : " -> ok\n"));
			WriteFrame(ResultFrame(id, std::move(result)));
			continue;
		}

		if (method == "ping")
		{
			Json result = Json::MakeObject();
			WriteFrame(ResultFrame(id, std::move(result)));
			continue;
		}

		WriteFrame(ErrorFrame(id, -32601, "未知方法: " + method));
	}

	// The reader may be sitting in PeekNamedPipe/ReadFile; it polls aRunning
	// every 20 ms, so it leaves on its own. Joining it here is what lets the
	// deck free the window instead of exiting the process (m083/B3).
	reader.join();
	WriteErrorLog(aLog, "mcp: endpoint stopped\n");
}

} // namespace mcp
} // namespace agent
} // namespace ahkdeck

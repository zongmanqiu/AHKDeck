#include "agent/McpServer.h"

#include "core/Version.h"

#include <windows.h>

#include <cstring>
#include <mutex>
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

	std::string pending;
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
			return;
		}
		pending.append(chunk, got);

		for (;;)
		{
			const size_t newline = pending.find('\n');
			if (newline == std::string::npos)
			{
				if (pending.size() > kMaxLine)
				{
					WriteErrorLog(aLog, "mcp: oversize frame dropped\n");
					pending.clear();
				}
				break;
			}
			std::string line = pending.substr(0, newline);
			pending.erase(0, newline + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;

			Json request;
			std::string parseError;
			if (!Json::Parse(line, request, parseError))
			{
				WriteFrame(ErrorFrame(Json(), -32700, "JSON 解析失败: " + parseError));
				continue;
			}
			const std::string method = request["method"].AsString();
			const Json id = IdOf(request);
			const bool notification = !request.Has("id");
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
				WriteErrorLog(aLog, "mcp: initialize from protocolVersion=" + requested + "\n");
				if (!notification)
					WriteFrame(ResultFrame(id, std::move(result)));
				continue;
			}

			if (notification || method == "ping")
			{
				if (notification)
					continue; // initialized / cancelled / progress: nothing to answer
				Json result = Json::MakeObject();
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

			WriteFrame(ErrorFrame(id, -32601, "未知方法: " + method));
		}
	}
}

} // namespace mcp
} // namespace agent
} // namespace ahkdeck

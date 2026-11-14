// agent/Toolbox - the deck's script tools, owned by nobody in particular (m063).
//
// Before m063 the tool implementations lived inside ai/DeckAgent, which is the
// OpenAI-compatible conversation loop. That made the loop the only possible
// caller, and any second consumer (the MCP endpoint in m064, a future gate that
// drives tools directly without a model) would have had to copy the tool list -
// two lists describing one fact is exactly the drift we keep getting caught by.
//
// So: the tools and their descriptions live here, once. Transports render them
// into their own dialect (OpenAI `function.parameters` vs MCP `inputSchema`) and
// decide their own retry policy; neither may add a tool of its own.
//
// Threading: Call() is serialized (a running script can block a tool for
// seconds), and it never touches FLTK. Everything the window must do - apply
// editor content, print a console line - goes out through the sinks, which the
// UI wires to a queue it drains on its own thread.
//
// Security: still core/AiPolicy for every path, still ids-only towards the
// model (rule 9: no machine path ever leaves this process).

#pragma once

#include "core/AiPolicy.h"
#include "core/Json.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ahkdeck
{
namespace agent
{

// One tool, described once. `properties` is a JSON object of name -> type
// description; `required` lists the mandatory argument names.
struct ToolDef
{
	std::string name;
	std::string description;
	Json properties = Json::MakeObject();
	std::vector<std::string> required;
};

class Toolbox
{
public:
	// The editor buffer the window is showing, as UTF-8. Supplied by the UI so
	// run_editor/check tools see exactly what the user sees.
	using EditorSupplier = std::function<std::string()>;
	// m062: the file backing the editing area, empty for an unnamed buffer.
	// run_editor needs it so A_ScriptDir and relative #Include match what the
	// user gets after saving.
	using EditorPathSupplier = std::function<std::wstring()>;
	// (content, note) - apply a whole new script to the editing area, unsaved.
	using EditorSink = std::function<void(const std::string &, const std::string &)>;
	// A line for the deck's console panel: what the caller did, in its own words.
	using NoteSink = std::function<void(const std::string &)>;
	// A script file that just changed on disk (UTF-8 canonical path), so the
	// window can add it to the library and reload it if the user has it open.
	using WriteSink = std::function<void(const std::string &)>;

	void SetPolicy(const AiScriptPolicy &aPolicy);
	void SetEditorSupplier(EditorSupplier aSupplier) { mEditor = std::move(aSupplier); }
	void SetEditorPathSupplier(EditorPathSupplier aSupplier) { mEditorPath = std::move(aSupplier); }
	void SetEditorSink(EditorSink aSink) { mEditorSink = std::move(aSink); }
	void SetNoteSink(NoteSink aSink) { mNoteSink = std::move(aSink); }
	void SetWriteSink(WriteSink aSink) { mWriteSink = std::move(aSink); }

	// Cooperative cancel, checked inside the run/wait loops.
	void Cancel() { mCancel = true; }
	bool Cancelled() const { return mCancel.load(); }

	// Enumerates the script area; the conversation loop calls this at the start
	// of a turn so ids stay stable while it works.
	void RefreshScripts();

	std::vector<ToolDef> Definitions() const;
	// OpenAI Chat Completions dialect: [{"type":"function","function":{...}}].
	Json OpenAiSchema() const;

	// Executes one tool. aArgumentsJson is the raw argument object the caller
	// sent; the return value is always a JSON object string ({"ok":false,...}
	// with a reason on every failure), never an exception.
	std::string Call(const std::string &aName, const std::string &aArgumentsJson);

private:
	struct ScriptEntry
	{
		std::string id;
		std::wstring path;
		std::string display;
	};

	int FindScriptById(const std::string &aId) const; // mScriptsMutex held
	std::string NewScriptId();
	void Note(const std::string &aText) const;
	// What a run tool should execute: the live editor text, except in the window
	// between edit_editor and the UI's next tick - there the text we just handed
	// over wins, or a follow-up run_editor would run last tick's script.
	std::string EditorText();
	std::string LiveEditorText() const;
	// Hands a whole script to the editing area and waits (bounded) until the
	// window shows it. False when the UI has no editor to write to.
	bool ApplyEditor(const std::string &aContent, const std::string &aNote);

	std::string ToolListScripts();
	std::string ToolReadScript(const Json &aArgs);
	std::string ToolEditEditor(const Json &aArgs);
	std::string ToolCheckEditor();
	std::string ToolValidateEditor();
	std::string ToolPatchEditor(const Json &aArgs);
	std::string ToolWriteScript(const Json &aArgs);
	std::string ToolRunEditor(const Json &aArgs);
	std::string ToolRunScript(const Json &aArgs);
	std::string ToolGetSessionOutput(const Json &aArgs);
	std::string ToolStopScript(const Json &aArgs);
	std::string ToolListSessions();
	std::string ToolStopAll();
	// m174: the session's own window tree, as measurement data. Read-only.
	std::string ToolInspectUi(const Json &aArgs);
	// m187: the same window, as pixels instead of coordinates.
	std::string ToolCaptureUi(const Json &aArgs);
	std::string ToolPixel(const Json &aArgs);
	// m177: the offline AutoHotkey help index, so a caller can confirm a
	// signature instead of recalling one.
	std::string ToolLookupHelp(const Json &aArgs);
	// Parses docs\help-index.json on first use. Lazy on purpose: the file is
	// ~280 KB and most sessions never ask for it, so no launch should pay for
	// it. A failure is remembered in mHelpError rather than thrown, because the
	// caller's next move is "quote the reason", not "crash".
	void EnsureHelpIndexLoaded();

	AiScriptPolicy mPolicy;
	EditorSupplier mEditor;
	EditorPathSupplier mEditorPath;
	EditorSink mEditorSink;
	NoteSink mNoteSink;
	WriteSink mWriteSink;
	std::string mPendingEditor; // guarded by mCallMutex
	std::atomic<bool> mCancel{false};

	// The id table is shared by every caller, so it is guarded separately from
	// mCallMutex (list_scripts must not block behind an 8-second run).
	mutable std::mutex mScriptsMutex;
	std::vector<ScriptEntry> mScripts;
	int mNextScriptId = 1;

	std::mutex mCallMutex; // one tool at a time, for every transport

	// m177: guarded by its own lock so a slow help query cannot sit behind an
	// 8-second script run.
	mutable std::mutex mHelpMutex;
	bool mHelpLoaded = false;
	Json mHelpEntries;
	std::string mHelpError;
};

} // namespace agent
} // namespace ahkdeck

// core/ExeCompiler.h - m172: compile one script to a standalone .exe via the
// OFFICIAL Ahk2Exe compiler, shipped inside the release under tools\.
//
// The deck owns no compiler of its own. Everything here is a thin, honest
// wrapper around the official exe:
//
//   tools\Ahk2Exe.exe  /in <script>  /out <exe>  /base <AutoHotkey64.exe>  /silent
//
// Semantics measured against the shipped pair (m172 probes, do not "improve"
// these from the docs alone):
//   * rc == 0            success; the success line lands on STDOUT;
//   * rc == 17           the script has a syntax error - Ahk2Exe validates it
//                        through its base first; stderr carries the kernel's own
//                        "==> ..." line;
//   * rc == 50           an #Include file cannot be opened; stderr explains it;
//   * /silent            never shows a dialog, which is exactly why we use it;
//   * the base exe MUST sit in the compiler's own directory: Ahk2Exe also
//                        searches there for a matching-version interpreter to
//                        use as a utility while compiling (docs: "Background
//                        Information"), so tools\ is not splittable and the
//                        /base path is still passed explicitly anyway.
//
// It runs as a SHORT-LIVED CHILD PROCESS on purpose. The in-process kernel can
// load a source file only once while its life (DeckEngine.h m103), and the
// compiler is a separate exe image with its own quirks - neither belongs inside
// this process. Output goes through temp files, not pipes: same deadlock
// argument as validate (Toolbox.cpp m103).
//
// Synchronous and blocking - call from a worker thread, never the UI thread.
// FLTK-free.
#pragma once

#include <string>

namespace ahkdeck
{

// Compiler tool locations under the exe directory. True when BOTH files are
// present; `aError` carries a user-readable Chinese sentence naming what is
// missing otherwise (fail closed - a half-populated tools\ must not surprise a
// user mid-compile).
bool ResolveCompileTools(const std::wstring &aExeDir, std::wstring &aCompilerOut,
	std::wstring &aBaseOut, std::string &aError);

// "<dir>\name.ahk" -> "<dir>\name.exe". Case-insensitive ".ahk" suffix is
// replaced; anything else gets ".exe" appended to the whole name (the deck
// only ever feeds it scanned .ahk paths, but the function must not corrupt a
// surprising one into a directory escape).
std::wstring DefaultExePathFor(const std::wstring &aScriptPath);

// The compiler's full command line (quoted, /silent throughout). Pure, so the
// quoting of Chinese and spaced paths is pinned by a unit test instead of by a
// GUI click.
std::wstring BuildCompileCommandLine(const std::wstring &aCompilerPath,
	const std::wstring &aScriptPath, const std::wstring &aOutPath,
	const std::wstring &aBasePath);

struct ExeCompileOutcome
{
	int rc = -1;           // the compiler's own exit code; -1 = never ran
	std::string output;    // UTF-8: stdout+stderr of the compiler, verbatim
	std::wstring outPath;  // where the .exe should be by the time rc == 0
	bool produced = false; // rc == 0 AND the file is on disk (belt: rc lies if
	                       // the process was killed mid-way, so check both)
	bool backedUp = false; // TRUE only when THIS call renamed a previous
	                       // target to ".old" - stale .old files from an
	                       // earlier round must not make the console lie
	std::string error;     // deck-side failure (spawn/wait/file), not the
	                       // compiler's - the compiler's voice is in `output`
};

// Compiles one script. The call blocks until the child exits or `aTimeoutMs`
// elapses (then it is killed and the outcome says so). Existing target is
// preserved first: renamed to "<target>.old", replacing any earlier .old -
// the same contract as the downloader's overwrite path (m160), so a recompile
// can never silently destroy a artifact the user kept.
// Intermediate capture files live under <exeDir>\.temp and are deleted.
ExeCompileOutcome CompileScriptToExe(const std::wstring &aExeDir,
	const std::wstring &aScriptPath, const std::wstring &aOutPath, int aTimeoutMs = 60000);

} // namespace ahkdeck

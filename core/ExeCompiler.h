// core/ExeCompiler.h - m172, rewritten in m188.
//
// Compiles one script to a standalone .exe. m188 dropped the official
// Ahk2Exe.exe (995 KB, because it is itself a compiled AutoHotkey v1.1 script
// carrying a whole previous-generation runtime) in favour of the job it
// actually performs:
//
//   AutoHotkey64.exe  ->  copy  ->  replace RCDATA id 1 with the script
//
// Measured, not assumed (m188 probes): the resulting exe runs, is the same size
// as Ahk2Exe's output (512 bytes of resource directory apart), and needs no
// v1.1 runtime. m192 P4: the base is the single shipped kernel next to
// AHKDeck.exe (also the EXE the editor spawns per session), so tools\ is gone
// and the package carries exactly two PE files.
//
// What went away with Ahk2Exe:
//   * #@Ahk2Exe-* directives - still not parsed (the runtime sees them as plain
//     comments). m189 restored the two carrying real user value through a
//     first-class entry instead: a custom icon and a fresh version block.
//   * console subsystem / MPRESS - not done; the deck never used either.
//   * #Include inlining - the runtime resolves #Include by itself, relative to
//     the exe. Identical for a single-file script (all six bundled ones).
// What was kept: the script is validated in a CHILD process first (m192 P4:
// AHKDeck.exe --validate, which starts its own kernel child), because a live
// interpreter cannot be reused for a load check, and bundling a script that
// cannot load is a trap the user only finds by double-clicking the result.
//
// Synchronous and blocking - call from a worker thread, never the UI thread.
// FLTK-free.
#pragma once

#include <string>

namespace ahkdeck
{

// Tool locations under the exe directory. m188: only the base image is needed.
// `aCompilerOut` stays in the signature for callers that probe the tool set and
// is ALWAYS empty now - a non-empty value there would mean nothing.
// `aError` carries a user-readable Chinese sentence naming what is missing
// (fail closed: a half-populated tools\ must not surprise a user mid-compile).
bool ResolveCompileTools(const std::wstring &aExeDir, std::wstring &aCompilerOut,
	std::wstring &aBaseOut, std::string &aError);

// "<dir>\name.ahk" -> "<dir>\name.exe". Case-insensitive ".ahk" suffix is
// replaced; anything else gets ".exe" appended to the whole name (the deck
// only ever feeds it scanned .ahk paths, but the function must not corrupt a
// surprising one into a directory escape).
std::wstring DefaultExePathFor(const std::wstring &aScriptPath);

struct ExeCompileOutcome
{
	int rc = -1;           // 0 = produced; the validator's code when it refused;
	                       // -1 = never got that far
	std::string output;    // UTF-8: the validator's diagnostics, already turned
	                       // from JSON records into "第 N 行：..." lines
	std::wstring outPath;  // where the .exe should be by the time rc == 0
	bool produced = false; // rc == 0 AND the file is on disk (belt: rc lies if
	                       // the move failed, so check both)
	bool backedUp = false; // TRUE only when THIS call renamed a previous
	                       // target to ".old" - stale .old files from an
	                       // earlier round must not make the console lie
	std::string error;     // deck-side failure (tool check/read/resource/move),
	                       // not the script's - the script's voice is in `output`
};

// m189: the artwork a compiled script carries. Both parts are optional writes
// on the same update handle as the script - no extra tool, no extra pass.
struct ExeCompileOptions
{
	std::wstring iconPath;         // .ico to adopt; empty = keep the base icon
	std::wstring fileVersion;      // "1.2.3.4"; empty = 1.0.0.0
	std::wstring productName;      // empty = the script's file name
	std::wstring fileDescription;  // empty = the script's file name
	std::wstring companyName;      // empty = omitted from the version block
	std::wstring legalCopyright;   // empty = omitted from the version block
};

// What a plain "编译为 EXE" uses: product/description from the script name,
// version 1.0.0.0, and - the convention that makes a custom icon reachable
// without a dialog - a sibling "<script>.ico" when one exists.
ExeCompileOptions DefaultCompileOptionsFor(const std::wstring &aScriptPath);

// Compiles one script. Blocks while the validator runs; `aTimeoutMs` bounds
// that child (the bundling itself is a copy plus resource writes).
// An existing target is preserved first: renamed to "<target>.old", replacing
// any earlier .old - the same contract as the downloader's overwrite path
// (m160), so a recompile can never silently destroy an artifact the user kept.
// The artifact is built under <exeDir>\temp\work and moved into place, so a
// failure cannot leave a half-written exe at the destination.
ExeCompileOutcome CompileScriptToExe(const std::wstring &aExeDir,
	const std::wstring &aScriptPath, const std::wstring &aOutPath, int aTimeoutMs = 60000);

ExeCompileOutcome CompileScriptToExe(const std::wstring &aExeDir,
	const std::wstring &aScriptPath, const std::wstring &aOutPath,
	const ExeCompileOptions &aOptions, int aTimeoutMs = 60000);

} // namespace ahkdeck

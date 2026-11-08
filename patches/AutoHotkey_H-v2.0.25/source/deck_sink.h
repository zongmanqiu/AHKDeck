// deck_sink.h - AHKDeck error sink seam (patched into AHK_H ReleaseDll builds).
//
// Why this exists: the kernel already intercepts every error/warning *before*
// Script::ShowError() via LibNotifyProblem(). That seam is enough to suppress
// the modal IDD_ERRORBOX dialog and hand a fully structured record to the host,
// without ever touching script semantics (A_ScriptDir, line numbers, ErrorLevel
// or thread termination stay exactly as upstream).
//
// The sink is a process-wide C ABI callback (one per loaded kernel DLL). It is
// invoked on the failing script's own thread, and must not block: the host
// implementation only pushes the record into a queue.
//
// Behaviour contract:
//   * No sink installed  -> every call site behaves byte-for-byte like upstream.
//   * Sink installed     -> DeckNotifyError() returns true and the canonical
//                           caller suppresses its dialog exactly as if a
//                           LibNotifyProblem() callback had handled the problem.

#pragma once

#ifdef CONFIG_DLL

class Line; // script.h
typedef void (__stdcall *DeckErrorSinkFn)(const char *aUtf8Json);

// When true, every script thread is treated as a standalone script: its
// filespec from the command line wins over the "secondary thread inherits the
// first script's directory" rule in Script::Init(). Required for A_ScriptDir /
// A_ScriptName / #Include to match the official executable.
extern bool g_DeckIsolatedScripts;

// Builds one UTF-8 JSON line and hands it to the installed sink.
// Returns false (and does nothing) when no sink is installed.
// aWarn distinguishes #Warn-style warnings from errors; aErrorType carries the
// upstream severity (FAIL / CRITICAL_ERROR / FAIL_OR_OK) untouched.
bool DeckNotifyError(LPCTSTR aMessage, LPCTSTR aExtra, Line *aLine, int aErrorType, bool aWarn);

// m061: the second seam. FileAppend's stdout target ("*") has no stdout handle
// in a GUI host, so upstream would try to open a file literally named "*". With
// an output sink installed the kernel hands the text to the host as one JSON
// record instead; without one the file path is untouched and behaviour stays
// byte-for-byte upstream.
typedef void (__stdcall *DeckOutputSinkFn)(const char *aUtf8Json);
bool DeckNotifyOutput(LPCTSTR aText);

#endif // CONFIG_DLL

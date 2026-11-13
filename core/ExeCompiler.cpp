// core/ExeCompiler.cpp - m172. See ExeCompiler.h for the compiler semantics
// these lines are written against (rc 0/17/50, stdout vs stderr, /silent).

#include "core/ExeCompiler.h"

#include "core/TextFile.h"

#include <windows.h>

#include <vector>

namespace ahkdeck
{
namespace
{

constexpr const wchar_t *kCompilerLeaf = L"tools\\Ahk2Exe.exe";
constexpr const wchar_t *kBaseLeaf = L"tools\\AutoHotkey64.exe";

std::string WideToUtf8(const std::wstring &aWide)
{
	const int need = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(),
		static_cast<int>(aWide.size()), nullptr, 0, nullptr, nullptr);
	if (need <= 0)
		return std::string();
	std::string out(static_cast<size_t>(need), '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), static_cast<int>(aWide.size()),
		out.data(), need, nullptr, nullptr);
	return out;
}

bool FileExists(const std::wstring &aPath)
{
	const DWORD attr = GetFileAttributesW(aPath.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring TrimTrailingSlash(std::wstring aPath)
{
	while (!aPath.empty() && (aPath.back() == L'\\' || aPath.back() == L'/'))
		aPath.pop_back();
	return aPath;
}

// Reads a whole file as bytes. The compiler's stdout/stderr are plain text;
// reading bytes avoids any codepage guessing on our side - whatever Ahk2Exe
// printed is what the console shows.
bool ReadRawFile(const std::wstring &aPath, std::string &aOut)
{
	aOut.clear();
	const HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	char chunk[4096];
	DWORD got = 0;
	while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got > 0)
		aOut.append(chunk, got);
	CloseHandle(file);
	return true;
}

} // namespace

bool ResolveCompileTools(const std::wstring &aExeDir, std::wstring &aCompilerOut,
	std::wstring &aBaseOut, std::string &aError)
{
	aCompilerOut.clear();
	aBaseOut.clear();
	aError.clear();

	const std::wstring root = TrimTrailingSlash(aExeDir);
	aCompilerOut = root + L"\\" + kCompilerLeaf;
	aBaseOut = root + L"\\" + kBaseLeaf;

	if (!FileExists(aCompilerOut))
	{
		aError = "找不到编译器 tools\\Ahk2Exe.exe——请用完整安装包安装 AHKDeck";
		aCompilerOut.clear();
		aBaseOut.clear();
		return false;
	}
	if (!FileExists(aBaseOut))
	{
		aError = "找不到编译底座 tools\\AutoHotkey64.exe——请用完整安装包安装 AHKDeck";
		aCompilerOut.clear();
		aBaseOut.clear();
		return false;
	}
	return true;
}

std::wstring DefaultExePathFor(const std::wstring &aScriptPath)
{
	if (aScriptPath.size() >= 4 &&
		_wcsicmp(aScriptPath.c_str() + aScriptPath.size() - 4, L".ahk") == 0)
		return aScriptPath.substr(0, aScriptPath.size() - 4) + L".exe";
	return aScriptPath + L".exe";
}

std::wstring BuildCompileCommandLine(const std::wstring &aCompilerPath,
	const std::wstring &aScriptPath, const std::wstring &aOutPath,
	const std::wstring &aBasePath)
{
	// Every path is quoted; Ahk2Exe documents that "parameters containing
	// spaces must be enclosed in double quotes", and Chinese paths ride on the
	// same rule. No inner quotes are possible in these inputs (a Windows path
	// cannot contain '"'), so a plain wrap is enough.
	std::wstring line;
	line += L"\"" + aCompilerPath + L"\"";
	line += L" /in \"" + aScriptPath + L"\"";
	line += L" /out \"" + aOutPath + L"\"";
	line += L" /base \"" + aBasePath + L"\"";
	line += L" /silent";
	return line;
}

ExeCompileOutcome CompileScriptToExe(const std::wstring &aExeDir,
	const std::wstring &aScriptPath, const std::wstring &aOutPath, int aTimeoutMs)
{
	ExeCompileOutcome outcome;
	outcome.outPath = aOutPath;

	std::wstring compiler;
	std::wstring base;
	if (!ResolveCompileTools(aExeDir, compiler, base, outcome.error))
		return outcome;
	if (!FileExists(aScriptPath))
	{
		outcome.error = "脚本文件不存在或已被移动";
		return outcome;
	}

	const std::wstring root = TrimTrailingSlash(aExeDir);
	const std::wstring tempDir = root + L"\\.temp";
	CreateDirectoryW(tempDir.c_str(), nullptr);

	// One capture file per stream, named against THIS child (pid + tick):
	// two compiles can never hit the same names, and the single-flight guard
	// in the UI already keeps the common case serial.
	const std::wstring tag = std::to_wstring(GetCurrentProcessId()) + L"-" +
		std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
	const std::wstring outLog = tempDir + L"\\compile-" + tag + L".out";
	const std::wstring errLog = tempDir + L"\\compile-" + tag + L".err";

	SECURITY_ATTRIBUTES inheritable = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
	const HANDLE out = CreateFileW(outLog.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	const HANDLE err = CreateFileW(errLog.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE)
	{
		outcome.error = "无法创建编译输出捕获文件";
		if (out != INVALID_HANDLE_VALUE)
			CloseHandle(out);
		if (err != INVALID_HANDLE_VALUE)
			CloseHandle(err);
		DeleteFileW(outLog.c_str());
		DeleteFileW(errLog.c_str());
		return outcome;
	}
	const HANDLE nullIn = CreateFileW(L"NUL", GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

	// Preserve an earlier artifact BEFORE the child runs: the overwrite
	// contract is rename-to-.old (same as the downloader), never truncate.
	// Done up here so a failed/aborted compile can never leave the user with a
	// half-written target and no backup.
	if (FileExists(aOutPath))
	{
		const std::wstring backup = aOutPath + L".old";
		DeleteFileW(backup.c_str());
		if (!MoveFileW(aOutPath.c_str(), backup.c_str()))
		{
			outcome.error = "旧的 " + WideToUtf8(aOutPath) + " 无法备份，编译已中止";
			// restore the handles we opened on nothing useful yet
			CloseHandle(out);
			CloseHandle(err);
			if (nullIn != INVALID_HANDLE_VALUE)
				CloseHandle(nullIn);
			DeleteFileW(outLog.c_str());
			DeleteFileW(errLog.c_str());
			return outcome;
		}
		outcome.backedUp = true; // THIS call renamed a previous target
	}

	const std::wstring command = BuildCompileCommandLine(compiler, aScriptPath, aOutPath, base);
	std::vector<wchar_t> commandLine(command.begin(), command.end());
	commandLine.push_back(0);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = nullIn == INVALID_HANDLE_VALUE ? out : nullIn;
	si.hStdOutput = out;
	si.hStdError = err;

	// Working directory = the compiler's own folder: its interpreter-utility
	// search (docs: Background Information) starts from where it lives.
	const std::wstring compilerDir = compiler.substr(0, compiler.find_last_of(L'\\'));

	PROCESS_INFORMATION pi = {};
	const BOOL started = CreateProcessW(compiler.c_str(), commandLine.data(), nullptr, nullptr,
		TRUE, CREATE_NO_WINDOW, nullptr, compilerDir.c_str(), &si, &pi);
	CloseHandle(out);
	CloseHandle(err);
	if (nullIn != INVALID_HANDLE_VALUE)
		CloseHandle(nullIn);

	if (!started)
	{
		outcome.error = "无法启动编译器（错误码 " + std::to_string(GetLastError()) + "）";
		DeleteFileW(outLog.c_str());
		DeleteFileW(errLog.c_str());
		return outcome;
	}
	CloseHandle(pi.hThread);

	const DWORD waited = WaitForSingleObject(pi.hProcess,
		aTimeoutMs > 0 ? static_cast<DWORD>(aTimeoutMs) : INFINITE);
	if (waited == WAIT_TIMEOUT)
	{
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 5000);
		outcome.error = "编译器超时未退出，已结束";
	}
	else
	{
		DWORD rc = 0;
		if (GetExitCodeProcess(pi.hProcess, &rc))
			outcome.rc = static_cast<int>(rc);
	}
	CloseHandle(pi.hProcess);

	ReadRawFile(outLog, outcome.output);
	std::string errText;
	ReadRawFile(errLog, errText);
	if (!errText.empty())
	{
		if (!outcome.output.empty())
			outcome.output += "\n";
		outcome.output += errText;
	}
	DeleteFileW(outLog.c_str());
	DeleteFileW(errLog.c_str());

	outcome.produced = outcome.rc == 0 && FileExists(aOutPath);
	if (outcome.error.empty() && outcome.rc == 0 && !FileExists(aOutPath))
		outcome.error = "编译器报告成功，但没有产出文件";
	return outcome;
}

} // namespace ahkdeck

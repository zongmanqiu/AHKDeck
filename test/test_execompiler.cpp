// m172: the script -> standalone exe compiler wrapper.
//
// The pure arms (path/name/command-line builders, fail-closed tool check)
// always run - they are the contract, and a contract that never gets exercised
// can rot. The end-to-end arms drive the REAL official compiler only when the
// tools are actually staged (the build copies them into .build\tools from
// .3rd); on a bare checkout with no compiler present they report SKIP instead
// of pretending to pass. A gate that cannot fail is not a gate.

#include "core/ExeCompiler.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace ahkdeck;

namespace
{

int gFailures = 0;

void Check(bool aCondition, const char *aWhat)
{
	if (aCondition)
		std::printf("ok   %s\n", aWhat);
	else
	{
		std::printf("FAIL %s\n", aWhat);
		++gFailures;
	}
}

void Skip(const char *aWhat)
{
	std::printf("skip %s\n", aWhat);
}

bool HasFile(const std::wstring &aPath)
{
	const DWORD attr = GetFileAttributesW(aPath.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void WriteRaw(const std::wstring &aPath, const std::string &aUtf8)
{
	FILE *file = nullptr;
	if (_wfopen_s(&file, aPath.c_str(), L"wb") != 0 || !file)
		return;
	std::fwrite(aUtf8.data(), 1, aUtf8.size(), file);
	std::fclose(file);
}

std::wstring TempDir()
{
	wchar_t dir[MAX_PATH] = { 0 };
	GetTempPathW(MAX_PATH, dir);
	std::wstring out(dir);
	if (!out.empty() && out.back() != L'\\')
		out += L'\\';
	return out + L"ahkdeck_execompiler_gate\\";
}

} // namespace

int main()
{
	// ---- pure arms: always run -------------------------------------------

	Check(DefaultExePathFor(L"C:\\x\\foo.ahk") == L"C:\\x\\foo.exe", "路径转换 .ahk->.exe");
	Check(DefaultExePathFor(L"C:\\x\\FOO.AHK") == L"C:\\x\\FOO.exe", "路径转换大小写不敏感");
	Check(DefaultExePathFor(L"C:\\x\\noext") == L"C:\\x\\noext.exe", "路径转换无扩展名补 .exe");
	Check(DefaultExePathFor(L"bad.ahk.dat") == L"bad.ahk.dat.exe", "路径转换中间带 .ahk 不误伤");

	const std::wstring line = BuildCompileCommandLine(
		L"C:\\a b\\Ahk2Exe.exe", L"C:\\脚本 目录\\x.ahk", L"C:\\out\\x.exe", L"C:\\base\\AHK.exe");
	Check(line.find(L"\"C:\\a b\\Ahk2Exe.exe\"") == 0, "命令行编译器路径加引号在最前");
	Check(line.find(L" /in \"C:\\脚本 目录\\x.ahk\"") != std::wstring::npos, "命令行 /in 中文空格加引号");
	Check(line.find(L" /out \"C:\\out\\x.exe\"") != std::wstring::npos, "命令行 /out 加引号");
	Check(line.find(L" /base \"C:\\base\\AHK.exe\"") != std::wstring::npos, "命令行 /base 加引号");
	// " /silent" is EIGHT characters including the leading space — the first
	// draft counted seven and the nail went red exactly as designed. The
	// length comes from a wide literal's own size(), never from hand counting.
	static const wchar_t kSilent[] = L" /silent";
	const size_t kSilentLen = sizeof(kSilent) / sizeof(kSilent[0]) - 1;
	Check(line.size() >= kSilentLen &&
		line.compare(line.size() - kSilentLen, kSilentLen, kSilent) == 0,
		"命令行以 /silent 收尾");

	// fail-closed tool check, on a directory that cannot hold the pair.
	{
		std::wstring compiler;
		std::wstring base;
		std::string error;
		const bool ok = ResolveCompileTools(L"Z:\\ahkdeck-not-a-real-dir", compiler, base, error);
		Check(!ok && !error.empty() && compiler.empty() && base.empty(),
			"缺工具时 fail-closed（两路径清空并给出说明）");
	}

	// A missing script must be rejected before any process is spawned.
	{
		ExeCompileOutcome outcome = CompileScriptToExe(TempDir(), L"Z:\\nope.ahk",
			L"Z:\\nope.exe", 5000);
		Check(!outcome.error.empty(), "脚本不存在时给出 deck 侧错误（未启动子进程）");
	}

	// ---- end-to-end arms: drive the real compiler when it is present -----

	// Locate the staged tools: prefer .build\tools (argv[1] or the exe dir),
	// fall back to the dev tree under .3rd so a local run still exercises the
	// real path. The exeDir passed to the wrapper is the directory whose
	// "tools\" subfolder holds the pair.
	std::wstring toolsExeDir;
	{
		const wchar_t *argvBuild = L"..\\..\\.build"; // CWD is .temp/<gate> at run time
		wchar_t modulePath[MAX_PATH] = { 0 };
		GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
		std::wstring moduleDir(modulePath);
		const size_t slash = moduleDir.find_last_of(L'\\');
		moduleDir.resize(slash == std::wstring::npos ? 0 : slash);
		// .temp/bin -> ../../.build
		const std::wstring buildDir = moduleDir + L"\\..\\..\\.build";
		if (HasFile(buildDir + L"\\tools\\Ahk2Exe.exe") &&
			HasFile(buildDir + L"\\tools\\AutoHotkey64.exe"))
			toolsExeDir = buildDir;
		else if (HasFile(std::wstring(argvBuild) + L"\\tools\\Ahk2Exe.exe"))
			toolsExeDir = argvBuild;
	}

	if (toolsExeDir.empty())
	{
		Skip("端到端绿臂（未 stage tools\\Ahk2Exe.exe，构建机才会跑）");
		Skip("端到端红臂（同上）");
	}
	else
	{
		const std::wstring dir = TempDir();
		CreateDirectoryW(dir.c_str(), nullptr);
		const std::wstring script = dir + L"good.ahk";
		const std::wstring out = dir + L"good.exe";
		DeleteFileW(out.c_str());
		WriteRaw(script, "#Requires AutoHotkey v2.0\n");

		ExeCompileOutcome ok = CompileScriptToExe(toolsExeDir, script, out, 60000);
		Check(ok.rc == 0 && ok.produced && HasFile(out), "端到端绿臂：正常脚本 rc=0 且产出 exe");
		Check(!ok.backedUp, "端到端绿臂：首次编译不报告备份");

		// Second compile over the same target must preserve it as .old.
		ExeCompileOutcome again = CompileScriptToExe(toolsExeDir, script, out, 60000);
		Check(again.rc == 0 && again.produced && again.backedUp &&
			HasFile(out + L".old"), "端到端绿臂：重复编译旧产物备份为 .old");

		const std::wstring bad = dir + L"bad.ahk";
		const std::wstring badOut = dir + L"bad.exe";
		DeleteFileW(badOut.c_str());
		WriteRaw(bad, "#Include %A_ScriptDir%\\ahkdeck-missing-file-9527.xyz\n");
		ExeCompileOutcome red = CompileScriptToExe(toolsExeDir, bad, badOut, 60000);
		Check(red.rc != 0 && !red.produced && !HasFile(badOut),
			"端到端红臂：坏脚本 rc!=0 且不产出");
		Check(!red.output.empty(), "端到端红臂：编译器原文被捕获回传");

		DeleteFileW(script.c_str());
		DeleteFileW(out.c_str());
		DeleteFileW((out + L".old").c_str());
		DeleteFileW(bad.c_str());
	}

	if (gFailures == 0)
		std::printf("ALL GREEN\n");
	return gFailures;
}

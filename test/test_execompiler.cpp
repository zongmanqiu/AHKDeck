// m172: the script -> standalone exe compiler wrapper.
//
// The pure arms (path/name builders, fail-closed tool check)
// always run - they are the contract, and a contract that never gets exercised
// can rot. The end-to-end arms drive the real bundler only when the ONE shipped
// kernel is staged at .build\AutoHotkey64.exe (m192 P4: CMake copies it from
// .temp\kernel_official). On a bare checkout without it they report SKIP
// instead of pretending to pass. A gate that cannot fail is not a gate.

#include "core/ExeCompiler.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
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

// m189: one resource of a built exe, read as data so no image is mapped.
bool ReadResource(const std::wstring &aPath, WORD aType, WORD aId, std::string &aOut)
{
	aOut.clear();
	HMODULE module = LoadLibraryExW(aPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
	if (!module)
		return false;
	bool ok = false;
	HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(aId), MAKEINTRESOURCEW(aType));
	if (found)
	{
		const DWORD size = SizeofResource(module, found);
		const void *data = LockResource(LoadResource(module, found));
		if (data && size)
		{
			aOut.assign(static_cast<const char *>(data), size);
			ok = true;
		}
	}
	FreeLibrary(module);
	return ok;
}

// Neither the base (159) nor the host (101) keeps its icon group at id 1, so
// the gate discovers it - hardcoding an id is what made m189's first run red.
WORD FindFirstResourceId(const std::wstring &aPath, WORD aType, WORD aMax)
{
	HMODULE module = LoadLibraryExW(aPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
	if (!module)
		return 0;
	WORD found = 0;
	for (WORD id = 1; id < aMax; ++id)
		if (FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(aType)))
		{
			found = id;
			break;
		}
	FreeLibrary(module);
	return found;
}

unsigned ReadU32(const std::string &aBytes, size_t aAt)
{
	if (aAt + 4 > aBytes.size())
		return 0;
	unsigned value = 0;
	memcpy(&value, aBytes.data() + aAt, sizeof(value));
	return value;
}

// The version block stores strings as UTF-16, so a plain char search misses it.
bool ContainsWide(const std::string &aHaystack, const std::wstring &aNeedle)
{
	std::string bytes(aNeedle.size() * sizeof(wchar_t), '\0');
	memcpy(bytes.data(), aNeedle.c_str(), bytes.size());
	return aHaystack.find(bytes) != std::string::npos;
}

// A minimal but real .ico: 16x16, 32bpp, one frame, opaque AND mask. Generated
// here so the gate carries no binary fixture and cannot drift from the parser.
std::string MakeTestIcon()
{
	const int side = 16;
	const int maskRowBytes = ((side + 31) / 32) * 4;
	const int maskBytes = maskRowBytes * side;
	const int pixelBytes = side * side * 4;
	const int imageBytes = 40 + pixelBytes + maskBytes;
	std::string ico;
	auto put16 = [&ico](unsigned v) {
		ico.push_back(static_cast<char>(v & 0xFF));
		ico.push_back(static_cast<char>((v >> 8) & 0xFF));
	};
	auto put32 = [&put16](unsigned v) {
		put16(v & 0xFFFF);
		put16((v >> 16) & 0xFFFF);
	};
	put16(0);
	put16(1);
	put16(1);                       // reserved, type=icon, one frame
	ico.push_back(static_cast<char>(side));
	ico.push_back(static_cast<char>(side));
	ico.push_back(0);               // colours
	ico.push_back(0);               // reserved
	put16(1);                       // planes
	put16(32);                      // bit count
	put32(static_cast<unsigned>(imageBytes));
	put32(22);                      // image offset = header + one entry
	put32(40);                      // BITMAPINFOHEADER
	put32(static_cast<unsigned>(side));
	put32(static_cast<unsigned>(side * 2)); // XOR + AND mask
	put16(1);
	put16(32);
	put32(0);                       // BI_RGB
	put32(static_cast<unsigned>(pixelBytes + maskBytes));
	put32(0);
	put32(0);
	put32(0);
	put32(0);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x)
		{
			ico.push_back(static_cast<char>(x * 16)); // B
			ico.push_back(static_cast<char>(y * 16)); // G
			ico.push_back(static_cast<char>(200));    // R
			ico.push_back(static_cast<char>(255));    // A
		}
	ico.append(static_cast<size_t>(maskBytes), '\0');
	return ico;
}

} // namespace

int main()
{
	// ---- pure arms: always run -------------------------------------------

	Check(DefaultExePathFor(L"C:\\x\\foo.ahk") == L"C:\\x\\foo.exe", "路径转换 .ahk->.exe");
	Check(DefaultExePathFor(L"C:\\x\\FOO.AHK") == L"C:\\x\\FOO.exe", "路径转换大小写不敏感");
	Check(DefaultExePathFor(L"C:\\x\\noext") == L"C:\\x\\noext.exe", "路径转换无扩展名补 .exe");
	Check(DefaultExePathFor(L"bad.ahk.dat") == L"bad.ahk.dat.exe", "路径转换中间带 .ahk 不误伤");


	// fail-closed tool check, on a directory that cannot hold the pair.
	{
		std::wstring compiler;
		std::wstring base;
		std::string error;
		const bool ok = ResolveCompileTools(L"Z:\\ahkdeck-not-a-real-dir", compiler, base, error);
		Check(!ok && !error.empty() && compiler.empty() && base.empty(),
			"缺工具时 fail-closed（两路径清空并给出说明）");
	}

	// m189: the icon convention is a sibling file, so it is pure logic and can
	// be asserted without building anything.
	{
		const std::wstring dir = TempDir();
		CreateDirectoryW(dir.c_str(), nullptr);
		const std::wstring bare = dir + L"noicon.ahk";
		DeleteFileW((dir + L"noicon.ico").c_str());
		Check(DefaultCompileOptionsFor(bare).iconPath.empty(),
			"图标约定：没有同名 .ico 时不带图标");
		WriteRaw(dir + L"withicon.ico", "x");
		Check(DefaultCompileOptionsFor(dir + L"withicon.ahk").iconPath
			== dir + L"withicon.ico", "图标约定：同名 .ico 被自动采用");
		Check(DefaultCompileOptionsFor(bare).fileVersion == L"1.0.0.0",
			"版本默认值：1.0.0.0");
		DeleteFileW((dir + L"withicon.ico").c_str());
	}

	// A missing script must be rejected before any process is spawned.
	{
		ExeCompileOutcome outcome = CompileScriptToExe(TempDir(), L"Z:\\nope.ahk",
			L"Z:\\nope.exe", 5000);
		Check(!outcome.error.empty(), "脚本不存在时给出 deck 侧错误（未启动子进程）");
	}

	// ---- end-to-end arms: drive the real compiler when it is present -----

	// m192 P4: locate the ONE shipped kernel. It sits next to AHKDeck.exe (not
	// under tools\ any more), and ResolveCompileTools uses that same file as the
	// compile base. The directory handed to the wrapper is the .build root.
	std::wstring shippedExeDir;
	{
		const wchar_t *argvBuild = L"..\\..\\.build"; // CWD is .temp/<gate> at run time
		wchar_t modulePath[MAX_PATH] = { 0 };
		GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
		std::wstring moduleDir(modulePath);
		const size_t slash = moduleDir.find_last_of(L'\\');
		moduleDir.resize(slash == std::wstring::npos ? 0 : slash);
		// .temp/bin -> ../../.build
		const std::wstring buildDir = moduleDir + L"\\..\\..\\.build";
		if (HasFile(buildDir + L"\\AutoHotkey64.exe"))
			shippedExeDir = buildDir;
		else if (HasFile(std::wstring(argvBuild) + L"\\AutoHotkey64.exe"))
			shippedExeDir = argvBuild;
	}

	if (shippedExeDir.empty())
	{
		Skip("端到端绿臂（未 stage .build\\AutoHotkey64.exe，构建机才会跑）");
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

		ExeCompileOutcome ok = CompileScriptToExe(shippedExeDir, script, out, 60000);
		Check(ok.rc == 0 && ok.produced && HasFile(out), "端到端绿臂：正常脚本 rc=0 且产出 exe");
		Check(!ok.backedUp, "端到端绿臂：首次编译不报告备份");

		// m188: a file on disk only proves UpdateResource returned TRUE - not
		// that the runtime can find the script inside it. Run the thing: this
		// banner script exits by itself, while a bundle written with the wrong
		// resource id sits on a dialog until the timeout below.
		{
			STARTUPINFOW si = {};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi = {};
			const BOOL ran = CreateProcessW(out.c_str(), nullptr, nullptr, nullptr,
				FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
			Check(ran != FALSE, "端到端：产物可以启动");
			if (ran)
			{
				const DWORD waited = WaitForSingleObject(pi.hProcess, 15000);
				DWORD runRc = 1;
				GetExitCodeProcess(pi.hProcess, &runRc);
				if (waited == WAIT_TIMEOUT)
					TerminateProcess(pi.hProcess, 1);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
				Check(waited != WAIT_TIMEOUT && runRc == 0,
					"端到端：产物能运行并自行退出（资源 id 与运行时对得上）");
			}
		}

		// Second compile over the same target must preserve it as .old.
		ExeCompileOutcome again = CompileScriptToExe(shippedExeDir, script, out, 60000);
		Check(again.rc == 0 && again.produced && again.backedUp &&
			HasFile(out + L".old"), "端到端绿臂：重复编译旧产物备份为 .old");

		// m189: a product compiled with no sibling .ico must keep the base icon
		// byte for byte - the artwork pass must not disturb what it does not own.
		{
			const std::wstring baseExe = shippedExeDir + L"\\AutoHotkey64.exe";
			const WORD groupId = FindFirstResourceId(baseExe, 14, 600);
			Check(groupId != 0, "图标臂：底座有可替换的图标组");
			std::string built;
			std::string baseGroup;
			Check(ReadResource(out, 14, groupId, built) && !built.empty(),
				"图标臂：产物自带图标组");
			Check(ReadResource(baseExe, 14, groupId, baseGroup) && built == baseGroup,
				"图标臂：未指定 .ico 时底座图标原样保留");
		}

		// m189: the artwork arm. A generated .ico plus explicit metadata, then
		// read the resources back out of the product.
		{
			const std::wstring ico = dir + L"mark.ico";
			WriteRaw(ico, MakeTestIcon());
			const std::wstring artOut = dir + L"art.exe";
			DeleteFileW(artOut.c_str());

			ExeCompileOptions options;
			options.iconPath = ico;
			options.fileVersion = L"2.3.4.5";
			options.productName = L"合同测试";
			options.fileDescription = L"图标与版本臂";

			ExeCompileOutcome art = CompileScriptToExe(shippedExeDir, script, artOut,
				options, 60000);
			Check(art.rc == 0 && art.produced && HasFile(artOut),
				"图标/版本臂：带选项编译成功");

			const std::wstring baseExe = shippedExeDir + L"\\AutoHotkey64.exe";
			const WORD groupId = FindFirstResourceId(baseExe, 14, 600);
			std::string group;
			const bool haveGroup = ReadResource(artOut, 14, groupId, group);
			Check(haveGroup && group.size() >= 20
				&& group[4] == 1 && group[5] == 0,
				"图标臂：图标组只含写入的那一帧");
			const unsigned imageId = haveGroup
				? (static_cast<unsigned char>(group[18])
					| (static_cast<unsigned char>(group[19]) << 8))
				: 0;
			std::string image;
			Check(imageId != 0 && ReadResource(artOut, 3,
				static_cast<WORD>(imageId), image) && image.size() == 1128,
				"图标臂：图标图像正是生成的 16x16 帧（1128 字节）");

			std::string version;
			Check(ReadResource(artOut, 16, 1, version) && !version.empty(),
				"版本臂：产物含版本资源");
			Check(version.size() > 56 && ReadU32(version, 40) == 0xFEEF04BD,
				"版本臂：VS_FIXEDFILEINFO 签名在预期偏移上");
			Check(ReadU32(version, 48) == 0x00020003 && ReadU32(version, 52) == 0x00040005,
				"版本臂：固定版本字段 = 2.3.4.5");
			Check(ContainsWide(version, L"2.3.4.5"),
				"版本臂：FileVersion 文本已写入");
			Check(ContainsWide(version, L"合同测试"),
				"版本臂：中文 ProductName 按 UTF-16 写入");

			DeleteFileW(ico.c_str());
			DeleteFileW(artOut.c_str());
		}

		const std::wstring bad = dir + L"bad.ahk";
		const std::wstring badOut = dir + L"bad.exe";
		DeleteFileW(badOut.c_str());
		WriteRaw(bad, "#Include %A_ScriptDir%\\ahkdeck-missing-file-9527.xyz\n");
		ExeCompileOutcome red = CompileScriptToExe(shippedExeDir, bad, badOut, 60000);
		Check(red.rc != 0 && !red.produced && !HasFile(badOut),
			"端到端红臂：坏脚本 rc!=0 且不产出");
		Check(!red.output.empty(), "端到端红臂：编译器原文被捕获回传");

		DeleteFileW(script.c_str());
		DeleteFileW(out.c_str());
		DeleteFileW((out + L".old").c_str());
		DeleteFileW(bad.c_str());
		DeleteFileW((dir + L"bad.exe").c_str());
	}

	if (gFailures == 0)
		std::printf("ALL GREEN\n");
	return gFailures;
}

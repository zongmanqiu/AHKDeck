// core/ZipPackage.cpp - m211, reworked m212. See the header.
//
// No third-party zip library any more. The reader here is split in two:
//   1. a raw central-directory parser (~150 lines of plain C++) that validates
//      every entry name and the size budget BEFORE anything touches the disk;
//   2. the system's own extraction (Shell "zip folder" namespace, i.e. the
//      same code Explorer uses when double-clicking a zip) - built into
//      Windows 10/11, zero bytes in our exe, and it handles non-ASCII names
//      through wide paths (the old miniz file API was ANSI-only).
#include "core/ZipPackage.h"

#include <windows.h>
#include <shlobj.h> // IShellDispatch / Folder: the Explorer zip namespace

#include <cstdio>
#include <string>
#include <vector>

namespace
{

std::wstring Utf8ToWide(const std::string &aText)
{
	if (aText.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, aText.c_str(),
		(int)aText.size(), nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aText.c_str(), (int)aText.size(),
		out.data(), n);
	return out;
}

// ---------------------------------------------------------------------------
// 1. Central directory reader - name and budget validation, no extraction.
// ---------------------------------------------------------------------------

struct CdEntry
{
	std::string name;
	unsigned long long uncomp = 0;
	bool isDir = false;
};

unsigned short ReadU16(const unsigned char *p)
{
	return (unsigned short)(p[0] | (p[1] << 8));
}

unsigned long ReadU32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
		| ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

bool ReadFileRange(HANDLE aFile, long long aOffset, void *aBuf, DWORD aBytes)
{
	LARGE_INTEGER seek;
	seek.QuadPart = aOffset;
	if (!SetFilePointerEx(aFile, seek, nullptr, FILE_BEGIN))
		return false;
	DWORD got = 0;
	return ReadFile(aFile, aBuf, aBytes, &got, nullptr) && got == aBytes;
}

// One archived path, checked to be relative and free of traversal segments.
// Zip names use '/', so the checks work on the raw bytes regardless of which
// code page non-ASCII parts were written in (the names never build paths
// here - Explorer does the extraction and handles the encoding itself).
bool SafeEntryName(const std::string &aName, std::string &aReason)
{
	if (aName.empty() || aName.size() > 240)
	{
		aReason = "条目名缺失或过长";
		return false;
	}
	if (aName.find('\\') != std::string::npos)
	{
		aReason = "条目名带反斜杠";
		return false;
	}
	if (aName.front() == '/' || aName.find(':') != std::string::npos)
	{
		aReason = "条目名是绝对路径";
		return false;
	}
	size_t start = 0;
	while (start <= aName.size())
	{
		size_t slash = aName.find('/', start);
		const std::string segment = aName.substr(start,
			slash == std::string::npos ? std::string::npos : slash - start);
		if (segment.empty() && slash != std::string::npos)
		{
			aReason = "条目名含空路径段";
			return false;
		}
		if (segment == ".." || segment == ".")
		{
			aReason = "条目名含 .. 或 . 段";
			return false;
		}
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return true;
}

// Reads and validates the central directory only (never the file data).
bool ReadCentralDirectory(const std::wstring &aZipPath,
	std::vector<CdEntry> &aEntries, std::string &aError)
{
	HANDLE file = CreateFileW(aZipPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		aError = "打不开 zip 包（文件不存在或被占用）";
		return false;
	}

	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 22)
	{
		CloseHandle(file);
		aError = "打不开 zip 包（文件损坏或不是 zip）";
		return false;
	}
	if (size.QuadPart > 1024LL * 1024 * 1024)
	{
		CloseHandle(file);
		aError = "zip 包本体超过 1 GB，拒绝处理";
		return false;
	}

	// Scan the last 66 KB for the end-of-central-directory record and take
	// the last candidate whose comment length is consistent with the file
	// size (a comment or an in-archive false positive sits before it).
	const DWORD tail = (DWORD)((size.QuadPart < 66 * 1024)
		? size.QuadPart : 66 * 1024);
	std::vector<unsigned char> buf(tail);
	if (!ReadFileRange(file, size.QuadPart - tail, buf.data(), tail))
	{
		CloseHandle(file);
		aError = "读 zip 包失败";
		return false;
	}
	long long eocdOff = -1;
	for (DWORD i = tail - 22 + 1; i-- > 0;)
	{
		if (ReadU32(buf.data() + i) != 0x06054b50)
			continue;
		const unsigned short comment = ReadU16(buf.data() + i + 20);
		if ((long long)i + 22 + comment == tail)
		{
			eocdOff = (long long)i;
			break;
		}
	}
	if (eocdOff < 0)
	{
		CloseHandle(file);
		aError = "打不开 zip 包（文件损坏或不是 zip）";
		return false;
	}
	const unsigned short total = ReadU16(buf.data() + eocdOff + 10);
	const unsigned long cdSize = ReadU32(buf.data() + eocdOff + 12);
	const unsigned long cdOff = ReadU32(buf.data() + eocdOff + 16);
	if (total == 0xFFFF || cdSize == 0xFFFFFFFFul || cdOff == 0xFFFFFFFFul)
	{
		CloseHandle(file);
		aError = "不支持 zip64 格式的包";
		return false;
	}
	if (total == 0 || total > 512)
	{
		CloseHandle(file);
		aError = "包内条目数为 0 或超过 512";
		return false;
	}
	if ((long long)cdOff + cdSize > size.QuadPart)
	{
		CloseHandle(file);
		aError = "打不开 zip 包（中央目录越界）";
		return false;
	}

	std::vector<unsigned char> cd(cdSize);
	if (!ReadFileRange(file, (long long)cdOff, cd.data(), cdSize))
	{
		CloseHandle(file);
		aError = "读 zip 包失败";
		return false;
	}
	CloseHandle(file);

	size_t pos = 0;
	long long totalBytes = 0;
	for (unsigned short i = 0; i < total; ++i)
	{
		if (pos + 46 > cd.size() || ReadU32(cd.data() + pos) != 0x02014b50)
		{
			aError = "打不开 zip 包（中央目录损坏）";
			return false;
		}
		const unsigned long uncomp = ReadU32(cd.data() + pos + 24);
		const unsigned short nameLen = ReadU16(cd.data() + pos + 28);
		const unsigned short extraLen = ReadU16(cd.data() + pos + 30);
		const unsigned short commentLen = ReadU16(cd.data() + pos + 32);
		const unsigned long extAttr = ReadU32(cd.data() + pos + 38);
		if (pos + 46 + nameLen > cd.size())
		{
			aError = "打不开 zip 包（中央目录损坏）";
			return false;
		}
		CdEntry entry;
		entry.name.assign((const char *)cd.data() + pos + 46, nameLen);
		entry.uncomp = uncomp;
		entry.isDir = (extAttr & 0x10) != 0
			|| (!entry.name.empty() && entry.name.back() == '/');

		if (uncomp > 64LL * 1024 * 1024)
		{
			aError = "包内有超过 64 MB 的单条目";
			return false;
		}
		if (!entry.isDir)
		{
			totalBytes += (long long)uncomp;
			if (totalBytes > 128LL * 1024 * 1024)
			{
				aError = "包解压后超过 128 MB";
				return false;
			}
		}
		std::string reason;
		if (!SafeEntryName(entry.name, reason))
		{
			aError = "包内存在不安全条目（" + reason + "）: " + entry.name;
			return false;
		}
		aEntries.push_back(std::move(entry));
		pos += 46 + (size_t)nameLen + extraLen + commentLen;
	}
	if (aEntries.empty())
	{
		aError = "包内条目数为 0 或超过 512";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// 2. Extraction through the system's zip namespace.
// ---------------------------------------------------------------------------

// Recursive file count over an extracted tree (files only).
long CountFilesOnDisk(const std::wstring &aDir)
{
	long count = 0;
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((aDir + L"\\*").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		const std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..")
			continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			count += CountFilesOnDisk(aDir + L"\\" + name);
		else
			++count;
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	return count;
}

// Polls until the extracted file count matches the archive and stays stable
// for two consecutive reads: CopyHere is asynchronous, and the last file of
// the batch may still be mid-write when the first count lands. A short block
// on the calling thread is fine; we deliberately do NOT pump messages, so
// there is no re-entrancy into the caller's UI loop.
bool WaitForExtraction(const std::wstring &aDestDir, long aExpectedFiles,
	std::string &aError)
{
	const ULONGLONG deadline = GetTickCount64() + 60000;
	long stable = 0;
	while (GetTickCount64() < deadline)
	{
		Sleep(120);
		const long got = CountFilesOnDisk(aDestDir);
		if (got == aExpectedFiles)
		{
			if (++stable >= 2)
				return true;
		}
		else
			stable = 0;
	}
	aError = "解压超时（60 秒内未完成）";
	return false;
}

// Post-walk: the budget and the name checks ran pre-extraction against the
// central directory; this pass makes sure what landed on disk agrees.
bool ValidateExtractedTree(const std::wstring &aDir, long aExpectedFiles,
	long long &aTotalBytes, std::string &aError)
{
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((aDir + L"\\*").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
	{
		aError = "解压结果为空";
		return false;
	}
	bool ok = true;
	do
	{
		const std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..")
			continue;
		if (name.find(L':') != std::wstring::npos)
		{
			aError = "解压结果含不安全条目";
			ok = false;
			break;
		}
		const std::wstring full = aDir + L"\\" + name;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (!ValidateExtractedTree(full, -1, aTotalBytes, aError))
			{
				ok = false;
				break;
			}
		}
		else
		{
			const long long bytes = ((long long)fd.nFileSizeHigh << 32)
				| fd.nFileSizeLow;
			if (bytes > 64LL * 1024 * 1024)
			{
				aError = "包内有超过 64 MB 的单条目";
				ok = false;
				break;
			}
			aTotalBytes += bytes;
			if (aTotalBytes > 128LL * 1024 * 1024)
			{
				aError = "包解压后超过 128 MB";
				ok = false;
				break;
			}
		}
	} while (FindNextFileW(find, &fd) && ok);
	FindClose(find);
	if (ok && aExpectedFiles >= 0)
	{
		const long got = CountFilesOnDisk(aDir);
		if (got != aExpectedFiles)
		{
			aError = "解压结果与包内容不符（" + std::to_string(got) + "/"
				+ std::to_string(aExpectedFiles) + " 个文件）";
			ok = false;
		}
	}
	return ok;
}

struct VariantGuard
{
	VARIANT v;
	VariantGuard() { VariantInit(&v); }
	~VariantGuard() { VariantClear(&v); }
};

template <typename T>
struct ComPtr
{
	T *p = nullptr;
	~ComPtr() { if (p) p->Release(); }
};

} // namespace

namespace ahkdeck
{

bool RemoveTreeWide(const std::wstring &aDir)
{
	std::wstring pattern = aDir + L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
	bool ok = true;
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..")
				continue;
			const std::wstring full = aDir + L"\\" + name;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				{
					// m240/A2: a junction/symlink directory is only ever the
					// link itself here - RemoveDirectoryW deletes the link and
					// never touches the target. Recursing into it would wipe
					// whatever the link points at, which is exactly what
					// core/Housekeeping refuses to do ("never follow
					// junctions"). Same rule, destructive side.
					if (!RemoveDirectoryW(full.c_str()))
						ok = false;
					continue;
				}
				if (!RemoveTreeWide(full))
					ok = false;
			}
			else
			{
				if (!DeleteFileW(full.c_str()))
					ok = false;
			}
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	return RemoveDirectoryW(aDir.c_str()) != FALSE && ok;
}

static bool CopyTreeWide(const std::wstring &aFrom, const std::wstring &aTo)
{
	if (!CreateDirectoryW(aTo.c_str(), nullptr))
		return false;
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((aFrom + L"\\*").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
		return true; // an empty directory copy is just the directory itself
	bool ok = true;
	do
	{
		const std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..")
			continue;
		const std::wstring src = aFrom + L"\\" + name;
		const std::wstring dst = aTo + L"\\" + name;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			continue; // m240/A2: never follow a junction in either direction - the backup holds real files only
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (!CopyTreeWide(src, dst))
				ok = false;
		}
		else
		{
			if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
				ok = false;
		}
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	return ok;
}

bool MoveDirToBackup(const std::wstring &aDir, const std::wstring &aTargetDir,
	std::string &aOutLeafUtf8)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char stamp[40];
	std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	// The stem doubles as the backup prefix, so a folder `名\` becomes
	// `名-<stamp>\` in the backup directory - readable and collision-free.
	const size_t slash = aDir.find_last_of(L"\\/");
	std::wstring stem = slash == std::wstring::npos ? aDir : aDir.substr(slash + 1);
	char stemUtf8[128] = { 0 };
	{
		const std::wstring ws = stem;
		const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
			stemUtf8, sizeof(stemUtf8) - 1, nullptr, nullptr);
		if (n <= 0)
			return false;
		stemUtf8[n] = '\0';
	}

	for (int attempt = 1; attempt <= 99; ++attempt)
	{
		std::string candidate = std::string(stemUtf8) + "-" + stamp;
		if (attempt > 1)
			candidate += "-" + std::to_string(attempt);
		const std::wstring target = aTargetDir + L"\\" + Utf8ToWide(candidate);
		if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
			continue;

		if (MoveFileW(aDir.c_str(), target.c_str()))
		{
			aOutLeafUtf8 = candidate;
			return true;
		}

		// %TEMP% may sit on another volume than the install directory (test
		// sandboxes do; some user setups too), and MoveFile reports that -
		// or plain access denied - without moving anything. The fallback is
		// copy + delete, but delete is the destructive half: m240/A1 - a
		// directory held open (script CWD, a viewer) makes RemoveTreeWide
		// PARTIAL: it wipes what it can, fails on the lock, and the caller
		// then treats the move as failed while the source is half gone.
		// Guard it in two steps:
		//   1. rename probe - a locked directory cannot be renamed either,
		//      so probe failure means the delete below would shred the
		//      source; fail safely instead (source untouched, no copy made).
		//   2. once the copy is in the backup dir, a partial delete never
		//      destroys the copy: report success with the leaf so the caller
		//      records where the content lives, leaving the leftover shell
		//      behind for hand recovery.
		{
			const std::wstring probe = aDir + L".~bakprobe";
			if (!MoveFileW(aDir.c_str(), probe.c_str()))
				return false; // locked: deleting now would shred the source
			if (!MoveFileW(probe.c_str(), aDir.c_str()))
			{
				// m240 second look: the probe rename worked but the restore
				// did not (a handle arrived in the window). The content now
				// lives in the probe directory - finish the move by renaming
				// IT into the backup slot instead of failing with data
				// stranded at an unrecorded name.
				if (MoveFileW(probe.c_str(), target.c_str()))
				{
					aOutLeafUtf8 = candidate;
					return true;
				}
				return false; // content survives in <dir>.~bakprobe, recoverable
			}
			if (!CopyTreeWide(aDir, target))
				return false; // copy failed: source intact, safe failure
			aOutLeafUtf8 = candidate;
			(void)RemoveTreeWide(aDir); // best effort; failure keeps the copy
			return true;
		}
	}
	return false;
}

// Windows' CompressedFolder shell extension is registered for the ".zip"
// extension only (HKCR\.zip -> CompressedFolder). Shell.NameSpace() therefore
// cannot open an archive just because its bytes are a zip: a perfectly valid
// package named *.part is rejected, and the generic Shell failure then reads
// as "the file is corrupt". That is exactly what m226 fixed.
bool HasZipExtension(const std::wstring &aPath)
{
	const size_t dot = aPath.find_last_of(L'.');
	const size_t sep = aPath.find_last_of(L"\\/");
	if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep))
		return false;
	std::wstring ext = aPath.substr(dot + 1);
	for (wchar_t &c : ext)
		if (c >= L'A' && c <= L'Z')
			c = (wchar_t)(c - L'A' + L'a');
	return ext == L"zip";
}

bool ExtractZipSafe(const std::wstring &aZipPath, const std::wstring &aDestDir,
	std::string &aError)
{
	// Pass 1: validate names and the size budget from the central directory.
	std::vector<CdEntry> entries;
	if (!ReadCentralDirectory(aZipPath, entries, aError))
		return false;
	long expectedFiles = 0;
	for (const CdEntry &e : entries)
		if (!e.isDir)
			++expectedFiles;
	if (expectedFiles == 0)
	{
		aError = "包内没有任何文件";
		return false;
	}

	// Pass 2 hands the file to the Shell, which only recognises the .zip
	// extension. Normalise the name here (copy next to the staging dir, delete
	// it on every exit path) so no caller has to know that rule. The installer
	// no longer stages as "<name>.zip.part" (m226 moved packages to
	// temp\work\package-<pid>.zip), but this stays as the layer's own guard:
	// a partial or oddly named archive must not turn into "corrupt file".
	std::wstring shellZip = aZipPath;
	std::wstring tempZip;
	if (!HasZipExtension(aZipPath))
	{
		std::wstring base = aDestDir;
		while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
			base.pop_back();
		if (base.empty())
		{
			aError = "解压目标路径形态不对";
			return false;
		}
		tempZip = base + L".package.zip";
		DeleteFileW(tempZip.c_str());
		if (!CopyFileW(aZipPath.c_str(), tempZip.c_str(), FALSE))
		{
			aError = "无法准备临时 zip 包（复制失败，错误码 " +
				std::to_string(GetLastError()) + "）";
			return false;
		}
		shellZip = tempZip;
	}
	struct TempZipGuard
	{
		std::wstring path;
		~TempZipGuard()
		{
			if (!path.empty())
				DeleteFileW(path.c_str());
		}
	} tempZipGuard{ tempZip };

	// Pass 2: the system extracts. Everything below runs through wide BSTR
	// paths, so non-ASCII install directories work without any code-page
	// fallback (this is why Shell beats a narrow-path zip library).
	if (!CreateDirectoryW(aDestDir.c_str(), nullptr)
		&& GetLastError() != ERROR_ALREADY_EXISTS)
	{
		aError = "无法创建解压目录";
		return false;
	}

	// An existing apartment (RPC_E_CHANGED_MODE / S_FALSE) is fine for Shell
	// dispatch; ownInit only records whether we must CoUninitialize later.
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool ownInit = SUCCEEDED(hrInit);
	ComPtr<IShellDispatch> shell;
	HRESULT hr = CoCreateInstance(__uuidof(Shell), nullptr, CLSCTX_INPROC_SERVER,
		__uuidof(IShellDispatch), (void **)&shell.p);
	if (FAILED(hr))
	{
		if (ownInit)
			CoUninitialize();
		aError = "系统解压组件初始化失败 (0x" + [&hr] {
			char b[16];
			std::snprintf(b, sizeof(b), "%08lX", (unsigned long)hr);
			return std::string(b);
		}() + ")";
		return false;
	}

	bool ok = false;
	{
		VariantGuard vZip;
		vZip.v.vt = VT_BSTR;
		vZip.v.bstrVal = SysAllocStringLen(shellZip.c_str(),
			(UINT)shellZip.size());
		VariantGuard vDest;
		vDest.v.vt = VT_BSTR;
		vDest.v.bstrVal = SysAllocStringLen(aDestDir.c_str(),
			(UINT)aDestDir.size());
		if (!vZip.v.bstrVal || !vDest.v.bstrVal)
			aError = "内存不足";
		else
		{
			ComPtr<Folder> src;
			ComPtr<FolderItems> items;
			ComPtr<Folder> dst;
			if (FAILED(shell.p->NameSpace(vZip.v, &src.p)) || !src.p
				|| FAILED(src.p->Items(&items.p)) || !items.p)
				aError = "打不开 zip 包（文件损坏或不是 zip）";
			else
			{
				long topCount = 0;
				items.p->get_Count(&topCount);
				if (topCount <= 0)
					aError = "包内条目数为 0 或超过 512";
				else if (FAILED(shell.p->NameSpace(vDest.v, &dst.p)) || !dst.p)
					aError = "无法创建解压目录";
				else
				{
					VariantGuard vItems;
					vItems.v.vt = VT_DISPATCH;
					vItems.v.pdispVal = (IDispatch *)items.p;
					vItems.v.pdispVal->AddRef();
					VariantGuard vFlags;
					vFlags.v.vt = VT_I4;
					// 4 no dialog, 16 yes-to-all, 512/1024 no confirmations.
					vFlags.v.lVal = 4 | 16 | 512 | 1024;
					dst.p->CopyHere(vItems.v, vFlags.v);
					if (WaitForExtraction(aDestDir, expectedFiles, aError))
					{
						long long totalBytes = 0;
						if (ValidateExtractedTree(aDestDir, expectedFiles,
								totalBytes, aError))
							ok = true;
					}
				}
			}
		}
	}
	if (ownInit)
		CoUninitialize();
	if (!ok)
		RemoveTreeWide(aDestDir);
	return ok;
}

} // namespace ahkdeck

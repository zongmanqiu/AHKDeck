// core/Updater.cpp - see core/Updater.h. FLTK-free on purpose.
#include "core/Updater.h"

#include "core/AppPaths.h"
#include "core/Json.h"
#include "core/TextFile.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace ahkdeck
{
namespace
{

// UTF-8 -> wide (WinHTTP and the file APIs take wide strings). A naive
// std::wstring(s.begin(), s.end()) would mangle every non-ASCII byte.
std::wstring Utf8ToWide(const std::string &aText)
{
	if (aText.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, aText.data(),
		(int)aText.size(), nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, aText.data(),
		(int)aText.size(), out.data(), n);
	return out;
}

std::string WideToUtf8(const std::wstring &aText)
{
	if (aText.empty())
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, aText.c_str(), (int)aText.size(),
		nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return std::string();
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aText.c_str(), (int)aText.size(), out.data(), n,
		nullptr, nullptr);
	return out;
}

// "v1.2.3" / "1.2" -> up to three integers.
void ParseVersionPart(const std::string &aText, int aOut[3])
{
	aOut[0] = aOut[1] = aOut[2] = 0;
	std::string s = aText;
	if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
		s.erase(0, 1);
	size_t i = 0;
	for (int part = 0; part < 3; ++part)
	{
		int value = 0;
		bool any = false;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9')
		{
			value = value * 10 + (s[i] - '0');
			any = true;
			++i;
		}
		if (any)
			aOut[part] = value;
		if (i < s.size() && s[i] == '.')
			++i;
		else
			break;
	}
}

// Flat manifest reader. `version` and at least one package URL are mandatory:
// without a URL the check could be displayed but never acted on, which reads
// worse than reporting the manifest as unusable.
bool ParseManifest(const std::string &aBody, UpdateManifest &aOut)
{
	std::string body = aBody;
	// A manifest authored on Windows may start with a UTF-8 BOM; the JSON
	// parser expects the first character to be the value itself.
	if (body.size() >= 3 && (unsigned char)body[0] == 0xEF &&
		(unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
		body.erase(0, 3);

	Json root;
	std::string error;
	if (!Json::Parse(body, root, error) || !root.IsObject())
		return false;

	UpdateManifest manifest;
	manifest.version = root["version"].AsString();
	manifest.published = root["published"].AsString();
	manifest.notes = root["notes"].AsString();
	manifest.notesEn = root["notes_en"].AsString();
	manifest.urlGitee = root["url_gitee"].AsString();
	manifest.urlGithub = root["url_github"].AsString();
	if (manifest.version.empty() ||
		(manifest.urlGitee.empty() && manifest.urlGithub.empty()))
		return false;

	aOut = std::move(manifest);
	return true;
}

// The whole file as text (BOM/encoding handled by the shared codec), so a
// cache written by an older build still parses.
bool ReadWholeFile(const std::string &aPathUtf8, std::string &aOut)
{
	const std::wstring path = Utf8ToWide(aPathUtf8);
	if (path.empty())
		return false;
	TextDocument document;
	std::string error;
	if (!ReadTextDocument(path, document, error))
		return false;
	aOut = std::move(document.utf8);
	return true;
}

// Seconds since the file was last written; -1 when it cannot be read.
long long FileAgeSeconds(const std::string &aPathUtf8)
{
	const std::wstring path = Utf8ToWide(aPathUtf8);
	if (path.empty())
		return -1;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	long long age = -1;
	FILETIME written;
	if (GetFileTime(file, nullptr, nullptr, &written))
	{
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		ULARGE_INTEGER current;
		current.HighPart = now.dwHighDateTime;
		current.LowPart = now.dwLowDateTime;
		ULARGE_INTEGER stamp;
		stamp.HighPart = written.dwHighDateTime;
		stamp.LowPart = written.dwLowDateTime;
		if (current.QuadPart >= stamp.QuadPart)
			age = (long long)((current.QuadPart - stamp.QuadPart) / 10000000ULL);
	}
	CloseHandle(file);
	return age;
}

// Parse [http|https]://host[:port]/path.
bool SplitUrl(const std::string &aUrl, std::string &aHost, std::string &aPath,
	bool &aTls, INTERNET_PORT &aPort)
{
	std::string url = aUrl;
	if (url.rfind("https://", 0) == 0)
	{
		aTls = true;
		url.erase(0, 8);
	}
	else if (url.rfind("http://", 0) == 0)
	{
		aTls = false;
		url.erase(0, 7);
	}
	else
	{
		aTls = true;
	}
	const size_t colon = url.find(':');
	const size_t slash = url.find('/');
	if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
	{
		const long portNumber = std::atol(url.c_str() + colon + 1);
		if (portNumber > 0 && portNumber <= 65535)
			aPort = (INTERNET_PORT)portNumber;
		url = url.substr(0, colon) +
			(slash != std::string::npos ? url.substr(slash) : std::string());
	}
	const size_t slash2 = url.find('/');
	if (slash2 != std::string::npos)
	{
		aHost = url.substr(0, slash2);
		aPath = url.substr(slash2);
	}
	else
	{
		aHost = url;
		aPath = "/";
	}
	if (!aTls && aPort == INTERNET_DEFAULT_HTTPS_PORT)
		aPort = INTERNET_DEFAULT_HTTP_PORT;
	return !aHost.empty();
}

// Follow one 3xx by re-issuing the request to Location. Gitee's raw host may
// redirect; WinHTTP's automatic redirect is not exposed on every SDK, so this
// is handled here - and with a hop bound, unlike an automatic follow.
bool FollowRedirect(const std::wstring &aLocation, std::string &aHost,
	std::string &aPath, bool &aTls, INTERNET_PORT &aPort)
{
	const std::string location = WideToUtf8(aLocation);
	if (location.empty())
		return false;
	if (location[0] == '/')
	{
		aPath = location; // same host, new path
		return true;
	}
	return SplitUrl(location, aHost, aPath, aTls, aPort);
}

// Rebuilds the absolute URL of the next hop after a redirect.
std::string AbsoluteUrl(const std::string &aHost, const std::string &aPath,
	bool aTls, INTERNET_PORT aPort)
{
	const std::string scheme = aTls ? "https://" : "http://";
	const int port = (aPort == INTERNET_DEFAULT_HTTPS_PORT ||
		aPort == INTERNET_DEFAULT_HTTP_PORT) ? -1 : (int)aPort;
	return scheme + aHost + (port > 0 ? (":" + std::to_string(port)) : "") + aPath;
}

// GET `aUrl`, body into `aOut`. Synchronous, blocking, bounded redirects.
// A 4xx/5xx is a failure: an error page is not a manifest, and handing one to
// the JSON reader would produce a far less useful message.
bool HttpGetBody(const std::string &aUrl, int aTimeoutMs, std::string &aOut,
	std::string &aError)
{
	std::string current = aUrl;
	for (int hop = 0; hop <= 8; ++hop)
	{
		std::string host, path;
		bool tls = true;
		INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
		if (!SplitUrl(current, host, path, tls, port))
		{
			aError = "invalid url";
			return false;
		}
		const std::wstring wideHost = Utf8ToWide(host);
		const std::wstring widePath = Utf8ToWide(path);

		HINTERNET session = WinHttpOpen(L"AHKDeckUpdater/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
		{
			aError = "WinHttpOpen failed";
			return false;
		}
		HINTERNET connection = WinHttpConnect(session, wideHost.c_str(), port, 0);
		if (!connection)
		{
			aError = "WinHttpConnect failed";
			WinHttpCloseHandle(session);
			return false;
		}
		HINTERNET request = WinHttpOpenRequest(connection, L"GET", widePath.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			tls ? WINHTTP_FLAG_SECURE : 0);
		if (!request)
		{
			aError = "WinHttpOpenRequest failed";
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		WinHttpSetTimeouts(request, aTimeoutMs, aTimeoutMs, aTimeoutMs, aTimeoutMs);

		if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0,
			0, 0))
		{
			aError = "WinHttpSendRequest failed (timeout or network)";
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}
		if (!WinHttpReceiveResponse(request, nullptr))
		{
			aError = "WinHttpReceiveResponse failed";
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		DWORD status = 0;
		DWORD statusSize = (DWORD)sizeof(status);
		if (WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, nullptr))
		{
			if (status == 301 || status == 302 || status == 303 || status == 307 ||
				status == 308)
			{
				wchar_t location[4096] = {0};
				DWORD locationSize = (DWORD)sizeof(location);
				if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
					WINHTTP_HEADER_NAME_BY_INDEX, location, &locationSize, nullptr) &&
					FollowRedirect(std::wstring(location,
						locationSize / sizeof(wchar_t)), host, path, tls, port))
				{
					current = AbsoluteUrl(host, path, tls, port);
					WinHttpCloseHandle(request);
					WinHttpCloseHandle(connection);
					WinHttpCloseHandle(session);
					continue;
				}
				aError = "server redirected but no usable Location header";
				WinHttpCloseHandle(request);
				WinHttpCloseHandle(connection);
				WinHttpCloseHandle(session);
				return false;
			}
			if (status >= 400)
			{
				aError = "HTTP " + std::to_string(status);
				WinHttpCloseHandle(request);
				WinHttpCloseHandle(connection);
				WinHttpCloseHandle(session);
				return false;
			}
		}

		char buffer[16384];
		aOut.clear();
		bool readOk = true;
		for (;;)
		{
			DWORD chunk = 0;
			// TRUE + 0 bytes = the server finished sending. FALSE = the
			// connection broke mid-body; a truncated manifest must not reach
			// the JSON parser as if it were complete.
			if (!WinHttpReadData(request, buffer, (DWORD)sizeof(buffer), &chunk))
			{
				readOk = false;
				break;
			}
			if (chunk == 0)
				break;
			aOut.append(buffer, chunk);
		}
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		if (!readOk)
		{
			aError = "download interrupted";
			return false;
		}
		return true;
	}
	aError = "too many redirects";
	return false;
}

// GET `aUrl`, stream the body to a wide-path file. Same redirect rule as
// HttpGetBody; a partial transfer deletes the file instead of leaving a
// package that would only fail later inside Expand-Archive.
bool HttpDownload(const std::string &aUrl, const std::wstring &aSavePath,
	int aTimeoutMs, std::string &aError, const DownloadProgress &aProgress)
{
	std::string current = aUrl;
	for (int hop = 0; hop <= 8; ++hop)
	{
		std::string host, path;
		bool tls = true;
		INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
		if (!SplitUrl(current, host, path, tls, port))
		{
			aError = "invalid url";
			return false;
		}
		const std::wstring wideHost = Utf8ToWide(host);
		const std::wstring widePath = Utf8ToWide(path);

		HINTERNET session = WinHttpOpen(L"AHKDeckUpdater/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
		{
			aError = "WinHttpOpen failed";
			return false;
		}
		HINTERNET connection = WinHttpConnect(session, wideHost.c_str(), port, 0);
		if (!connection)
		{
			aError = "WinHttpConnect failed";
			WinHttpCloseHandle(session);
			return false;
		}
		HINTERNET request = WinHttpOpenRequest(connection, L"GET", widePath.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			tls ? WINHTTP_FLAG_SECURE : 0);
		if (!request)
		{
			aError = "WinHttpOpenRequest failed";
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		WinHttpSetTimeouts(request, aTimeoutMs, aTimeoutMs, aTimeoutMs, aTimeoutMs);

		if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0,
			0, 0))
		{
			aError = "WinHttpSendRequest failed (timeout or network)";
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}
		if (!WinHttpReceiveResponse(request, nullptr))
		{
			aError = "WinHttpReceiveResponse failed";
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		DWORD status = 0;
		DWORD statusSize = (DWORD)sizeof(status);
		if (WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, nullptr))
		{
			if (status == 301 || status == 302 || status == 303 || status == 307 ||
				status == 308)
			{
				wchar_t location[4096] = {0};
				DWORD locationSize = (DWORD)sizeof(location);
				if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
					WINHTTP_HEADER_NAME_BY_INDEX, location, &locationSize, nullptr) &&
					FollowRedirect(std::wstring(location,
						locationSize / sizeof(wchar_t)), host, path, tls, port))
				{
					current = AbsoluteUrl(host, path, tls, port);
					WinHttpCloseHandle(request);
					WinHttpCloseHandle(connection);
					WinHttpCloseHandle(session);
					continue;
				}
				aError = "server redirected but no usable Location header";
				WinHttpCloseHandle(request);
				WinHttpCloseHandle(connection);
				WinHttpCloseHandle(session);
				return false;
			}
			if (status >= 400)
			{
				// Never save an error page as the package.
				aError = "HTTP " + std::to_string(status);
				WinHttpCloseHandle(request);
				WinHttpCloseHandle(connection);
				WinHttpCloseHandle(session);
				return false;
			}
		}

		HANDLE file = CreateFileW(aSavePath.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			aError = "cannot open save file (check write permission)";
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		// Content-Length (0 = unknown) lets the dialog show a real percentage
		// instead of a spinner; CDN redirect targets usually send it.
		long long total = 0;
		{
			wchar_t lengthText[64] = {0};
			DWORD lengthSize = (DWORD)sizeof(lengthText);
			if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
				WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize, nullptr))
				total = _wtoi64(lengthText);
		}

		char buffer[16384];
		DWORD written = 0;
		long long received = 0;
		bool good = true;
		bool readOk = true;
		// m094: a 1.7 MB package used to post ~110 cross-thread updates, and a
		// big one thousands. One emission per 100 ms (plus the final one) keeps
		// the UI event queue short without making the bar look stuck.
		unsigned long long lastEmit = 0;
		auto emitProgress = [&](bool aForce) {
			if (!aProgress)
				return;
			const unsigned long long now = GetTickCount64();
			if (!aForce && now - lastEmit < 100)
				return;
			lastEmit = now;
			aProgress(received, total);
		};
		emitProgress(true);
		for (;;)
		{
			DWORD chunk = 0;
			// TRUE + 0 bytes = the body ended normally. FALSE = the link
			// broke; treating that as success used to deliver half a zip.
			if (!WinHttpReadData(request, buffer, (DWORD)sizeof(buffer), &chunk))
			{
				readOk = false;
				break;
			}
			if (chunk == 0)
				break;
			if (!WriteFile(file, buffer, chunk, &written, nullptr) ||
				written != chunk)
			{
				good = false;
				break;
			}
			received += (long long)chunk;
			emitProgress(false);
		}
		CloseHandle(file);
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);

		if (!good)
		{
			aError = "write failed";
			DeleteFileW(aSavePath.c_str());
			return false;
		}
		if (!readOk)
		{
			aError = "download interrupted";
			DeleteFileW(aSavePath.c_str());
			return false;
		}
		// A clean EOF is not proof of a complete file: when the server sent a
		// length, the received size must match it. Chunked responses carry no
		// length (total == 0) and rely on the EOF above.
		if (total > 0 && received != total)
		{
			aError = "truncated download (" + std::to_string(received) + " of " +
				std::to_string(total) + " bytes)";
			DeleteFileW(aSavePath.c_str());
			return false;
		}
		emitProgress(true); // the bar must end at 100%, not at the last tick
		return true;
	}
	aError = "too many redirects";
	return false;
}

// Best effort: a read-only install directory must not break the check itself.
void WriteCacheManifest(const std::string &aRawJson)
{
	const std::wstring path = Utf8ToWide(UpdateCachePathUtf8());
	if (path.empty())
		return;
	CreateDirectoryW(AppTempRoot().c_str(), nullptr); // first run: the folder may not exist yet
	std::string error;
	WriteTextDocument(path, aRawJson, TextEncoding::Utf8NoBom, false, nullptr, error);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool FetchText(const std::string &aUrl, int aTimeoutMs, std::string &aOut,
	std::string &aError)
{
	return HttpGetBody(aUrl, aTimeoutMs, aOut, aError);
}

std::string GetExeDirUtf8()
{
	wchar_t exe[MAX_PATH] = {0};
	if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
		return std::string();
	wchar_t *slash = wcsrchr(exe, L'\\');
	if (slash)
		*(slash + 1) = 0; // keep the trailing backslash
	return WideToUtf8(std::wstring(exe));
}

int CompareVersions(const std::string &aCurrent, const std::string &aLatest)
{
	int current[3];
	int latest[3];
	ParseVersionPart(aCurrent, current);
	ParseVersionPart(aLatest, latest);
	for (int i = 0; i < 3; ++i)
	{
		if (latest[i] != current[i])
			return latest[i] > current[i] ? 1 : -1;
	}
	return 0;
}

std::string UpdateCachePathUtf8()
{
	// m177: a cache is scratch, so it lives under temp\ with the rest of it.
	return GetExeDirUtf8() + "temp\\update.cache.json";
}

bool ReadCachedManifest(UpdateManifest &aOut, long long &aAgeSeconds)
{
	aAgeSeconds = -1;
	std::string raw;
	if (!ReadWholeFile(UpdateCachePathUtf8(), raw))
		return false;
	UpdateManifest manifest;
	if (!ParseManifest(raw, manifest))
		return false;
	aAgeSeconds = FileAgeSeconds(UpdateCachePathUtf8());
	aOut = std::move(manifest);
	return true;
}

UpdateCheckResult CheckUpdate(const std::string &aCurrentVersion,
	const std::string &aUrlGitee, const std::string &aUrlGithub, int aSource)
{
	UpdateCheckResult result;
	const std::string urls[2] = { aUrlGitee, aUrlGithub };
	const int order[2] = { aSource == 1 ? 1 : 0, aSource == 1 ? 0 : 1 };
	std::string lastError = "没有可用的更新清单地址";

	for (int i = 0; i < 2; ++i)
	{
		const int index = order[i];
		if (urls[index].empty())
			continue; // an unset source is skipped, not tried
		std::string body, error;
		if (!HttpGetBody(urls[index], 10000, body, error))
		{
			lastError = error;
			continue;
		}
		UpdateManifest manifest;
		if (!ParseManifest(body, manifest))
		{
			lastError = "更新清单无法解析";
			continue;
		}
		WriteCacheManifest(body);
		result.online = true;
		result.sourceUsed = index;
		result.manifest = std::move(manifest);
		result.hasUpdate = CompareVersions(aCurrentVersion, result.manifest.version) > 0;
		return result;
	}

	// Neither source answered. The cache may still say "a newer version was
	// seen last time", but it must never be reported as "up to date": after an
	// upgrade, the cache in the install directory was written by the PREVIOUS
	// build.
	UpdateManifest cached;
	long long age = -1;
	if (ReadCachedManifest(cached, age))
	{
		result.fromCache = true;
		result.cacheAgeSec = age;
		result.manifest = std::move(cached);
		result.hasUpdate = CompareVersions(aCurrentVersion, result.manifest.version) > 0;
		return result;
	}
	result.error = lastError;
	return result;
}

bool DownloadFile(const std::string &aUrl, const std::string &aSavePathUtf8,
	DownloadProgress aProgress)
{
	const std::wstring path = Utf8ToWide(aSavePathUtf8);
	if (path.empty())
		return false;
	std::string error;
	return HttpDownload(aUrl, path, 60000, error, aProgress);
}

// Generates the updater batch. Proven recipe:
//   * CRLF line endings - cmd parses LF-only files unreliably;
//   * UTF-8 BOM + chcp 65001, so Chinese install paths survive;
//   * every step appends to ahkdeck_update.log, and the log (like the zip) is
//     kept only when something failed, so a failure leaves evidence and a
//     retry does not have to re-download.
bool WriteUpdateBat(const std::string &aExeDirUtf8,
	const std::string &aZipPathUtf8, const std::string &aBatPathUtf8)
{
	const std::string app = aExeDirUtf8; // ends with a trailing separator
	const std::string zip = aZipPathUtf8;

	// A quoted robocopy destination must NOT end with a separator: inside
	// "...\" the trailing backslash escapes the closing quote, so cmd swallows
	// the rest of the line into the path and robocopy copies nothing.
	std::string appNoSlash = app;
	while (!appNoSlash.empty() &&
		(appNoSlash.back() == '\\' || appNoSlash.back() == '/'))
		appNoSlash.pop_back();

	auto line = [](const std::string &text) { return text + "\r\n"; };

	std::string bat;
	bat += line("@echo off");
	bat += line("chcp 65001 >nul");
	bat += line("setlocal enabledelayedexpansion");
	bat += line("set \"APP=" + app + "\"");
	bat += line("set \"APPD=" + appNoSlash + "\"");
	bat += line("set \"ZIP=" + zip + "\"");
	// m177: the update's own scratch is not part of the install. Extract dir,
	// log and rollback copy all live under temp\, so a finished update leaves
	// the program folder with the same five names it had before.
	bat += line("set \"EXT=" + app + "temp\\update\\extracted\"");
	bat += line("set \"LOG=" + app + "temp\\logs\\update.log\"");
	bat += line("mkdir \"!APPD!\\temp\\logs\" 2>nul");
	bat += line("set \"FAIL=0\"");
	bat += line(">>\"!LOG!\" echo =============================================");
	bat += line(">>\"!LOG!\" echo AHKDeck Updater %DATE% %TIME%");
	bat += line(">>\"!LOG!\" echo APP=!APP!");
	bat += line(">>\"!LOG!\" echo ZIP=!ZIP!");

	// Close this installation's process only. Scope by ExecutablePath, not by
	// image name: taskkill /im AHKDeck.exe would kill a second copy installed
	// elsewhere, and the retry loop must be bounded.
	bat += line("echo [1/6] Closing AHKDeck...");
	bat += line(">>\"!LOG!\" echo [1/6] closing");
	bat += line("set \"TRIES=0\"");
	bat += line(":waitkill");
	bat += line("set /a TRIES+=1");
	bat += line("if !TRIES! GTR 10 goto killfail");
	bat += line("powershell -NoProfile -ExecutionPolicy Bypass -Command \"$ps=@(Get-CimInstance Win32_Process | Where-Object { @('AHKDeck.exe') -contains $_.Name -and $_.ExecutablePath -and ([IO.Path]::GetDirectoryName($_.ExecutablePath) -ieq $env:APPD) }); foreach($p in $ps){ Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }; if($ps.Count -gt 0){ exit 1 } else { exit 0 }\"");
	bat += line("if errorlevel 1 (timeout /t 1 /nobreak >nul & goto waitkill)");
	bat += line("goto killed");
	bat += line(":killfail");
	bat += line(">>\"!LOG!\" echo could not close every process of this installation after 10 tries");
	bat += line("set \"FAIL=1\"");
	bat += line(":killed");
	bat += line("timeout /t 3 /nobreak >nul"); // let the file lock settle
	bat += line("if exist \"!APP!AHKDeck.exe\" (>>\"!LOG!\" echo exe-present) else (>>\"!LOG!\" echo exe-MISSING)");

	// m094: keep a rollback copy of the two things an update replaces. The exe
	// is the file that makes the app start at all, and Scripts/ is what the
	// user sees; a half-copied install with a broken exe is a bricked machine.
	bat += line("echo Backing up the current build...");
	bat += line(">>\"!LOG!\" echo backup");
	bat += line("if exist \"!APPD!\\temp\\backups\\update\" rmdir /s /q \"!APPD!\\temp\\backups\\update\" 2>nul");
	bat += line("mkdir \"!APPD!\\temp\\backups\\update\" 2>nul");
	bat += line("if exist \"!APP!AHKDeck.exe\" copy /y \"!APP!AHKDeck.exe\" \"!APPD!\\temp\\backups\\update\\AHKDeck.exe\" >nul 2>&1");
	bat += line("if exist \"!APPD!\\Scripts\" robocopy \"!APPD!\\Scripts\" \"!APPD!\\temp\\backups\\update\\Scripts\" /e /njh /njs /ndl /nc /ns /np /r:1 /w:1 >>\"!LOG!\" 2>&1");

	// Extract, then locate the payload (a zip may wrap everything in one or
	// two folders).
	bat += line("echo [2/6] Extracting...");
	bat += line(">>\"!LOG!\" echo [2/6] extracting");
	bat += line("if exist \"!EXT!\" rmdir /s /q \"!EXT!\" 2>nul");
	bat += line("mkdir \"!EXT!\" 2>nul");
	bat += line("powershell -NoProfile -Command \"Expand-Archive -LiteralPath '!ZIP!' -DestinationPath '!EXT!' -Force\" >>\"!LOG!\" 2>&1");
	bat += line("if errorlevel 1 set \"FAIL=1\"");
	bat += line(">>\"!LOG!\" echo expand-error=!ERRORLEVEL!");
	bat += line("echo [3/6] Locating payload...");
	bat += line("set \"SRC=!EXT!\"");
	bat += line("if not exist \"!SRC!\\AHKDeck.exe\" (");
	bat += line("  for /d %%d in (\"!EXT!\\*\") do (");
	bat += line("    if exist \"%%d\\AHKDeck.exe\" set \"SRC=%%d\"");
	bat += line("  )");
	bat += line(")");
	bat += line("if not exist \"!SRC!\\AHKDeck.exe\" (");
	bat += line("  for /d %%d in (\"!EXT!\\*\") do (");
	bat += line("    for /d %%e in (\"%%d\\*\") do (");
	bat += line("      if exist \"%%e\\AHKDeck.exe\" set \"SRC=%%e\"");
	bat += line("    )");
	bat += line("  )");
	bat += line(")");
	bat += line(">>\"!LOG!\" echo SRC=!SRC!");
	bat += line("if not exist \"!SRC!\\AHKDeck.exe\" set \"FAIL=1\"");

	// Overwrite the install directory; the exe gets a second, direct copy
	// because it is the one file that may still be locked.
	bat += line("echo [4/6] Copying files...");
	bat += line(">>\"!LOG!\" echo [4/6] robocopy");
	bat += line("robocopy \"!SRC!\" \"!APPD!\" /e /njh /njs /ndl /nc /ns /np /is /it /r:5 /w:2 >>\"!LOG!\" 2>&1");
	bat += line("set \"RC=!ERRORLEVEL!\"");
	bat += line(">>\"!LOG!\" echo robocopy-exit=!RC!");
	bat += line("if !RC! GEQ 8 set \"FAIL=1\"");
	bat += line("if exist \"!SRC!\\AHKDeck.exe\" (");
	bat += line("  copy /y \"!SRC!\\AHKDeck.exe\" \"!APP!AHKDeck.exe\" >>\"!LOG!\" 2>&1");
	bat += line("  if errorlevel 1 set \"FAIL=1\"");
	bat += line(")");

	// m094: a failed copy restores what we backed up. Never leave a half-new,
	// half-old install: the stub exe plus the old Scripts is known to start.
	bat += line("if \"!FAIL!\"==\"1\" (");
	bat += line("  echo Rolling back...");
	bat += line("  >>\"!LOG!\" echo rollback");
	bat += line("  if exist \"!APPD!\\temp\\backups\\update\\AHKDeck.exe\" copy /y \"!APPD!\\temp\\backups\\update\\AHKDeck.exe\" \"!APP!AHKDeck.exe\" >>\"!LOG!\" 2>&1");
	bat += line("  if exist \"!APPD!\\temp\\backups\\update\\Scripts\" robocopy \"!APPD!\\temp\\backups\\update\\Scripts\" \"!APPD!\\Scripts\" /e /njh /njs /ndl /nc /ns /np /r:1 /w:1 >>\"!LOG!\" 2>&1");
	bat += line(")");

	// Clean up ONLY on success: a failed robocopy keeps the package so a retry
	// does not re-download it before failing the same way.
	bat += line("echo [5/6] Cleaning up...");
	bat += line("if \"!FAIL!\"==\"0\" (");
	bat += line("  rmdir /s /q \"!EXT!\" 2>nul");
	bat += line("  del /q \"!ZIP!\" 2>nul");
	bat += line(") else (");
	bat += line("  >>\"!LOG!\" echo kept for retry: !ZIP!");
	bat += line("  echo Update failed - the package was kept for a retry.");
	bat += line(")");

	bat += line("echo [6/6] Starting AHKDeck...");
	bat += line(">>\"!LOG!\" echo [6/6] start");
	bat += line("start \"\" \"!APP!AHKDeck.exe\"");
	bat += line(">>\"!LOG!\" echo start-error=!ERRORLEVEL!");
	bat += line(">>\"!LOG!\" echo FAIL=!FAIL!");
	bat += line("if \"!FAIL!\"==\"0\" del /q \"!LOG!\" 2>nul");
	bat += line("del \"%~f0\"");
	bat += line("exit");

	// UTF-8 with an explicit BOM so Chinese paths survive chcp 65001.
	const std::string out = "\xEF\xBB\xBF" + bat;
	const std::wstring path = Utf8ToWide(aBatPathUtf8);
	if (path.empty())
		return false;
	FILE *file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
		return false;
	const size_t wrote = std::fwrite(out.data(), 1, out.size(), file);
	const int error = std::ferror(file);
	std::fclose(file);
	return error == 0 && wrote == out.size();
}

void LaunchUpdater(const std::string &aBatPathUtf8)
{
	const std::wstring path = Utf8ToWide(aBatPathUtf8);
	if (path.empty())
		return;
	ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_HIDE);
}

} // namespace ahkdeck

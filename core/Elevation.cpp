#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Elevation.h"

namespace ahkdeck
{

bool IsElevated()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	DWORD elevated = 0;
	DWORD size = 0;
	const BOOL ok = GetTokenInformation(token, TokenElevation, &elevated,
		sizeof(elevated), &size);
	CloseHandle(token);
	return ok && elevated != 0;
}

} // namespace ahkdeck

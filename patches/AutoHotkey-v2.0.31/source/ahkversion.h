
#define AHK_NAME "AutoHotkey"
#define T_AHK_NAME _T(AHK_NAME)

#ifndef RAW_AHK_VERSION
	// Fallback version string for when `git describe` fails during build.
	// AHKDeck (m192): the source tree we build from has no .git, so git describe
	// never runs and this fallback would report "2.0" - pin the real version.
	#define RAW_AHK_VERSION "2.0.31"
#endif
#ifdef RC_INVOKED
	#define AHK_VERSION RAW_AHK_VERSION
	#ifndef AHK_VERSION_N
	#define AHK_VERSION_N 2,0,31,0
	#endif
#else
	// Use global variables rather than macros so only ahkversion.cpp needs to be recompiled
	// whenever GITDESC changes.
	extern LPSTR AHK_VERSION;
	extern LPTSTR T_AHK_VERSION;
	extern LPTSTR T_AHK_NAME_VERSION;
#endif

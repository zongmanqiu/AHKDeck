
#define AHK_NAME "AutoHotkey"
#define T_AHK_NAME _T(AHK_NAME)

#ifndef RAW_AHK_VERSION
	// Fallback version string for when `git describe` fails during build.
	// PATCHED (AHKDeck): the pinned source archive has no .git, so upstream's
	// "2.0" fallback is what every build of ours would report - and it lies
	// about the kernel A_AhkVersion / error dialogs / #Requires diagnostics
	// compare against. deps.txt pins this tree to v2.0.25, so state it.
	#define RAW_AHK_VERSION "2.0.25"
#endif
#ifdef RC_INVOKED
	#define AHK_VERSION RAW_AHK_VERSION
	#ifndef AHK_VERSION_N
	// PATCHED (AHKDeck): file properties showed 2.0.0.0 with the upstream default.
	#define AHK_VERSION_N 2,0,25,0
	#endif
#else
	// Use global variables rather than macros so only ahkversion.cpp needs to be recompiled
	// whenever GITDESC changes.
	extern LPSTR AHK_VERSION;
	extern LPTSTR T_AHK_VERSION;
	extern LPTSTR T_AHK_NAME_VERSION;
#endif

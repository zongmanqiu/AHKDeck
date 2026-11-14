// core/Version.h - the version has ONE source: project(AHKDeck VERSION ...)
// in CMakeLists.txt. Its three numbers arrive here as compile definitions
// (AHKDECK_VERSION_MAJOR/MINOR/PATCH, PUBLIC on the core target), so a bump
// touches CMakeLists.txt only - the .rc and VersionString() follow, and
// test_version.cpp derives its expectation from these constants.

#pragma once

namespace ahkdeck {

constexpr int kVersionMajor = AHKDECK_VERSION_MAJOR;
constexpr int kVersionMinor = AHKDECK_VERSION_MINOR;
constexpr int kVersionPatch = AHKDECK_VERSION_PATCH;

// "major.minor.patch" - built once, stable pointer, no allocation per call.
const char* VersionString();

// m201: the OFFICIAL AUTOHOTKEY KERNEL version, one parameter for every place
// the product names it. This is NOT the AHKDeck product version above - that
// one belongs to CMakeLists.txt. Both are single-source, but they move on
// different schedules (a kernel upgrade does not bump the deck).
//
// Changing this one line updates the status bar, the About box and the MCP
// help text. It must also match the upstream tarball tag in build/deps.txt,
// RAW_AHK_VERSION in patches/AutoHotkey-v2.0.31/source/ahkversion.h and the
// ProductVersion of the shipped AutoHotkey64.exe - ctest kernel_version
// asserts all four, so the parameter can never quietly drift from the binary.
constexpr const char *kKernelVersion = "2.0.31";

// Stable pointer, same lifetime as VersionString().
const char* KernelVersionString();

} // namespace ahkdeck

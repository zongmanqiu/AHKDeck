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

} // namespace ahkdeck

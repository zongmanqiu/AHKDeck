// core/Version.h - single source of truth for the product version.
// The CMake project() version and this header must agree; test_version.cpp
// pins the string so a bump that only touches one side fails the build.

#pragma once

namespace ahkdeck {

constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

// "0.1.0" - stable pointer, no allocation.
const char* VersionString();

} // namespace ahkdeck

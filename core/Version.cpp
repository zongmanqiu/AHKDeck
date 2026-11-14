// core/Version.cpp - see core/Version.h.

#include "core/Version.h"

#include <cstdio>
#include <string>

namespace ahkdeck {

const char* VersionString()
{
    // m094: derived, never typed by hand. The three constants come from
    // CMakeLists.txt's project(VERSION), so this string cannot drift from the
    // exe resource or from what the updater compares.
    static const std::string text = [] {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%d.%d.%d",
                      kVersionMajor, kVersionMinor, kVersionPatch);
        return std::string(buffer);
    }();
    return text.c_str();
}

const char* KernelVersionString()
{
    // m201: a plain constant is enough here (no string building). The function
    // keeps the call sites independent of how the constant is stored, and lets
    // test_version assert the shape of the single source.
    return kKernelVersion;
}

} // namespace ahkdeck

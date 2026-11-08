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

} // namespace ahkdeck

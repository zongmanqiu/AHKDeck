// test/test_version.cpp - pins the version contract.
// m094: the numbers now come from CMakeLists.txt's project(VERSION) through
// compile definitions, so this test no longer hardcodes them: it checks that
// VersionString() is exactly what the Constants say, which is the only thing
// that can still drift (a hand-typed string).
//
// Style note: asserts go through Expect(), not through a do/while(0) macro -
// a macro's `if (constant-condition)` trips C4127 under our /W4 /WX gate
// (caught by 4_build on day one), and a helper function is cleaner anyway.

#include "core/Version.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

// Records one expectation. Returns the failure count it added (0 or 1).
int Expect(bool ok, const char* what, const char* file, int line)
{
    if (!ok) {
        std::printf("FAIL %s:%d: %s\n", file, line, what);
        ++g_failures;
    }
    return ok ? 0 : 1;
}

} // namespace

int main()
{
    const char* file = __FILE__;

    Expect(ahkdeck::kVersionMajor >= 0, "kVersionMajor >= 0", file, __LINE__);
    Expect(ahkdeck::kVersionMinor >= 0, "kVersionMinor >= 0", file, __LINE__);
    Expect(ahkdeck::kVersionPatch >= 0, "kVersionPatch >= 0", file, __LINE__);

    const std::string expected = std::to_string(ahkdeck::kVersionMajor) + "." +
                                 std::to_string(ahkdeck::kVersionMinor) + "." +
                                 std::to_string(ahkdeck::kVersionPatch);
    Expect(std::strcmp(ahkdeck::VersionString(), expected.c_str()) == 0,
           "VersionString() == major.minor.patch", file, __LINE__);

    if (g_failures == 0) {
        std::printf("test_version: all checks passed (%s)\n", ahkdeck::VersionString());
        return 0;
    }
    std::printf("test_version: %d check(s) failed\n", g_failures);
    return 1;
}

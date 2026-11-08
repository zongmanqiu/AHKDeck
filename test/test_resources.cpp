// test/test_resources.cpp - pins the arithmetic behind the status line (m107).
//
// Only the pure half is tested here: the CPU percentage is two GetProcessTimes
// readings divided by the wall clock and the core count, and the formatter is
// string work. The Win32 side (psapi / Toolhelp / PDH) is proved by the stats
// gate against the shipped exe, which also proves the line really comes out of
// a process rather than out of these constants.
//
// Style note: asserts go through Expect(), not a do/while(0) macro - a macro's
// `if (constant-condition)` trips C4127 under our /W4 /WX gate.

#include "core/ResourceStats.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

int Expect(bool ok, const char* what, const char* file, int line)
{
    if (!ok) {
        std::printf("FAIL %s:%d: %s\n", file, line, what);
        ++g_failures;
    }
    return ok ? 0 : 1;
}

// 1 second in 100 ns units, the unit GetProcessTimes hands out.
constexpr unsigned long long kOneSecond = 10000000ULL;

} // namespace

int main()
{
    const char* file = __FILE__;
    using ahkdeck::CpuPercent;
    using ahkdeck::FormatBytes;
    using ahkdeck::FormatResources;
    using ahkdeck::ResourceSample;

    // --- CPU percentage ---------------------------------------------------
    // Half a second of CPU over one second of wall on a single core = 50%.
    Expect(CpuPercent(0, kOneSecond, kOneSecond / 2, 2 * kOneSecond, 1) == 50.0,
           "one core, 0.5s CPU over 1s = 50 percent", file, __LINE__);
    // The same CPU time spread over four cores is a quarter of the machine.
    Expect(CpuPercent(0, 0, kOneSecond, 2 * kOneSecond, 4) == 12.5,
           "four cores, 1s CPU over 2s = 12.5 percent", file, __LINE__);
    // A counter that did not advance is 0%, not a division by zero.
    Expect(CpuPercent(0, kOneSecond, 2 * kOneSecond, kOneSecond, 1) == 0.0,
           "no wall time between samples is 0 percent", file, __LINE__);
    Expect(CpuPercent(kOneSecond, kOneSecond, kOneSecond, 2 * kOneSecond, 1) == 0.0,
           "no CPU time between samples is 0 percent", file, __LINE__);
    // More CPU than wall time (impossible on one core, possible on many) is
    // clamped: the status bar must never read "1200%".
    Expect(CpuPercent(0, 0, kOneSecond, kOneSecond / 10, 1) == 100.0,
           "an impossible ratio clamps to 100 percent", file, __LINE__);
    // A zero core count is treated as one instead of dividing by zero.
    Expect(CpuPercent(0, 0, kOneSecond, 2 * kOneSecond, 0) == 50.0,
           "zero cores behaves as one core", file, __LINE__);

    // --- byte formatting --------------------------------------------------
    Expect(FormatBytes(900) == "900B", "bytes below a kilobyte keep the B unit",
           file, __LINE__);
    Expect(FormatBytes(2048) == "2KB", "kilobytes have no decimals", file, __LINE__);
    Expect(FormatBytes(52ULL * 1024 * 1024) == "52MB", "megabytes have no decimals",
           file, __LINE__);
    Expect(FormatBytes(3ULL * 1024 * 1024 * 1024 / 2) == "1.5GB",
           "gigabytes keep one decimal", file, __LINE__);

    // --- the display line -------------------------------------------------
    ResourceSample sample;
    sample.cpuPercent = 3.4;
    sample.workingSetBytes = 52ULL * 1024 * 1024;
    sample.privateWorkingSetBytes = 52ULL * 1024 * 1024;
    sample.threadCount = 12;
    sample.handleCount = 231;
    sample.gpuPercent = 1.0;
    const std::string withGpu = FormatResources(sample);
    Expect(withGpu.find("CPU 3%") != std::string::npos, "CPU is rounded to a whole percent",
           file, __LINE__);
    Expect(withGpu.find("内存 52MB") != std::string::npos, "memory uses the MB form",
           file, __LINE__);
    // m107: threads and handles stay in the --stats JSON, never on the status
    // line - the line keeps only what a user reads directly.
    Expect(withGpu.find("线程") == std::string::npos,
           "the status line does not carry the thread count", file, __LINE__);
    Expect(withGpu.find("句柄") == std::string::npos,
           "the status line does not carry the handle count", file, __LINE__);
    Expect(withGpu.find("GPU 1%") != std::string::npos, "a GPU reading is shown",
           file, __LINE__);

    // Task Manager's Memory column is the private working set, so the line
    // must print 18MB - not the 34MB total that includes shared DLL pages.
    sample.workingSetBytes = 34ULL * 1024 * 1024;
    sample.privateWorkingSetBytes = 18ULL * 1024 * 1024;
    const std::string privateOnly = FormatResources(sample);
    Expect(privateOnly.find("内存 18MB") != std::string::npos,
           "the line shows the private working set, matching Task Manager", file, __LINE__);
    Expect(privateOnly.find("34MB") == std::string::npos,
           "the line never prints the shared-page total", file, __LINE__);

    sample.gpuPercent = -1.0;
    const std::string withoutGpu = FormatResources(sample);
    Expect(withoutGpu.find("GPU") == std::string::npos,
           "an unavailable GPU counter is omitted, not printed as a dash", file, __LINE__);

    if (g_failures == 0) {
        std::printf("test_resources: all checks passed (\"%s\")\n", withGpu.c_str());
        return 0;
    }
    std::printf("test_resources: %d check(s) failed\n", g_failures);
    return 1;
}

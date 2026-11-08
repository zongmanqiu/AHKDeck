// test/test_housekeeping.cpp - pins the startup cleanup rules (m109).
//
// Two halves: the JSON knob and the time arithmetic are pure, and the actual
// delete pass is exercised on a throwaway directory under %TEMP% (old file
// removed, recent file kept, empty subdirectory removed, disabled setting
// removes nothing). The shipping gate repeats it against the shipped exe, which
// also proves the deck calls this at startup.

#include "core/Housekeeping.h"
#include "core/Json.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

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

constexpr std::uint64_t kDay100ns = 864000000000ULL;

std::uint64_t NowTicks()
{
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(now.dwLowDateTime);
}

std::uint64_t FileTicks(const std::wstring& aPath)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(aPath.c_str(), GetFileExInfoStandard, &data))
        return 0;
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(data.ftLastWriteTime.dwLowDateTime);
}

bool Exists(const std::wstring& aPath)
{
    return GetFileAttributesW(aPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Creates a file whose last-write time is aTicks.
void MakeFile(const std::wstring& aPath, std::uint64_t aTicks)
{
    const HANDLE file = CreateFileW(aPath.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    FILETIME ticks = {};
    ticks.dwLowDateTime = static_cast<DWORD>(aTicks & 0xFFFFFFFFULL);
    ticks.dwHighDateTime = static_cast<DWORD>(aTicks >> 32);
    SetFileTime(file, nullptr, nullptr, &ticks);
    CloseHandle(file);
}

std::wstring TestRoot()
{
    wchar_t temp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"ahkdeck-housekeeping-test";
    // Leave no leftovers from an earlier run.
    const std::wstring stale[] = {
        root + L"\\old.txt", root + L"\\new.txt",
        root + L"\\old-dir\\old.txt", root + L"\\new-dir\\new.txt"
    };
    for (const std::wstring& path : stale)
        DeleteFileW(path.c_str());
    RemoveDirectoryW((root + L"\\old-dir").c_str());
    RemoveDirectoryW((root + L"\\new-dir").c_str());
    RemoveDirectoryW(root.c_str());
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

} // namespace

int main()
{
    const char* file = __FILE__;
    using ahkdeck::CutoffTicks;
    using ahkdeck::IsOlderThan;
    using ahkdeck::PruneOlderThan;
    using ahkdeck::RetentionDaysFromJson;
    using ahkdeck::kHousekeepingDefaultDays;

    // --- the settings knob ---------------------------------------------------
    ahkdeck::Json empty = ahkdeck::Json::MakeObject();
    Expect(RetentionDaysFromJson(empty) == kHousekeepingDefaultDays,
        "missing housekeeping section means the 30-day default", file, __LINE__);

    ahkdeck::Json custom = ahkdeck::Json::MakeObject();
    // Json::Set returns the object itself, not the member, so the table is built
    // on the side and then stored (same shape the deck uses when it adds the key).
    auto setDays = [](ahkdeck::Json &aRoot, ahkdeck::Json aDays) {
        ahkdeck::Json table = ahkdeck::Json::MakeObject();
        table.Set("max_age_days", std::move(aDays));
        aRoot.Set("housekeeping", std::move(table));
    };
    setDays(custom, ahkdeck::Json((double)45));
    Expect(RetentionDaysFromJson(custom) == 45, "an explicit 45 is honoured", file, __LINE__);

    setDays(custom, ahkdeck::Json((double)0));
    Expect(RetentionDaysFromJson(custom) == 0, "0 turns the cleanup off", file, __LINE__);

    setDays(custom, ahkdeck::Json((double)-5));
    Expect(RetentionDaysFromJson(custom) == 0, "a negative value means off", file, __LINE__);

    setDays(custom, ahkdeck::Json("soon"));
    Expect(RetentionDaysFromJson(custom) == kHousekeepingDefaultDays,
        "a non-numeric value falls back to the default", file, __LINE__);

    // --- the time arithmetic -------------------------------------------------
    const std::uint64_t now = NowTicks();
    Expect(CutoffTicks(now, 30) == now - 30 * kDay100ns, "30 days before now", file, __LINE__);
    Expect(CutoffTicks(now, 0) == 0, "disabled = cutoff at the epoch", file, __LINE__);
    Expect(CutoffTicks(now, -1) == 0, "negative = cutoff at the epoch", file, __LINE__);
    Expect(CutoffTicks(10, 30) == 0, "an earlier clock cannot underflow", file, __LINE__);

    const std::uint64_t cutoff = CutoffTicks(now, 30);
    Expect(!IsOlderThan(cutoff, cutoff), "exactly N days old is kept", file, __LINE__);
    Expect(IsOlderThan(cutoff - 1, cutoff), "one tick older is expired", file, __LINE__);

    // --- the delete pass -----------------------------------------------------
    const std::wstring root = TestRoot();
    const std::uint64_t oldTicks = now - 40 * kDay100ns;
    const std::uint64_t newTicks = now - 1 * kDay100ns;
    MakeFile(root + L"\\old.txt", oldTicks);
    MakeFile(root + L"\\new.txt", newTicks);
    CreateDirectoryW((root + L"\\old-dir").c_str(), nullptr);
    CreateDirectoryW((root + L"\\new-dir").c_str(), nullptr);
    MakeFile(root + L"\\old-dir\\old.txt", oldTicks);
    MakeFile(root + L"\\new-dir\\new.txt", newTicks);

    std::string error;
    const int removed = PruneOlderThan(root, 30, now, error);
    Expect(removed == 2, "two old files deleted", file, __LINE__);
    Expect(error.empty(), "no failure when nothing is locked", file, __LINE__);
    Expect(!Exists(root + L"\\old.txt"), "the old file is gone", file, __LINE__);
    Expect(Exists(root + L"\\new.txt"), "the recent file stays", file, __LINE__);
    Expect(!Exists(root + L"\\old-dir"), "an empty old subdirectory is gone", file, __LINE__);
    Expect(Exists(root + L"\\new-dir\\new.txt"), "a non-empty recent subdirectory stays",
        file, __LINE__);
    Expect(Exists(root), "the root directory itself is never removed", file, __LINE__);

    // The same call with the cleanup disabled must not touch even the old file.
    MakeFile(root + L"\\old.txt", oldTicks);
    Expect(FileTicks(root + L"\\old.txt") < CutoffTicks(now, 30), "fixture is past the cutoff",
        file, __LINE__);
    Expect(PruneOlderThan(root, 0, now, error) == 0, "0 days deletes nothing", file, __LINE__);
    Expect(Exists(root + L"\\old.txt"), "the old file survives a disabled run", file, __LINE__);

    // Cleanup so a repeated run starts from a clean tree.
    DeleteFileW((root + L"\\old.txt").c_str());
    DeleteFileW((root + L"\\new.txt").c_str());
    DeleteFileW((root + L"\\new-dir\\new.txt").c_str());
    RemoveDirectoryW((root + L"\\new-dir").c_str());
    RemoveDirectoryW(root.c_str());

    if (g_failures > 0) {
        std::printf("housekeeping: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("housekeeping: all checks passed\n");
    return 0;
}

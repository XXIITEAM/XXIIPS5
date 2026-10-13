#include "common/diagnostics/hleTrace.h"
#include "common/diagnostics/debugAudio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>

namespace {
void Require(bool condition, const char* title) {
    if (!condition) {
        std::fprintf(stderr, "HleTraceTests: FAILED %s\n", title);
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc == 2 && std::strcmp(argv[1], "--wait-snapshot-child") == 0) {
        // A real child process, not a mocked shared-memory writer. The
        // parent snapshots this in-flight wait while its child is asleep.
        if (!Common::HleTrace::Enabled() ||
            !Common::HleWaitSnapshot::RuntimeTable()) return 31;
        Common::HleTrace::Span blocked("sync_wait32", "infinite");
        Sleep(2200);
        blocked.Finish(0);
        return 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    using namespace Common::HleTrace;
    using Common::DebugAudio::ParsePercent;
    Require(ParsePercent("10") == 10 && ParsePercent("0") == 0 &&
            ParsePercent("100") == 100 && ParsePercent("005") == 5,
            "debug audio percent valid range");
    Require(ParsePercent(nullptr) == 100 && ParsePercent("") == 100 &&
            ParsePercent("-1") == 100 && ParsePercent("101") == 100 &&
            ParsePercent("1000") == 100 && ParsePercent("10%") == 100,
            "invalid or absent output limit leaves normal gain");
    Require(SampleCall(1) && SampleCall(96) && !SampleCall(97), "warning initial budget");
    Require(SampleCall(512) && !SampleCall(513), "warning stride");
    Require(SampleCall(1024, 0) && !SampleCall(1025, 0), "error sampling");
    Require(SampleCall(512, 2) && !SampleCall(513, 2), "info sampling");
    Require(SampleCall(4096, 3) && SampleCall(4128, 3) &&
            !SampleCall(4129, 3), "trace sampling");
    Require(!SampleCall(0), "zero sequence never sampled");
    Require(SampleOptionalLookup(24, 1) && !SampleOptionalLookup(25, 1) &&
            SampleOptionalLookup(128, 1), "warning optional-lookup budget");
    Require(SampleOptionalLookup(8, 0) && !SampleOptionalLookup(9, 0) &&
            SampleOptionalLookup(256, 0), "error optional-lookup budget");
    Require(SampleOptionalLookup(128, 3) && !SampleOptionalLookup(129, 3) &&
            SampleOptionalLookup(136, 3), "escalated optional-lookup budget");

    Require(SymbolFingerprint("ABCD1234+/Z") ==
            SymbolFingerprint("ABCD1234+/Z"), "deterministic symbol fingerprint");
    Require(SymbolFingerprint("ABCD1234+/Z") !=
            SymbolFingerprint("ABCD1234+/X"), "distinct symbols");
    char source[49] {};
    Require(std::strcmp(SourceBase("C:/private/code/libKernel.cpp", source),
                        "libKernel") == 0, "strip Windows path");
    Require(std::strcmp(SourceBase("/home/developer/pm4Handlers.cpp", source),
                        "pm4Handlers") == 0, "strip Unix path");
    Require(std::strcmp(SourceBase("invalid path?bad.cpp", source),
                        "invalid") == 0, "do not retain arbitrary path text");
#if defined(_WIN32)
    // Exercise the same low-frequency focus event protocol the GUI uses.
    const std::string eventName = "Local\\PSVXXII-HLE-test-" +
                                  std::to_string(GetCurrentProcessId());
    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, eventName.c_str());
    Require(event != nullptr, "create diagnostic focus event");
    Require(_putenv_s("XXII_COMPAT_HLE_FOCUS_EVENT", eventName.c_str()) == 0,
            "set transient focus name");
    for (int i = 0; i < 64; ++i)
        Require(!FocusBoostActive(), "focus initially disabled");
    Require(SetEvent(event) != 0, "request focused detail");
    bool boosted = false;
    for (int i = 0; i < 64; ++i) boosted = FocusBoostActive() || boosted;
    Require(boosted, "focus detected in bounded HLE calls");
    Require(ResetEvent(event) != 0, "cancel focused detail");
    bool active = true;
    for (int i = 0; i < 64; ++i) active = FocusBoostActive();
    Require(!active, "focus returns to idle");
    CloseHandle(event);

    // Parent and child use the exact Win32 mapping protocol of PSVXXII.
    // No game memory or thread suspension is required to observe the wait.
    const std::string mappingName = "Local\\PSVXXII-WAITS-test-" +
                                    std::to_string(GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(Common::HleWaitSnapshot::Table), mappingName.c_str());
    Require(mapping != nullptr && GetLastError() != ERROR_ALREADY_EXISTS,
            "create unique HLE wait mapping");
    auto* table = static_cast<Common::HleWaitSnapshot::Table*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                      sizeof(Common::HleWaitSnapshot::Table)));
    Require(table != nullptr, "map HLE wait snapshot");
    Common::HleWaitSnapshot::Initialize(table);
    using namespace Common::HleWaitSnapshot;
    Require(!Inspect(table, GetTickCount64()).connected,
            "mapping creation alone is not guest connection");

    auto now = GetTickCount64();
    // Regression: a dense 24-slot cluster at the allocation cursor must
    // not cause a dropped trace when hundreds of other slots are free.
    std::array<Ticket, 24> clustered {};
    for (uint32_t i = 0; i < clustered.size(); ++i) {
        clustered[i] = Enter(table, i + 1, Operation::Address32,
                             Mode::Infinite, now - 9000, i + 1);
        Require(clustered[i].slot != nullptr, "create dense wait cluster");
    }
    InterlockedExchange(&table->cursor, 0);
    auto rescued = Enter(table, 10000, Operation::Condition,
                         Mode::Infinite, now - 8000, 900);
    Require(rescued.slot != nullptr && Inspect(table, now).dropped == 0,
            "distributed probing avoids artificial slot loss");
    Leave(rescued);
    for (auto ticket : clustered) Leave(ticket);
    Initialize(table);

    std::array<Ticket, Capacity> tickets {};
    for (uint32_t i = 0; i < Capacity; ++i) {
        tickets[i] = Enter(table, i + 1, Operation::Address32, Mode::Infinite,
                           now - 2000 - i, i + 10);
        Require(tickets[i].slot != nullptr, "bounded outstanding wait slots");
    }
    auto full = Inspect(table, now);
    Require(full.outstanding == Capacity && full.count == TopWaits &&
            full.omitted == Capacity - TopWaits,
            "bounded oldest waits sorted and truncated");
    Require(full.oldest[0].ageMs >= full.oldest[1].ageMs,
            "oldest waits first");
    Require(!Enter(table, 500, Operation::Condition, Mode::Infinite,
                   now, 999).slot && Inspect(table, now).dropped == 1,
            "saturation does not block and exposes dropped slots");
    for (auto ticket : tickets) Leave(ticket);
    Require(Inspect(table, now).outstanding == 0, "released waits disappear");

    Require(_putenv_s("XXII_COMPAT_HLE_WAIT_MAP", mappingName.c_str()) == 0 &&
            _putenv_s("XXII_COMPAT_HLE_TRIAGE", "1") == 0,
            "publish snapshot mapping for child");
    char executable[MAX_PATH] {};
    Require(GetModuleFileNameA(nullptr, executable, MAX_PATH) > 0,
            "locate wait test executable");
    std::string arguments = std::string("\"") + executable +
                            "\" --wait-snapshot-child";
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child {};
    Require(CreateProcessA(nullptr, arguments.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child) != 0,
            "launch guest-style mapped wait child");
    bool sawChildWait = false;
    for (int retry = 0; retry < 100; ++retry) {
        const auto snapshot = Inspect(table, GetTickCount64());
        if (snapshot.outstanding > 0 && snapshot.count > 0) {
            sawChildWait = snapshot.oldest[0].operation == Operation::Address32 &&
                           snapshot.oldest[0].mode == Mode::Infinite;
            if (sawChildWait) break;
        }
        Sleep(20);
    }
    const auto pending = Inspect(table, GetTickCount64());
    Require(sawChildWait && pending.connected && pending.count >= 1,
            "parent sees guest-confirmed unsampled child wait");
    Require(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
            "child returned from wait");
    DWORD exitCode = ~0UL;
    Require(GetExitCodeProcess(child.hProcess, &exitCode) && exitCode == 0 &&
            Inspect(table, GetTickCount64()).outstanding == 0,
            "child release observed and exit status valid");
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    UnmapViewOfFile(table);
    CloseHandle(mapping);
#endif
    std::puts("HLE trace fingerprint, cross-process wait snapshot and sampling tests passed");
    return 0;
}
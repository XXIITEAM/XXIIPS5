#include "graphics/host_gpu/renderer/gpuResourceLifetime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Libs::Graphics::GpuTrace;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "GpuResourceLifetimeTests: FAILED %s\n", message);
        std::exit(1);
    }
}
int main() {
    ResourceLifetimeHistory trace;
    Require(trace.Count() == 0 && trace.Total() == 0, "initial empty");
    const ResourceEvent created {ResourceAction::Created, 100, 98, 0, 4096, 5, 1, 126, 1};
    const ResourceEvent queued {ResourceAction::RetireQueued, 112, 110, 112, 4096, 5, 1, 126, 1};
    const ResourceEvent executed {ResourceAction::RetireExecuted, 115, 113, 112, 4096, 5, 1, 126, 1};
    const ResourceEvent too_early {ResourceAction::RetireExecuted, 112, 111, 112, 4096, 5, 1, 126, 1};
    Require(!PrematureResourceRetire(created), "created is not retire");
    Require(!PrematureResourceRetire(queued), "queued is not executed");
    Require(!PrematureResourceRetire(executed), "completed retirement");
    Require(PrematureResourceRetire(too_early), "early destruction detected");
    trace.Push(created); trace.Push(queued); trace.Push(executed);
    Require(trace.Count() == 3 && trace.At(0).slot_generation == 1, "normal sequence");
    FILE* report = std::tmpfile();
    Require(report != nullptr, "temp report");
    trace.Dump(report, true);
    std::rewind(report);
    char data[5000] {};
    auto read = std::fread(data, 1, sizeof(data) - 1, report);
    std::fclose(report);
    Require(read > 0 && std::strstr(data, "premature_retire=0"), "valid report");
    Require(std::strstr(data, "slot=5 generation=1"), "stable slot and generation");
    trace.Push(too_early);
    report = std::tmpfile();
    Require(report != nullptr, "temp violation report");
    trace.Dump(report, true);
    std::rewind(report);
    std::memset(data, 0, sizeof(data));
    read = std::fread(data, 1, sizeof(data) - 1, report);
    std::fclose(report);
    Require(read > 0 && std::strstr(data, "premature_retire=1"), "violation in report");
    for (uint64_t i = 0; i < ResourceLifetimeHistory::Capacity + 6; ++i)
        trace.Push({ResourceAction::Created, i, i, 0, 4, uint32_t(i), 2, 5, 0});
    Require(trace.Count() == ResourceLifetimeHistory::Capacity, "bounded retention");
    Require(trace.Dropped() == 10, "lost earlier events counted");
    Require(trace.At(trace.Count() - 1).event_tick == ResourceLifetimeHistory::Capacity + 5,
            "last event retained");
    Require(std::strcmp(ResourceActionName(ResourceAction::RetireQueued), "retire_queued") == 0,
            "event name");
    std::puts("GPU resource lifetime tests passed");
}

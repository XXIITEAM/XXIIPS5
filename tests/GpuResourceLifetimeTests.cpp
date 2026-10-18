#include "graphics/host_gpu/renderer/gpuResourceLifetime.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "common/slotVector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Libs::Graphics::GpuTrace;

template <typename T>
concept HasResourceCapture = requires { &T::RecordResourceEvent; };
static_assert(HasResourceCapture<Libs::Graphics::MasterSemaphore>);

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "GpuResourceLifetimeTests: FAILED %s\n", message);
        std::exit(1);
    }
}
int main() {
    // The diagnostic observer must not turn a retired image handle into a
    // second guest crash. It must distinguish a reused slot generation too.
    Common::SlotVector<uint32_t> imageSlots;
    const auto retiredId = imageSlots.insert(42u);
    Require(imageSlots.try_get(retiredId) != nullptr &&
            *imageSlots.try_get(retiredId) == 42u, "active image slot readable");
    imageSlots.erase(retiredId);
    Require(!imageSlots.is_allocated(retiredId) &&
            imageSlots.try_get(retiredId) == nullptr,
            "retired image read is non-fatal");
    const auto reusedId = imageSlots.insert(43u);
    Require(reusedId.index == retiredId.index &&
            reusedId.generation != retiredId.generation,
            "retired image generation incremented");
    Require(imageSlots.try_get(retiredId) == nullptr &&
            imageSlots.try_get(reusedId) != nullptr &&
            *imageSlots.try_get(reusedId) == 43u,
            "stale image cannot alias replacement");
    const auto& constImageSlots = imageSlots;
    Require(constImageSlots.try_get(retiredId) == nullptr &&
            constImageSlots.try_get(Common::SlotId {}) == nullptr,
            "const lookup and unallocated id fail closed");

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
    constexpr const char* report_name = "gpu-resource-lifetime-test-report.txt";
    FILE* report = std::fopen(report_name, "w+bx");
    Require(report != nullptr, "exclusive project-local report");
    trace.Dump(report, true);
    std::rewind(report);
    char data[5000] {};
    auto read = std::fread(data, 1, sizeof(data) - 1, report);
    std::fclose(report);
    Require(std::remove(report_name) == 0, "remove project-local report");
    Require(read > 0 && std::strstr(data, "premature_retire=0"), "valid report");
    Require(std::strstr(data, "slot=5 generation=1"), "stable slot and generation");
    trace.Push(too_early);
    report = std::fopen(report_name, "w+bx");
    Require(report != nullptr, "exclusive project-local violation report");
    trace.Dump(report, true);
    std::rewind(report);
    std::memset(data, 0, sizeof(data));
    read = std::fread(data, 1, sizeof(data) - 1, report);
    std::fclose(report);
    Require(std::remove(report_name) == 0, "remove project-local violation report");
    Require(read > 0 && std::strstr(data, "premature_retire=1"), "violation in report");
    for (uint64_t i = 0; i < ResourceLifetimeHistory::Capacity + 6; ++i)
        trace.Push({ResourceAction::Created, i, i, 0, 4, uint32_t(i), 2, 5, 0});
    Require(trace.Count() == ResourceLifetimeHistory::Capacity, "bounded retention");
    Require(trace.Dropped() == 10, "lost earlier events counted");
    Require(trace.At(trace.Count() - 1).event_tick == ResourceLifetimeHistory::Capacity + 5,
            "last event retained");
    Require(std::strcmp(ResourceActionName(ResourceAction::RetireQueued), "retire_queued") == 0,
            "event name");
    std::printf("GPU resource lifetime tests passed master_bytes=%zu history_bytes=%zu\n",
                sizeof(Libs::Graphics::MasterSemaphore), sizeof(ResourceLifetimeHistory));
}

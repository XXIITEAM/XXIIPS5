#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPU_RESOURCE_LIFETIME_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPU_RESOURCE_LIFETIME_H_

#include "common/diagnostics/runtimeTelemetry.h"

#if KYTY_DIAGNOSTIC_RUNTIME
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics::GpuTrace {

// Host CPU events. "retired" means removed from image lookup, NOT necessarily
// released on the GPU. Recycle/destruction requires timeline completion.
enum class ResourceAction : uint32_t {
    Created = 1,
    RetireQueued,
    RetireExecuted,
    RetireImmediate,
};

[[nodiscard]] inline const char* ResourceActionName(ResourceAction action) noexcept {
    switch (action) {
        case ResourceAction::Created: return "created";
        case ResourceAction::RetireQueued: return "retire_queued";
        case ResourceAction::RetireExecuted: return "retire_executed";
        case ResourceAction::RetireImmediate: return "retire_immediate";
    }
    return "unknown";
}

struct ResourceEvent {
    ResourceAction action = ResourceAction::Created;
    uint64_t event_tick = 0;
    uint64_t completed_tick = 0;
    uint64_t retire_tick = 0;
    uint64_t bytes = 0;
    uint32_t slot_index = 0;
    uint32_t slot_generation = 0;
    uint32_t format = 0;
    uint32_t flags = 0;
};

[[nodiscard]] constexpr bool PrematureResourceRetire(const ResourceEvent& event) noexcept {
    return event.action == ResourceAction::RetireExecuted &&
           event.completed_tick < event.retire_tick;
}

// Snapshot-friendly bounded CPU event history: no pointers or guest data.
class ResourceLifetimeHistory {
public:
    static constexpr size_t Capacity = 768;

    void Push(const ResourceEvent& event) noexcept {
        m_records[static_cast<size_t>(m_total % Capacity)] = event;
        ++m_total;
    }
    [[nodiscard]] size_t Count() const noexcept {
        return static_cast<size_t>(std::min<uint64_t>(m_total, Capacity));
    }
    [[nodiscard]] uint64_t Total() const noexcept { return m_total; }
    [[nodiscard]] uint64_t Dropped() const noexcept { return m_total - Count(); }
    [[nodiscard]] const ResourceEvent& At(size_t index) const noexcept {
        return m_records[static_cast<size_t>((m_total - Count() + index) % Capacity)];
    }
    void Dump(FILE* file, bool full) const noexcept {
        if (!file) return;
        uint32_t premature = 0;
        for (size_t i = 0; i < Count(); ++i)
            premature += PrematureResourceRetire(At(i)) ? 1u : 0u;
        std::fprintf(file,
                     "XXII_GPU_RESOURCE_SUMMARY events=%" PRIu64 " retained=%zu"
                     " dropped=%" PRIu64 " premature_retire=%u\n",
                     m_total, Count(), Dropped(), premature);
        const size_t start = full || Count() <= 16 ? 0 : Count() - 16;
        for (size_t i = start; i < Count(); ++i) {
            const auto& e = At(i);
            std::fprintf(file,
                         "XXII_GPU_RESOURCE_EVENT serial=%" PRIu64
                         " kind=%s tick=%" PRIu64 " completed=%" PRIu64
                         " retire_tick=%" PRIu64 " slot=%u generation=%u"
                         " format=%u flags=%u bytes=%" PRIu64 " premature=%u\n",
                         m_total - Count() + i, ResourceActionName(e.action),
                         e.event_tick, e.completed_tick, e.retire_tick,
                         e.slot_index, e.slot_generation, e.format,
                         e.flags, e.bytes, PrematureResourceRetire(e) ? 1u : 0u);
        }
        std::fflush(file);
    }
private:
    std::array<ResourceEvent, Capacity> m_records {};
    uint64_t m_total = 0;
};
} // namespace Libs::Graphics::GpuTrace

#endif // KYTY_DIAGNOSTIC_RUNTIME
#endif

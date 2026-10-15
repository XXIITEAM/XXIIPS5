#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

#include "common/common.h"
#include "common/diagnostics/runtimeTelemetry.h"
#if KYTY_DIAGNOSTIC_RUNTIME
#include "graphics/host_gpu/renderer/gpuFlightRecorder.h"
#include "graphics/host_gpu/renderer/gpuResourceLifetime.h"
#endif
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#if KYTY_DIAGNOSTIC_RUNTIME
#include <array>
#include <memory>
#include <mutex>
#endif

namespace Libs::Graphics {

struct GraphicContext;

class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_semaphore; }

	void Refresh();
	void Wait(uint64_t tick);
#if KYTY_DIAGNOSTIC_RUNTIME
    // Runtime-disabled except during explicit compatibility probes. Retain
    // only numeric GPU operation metadata, never guest instructions/shaders.
    void RecordSubmission(uint64_t tick, uint32_t operation, uint64_t submitId,
                          uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint64_t e,
                          uint64_t checkpoint_marker,
                          const GpuTrace::CommandTrace& commands);
    void DumpRecentSubmissions() const;
    void SampleFlightRecorder(uint64_t submitted_tick) const;
    void RecordResourceEvent(GpuTrace::ResourceAction action, uint64_t tick,
                             uint64_t retire_tick, uint32_t slot, uint32_t generation,
                             uint32_t format, uint32_t flags, uint64_t bytes);
    void DumpCheckpoints() const;
#endif
    void DumpDeviceFault() const;

private:
	GraphicContext&       m_graphics;
	vk::Semaphore         m_semaphore = nullptr;
	std::atomic<uint64_t> m_gpu_tick {0};
	std::atomic<uint64_t> m_current_tick {1};
#if KYTY_DIAGNOSTIC_RUNTIME
    struct SubmittedOperation {
        uint64_t tick = 0;
        uint32_t operation = 0;
        uint64_t submitId = 0;
        uint32_t arg0 = 0, arg1 = 0, arg2 = 0, arg3 = 0;
        uint64_t arg4 = 0;
        uint64_t checkpoint_marker = 0;
    };
    static constexpr size_t TraceCapacity = 32;
    mutable std::mutex m_traceMutex;
    std::array<SubmittedOperation, TraceCapacity> m_trace {};
    size_t m_traceHead = 0;
    size_t m_traceCount = 0;
    bool m_traceEnabled = false;
    // A full recorder is about a megabyte. Allocate only in opt-in probes:
    // CommandScheduler can otherwise live on the Windows test thread stack.
    std::unique_ptr<GpuTrace::FlightRecorder> m_flight_recorder;
    bool m_resource_trace_enabled = false;
    GpuTrace::ResourceLifetimeHistory m_resource_history;
#endif
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

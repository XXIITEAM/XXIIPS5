#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <memory>

namespace Libs::Graphics {

namespace {
// Vulkan enum 4 is an unknown instruction pointer; it is NOT evidence of a
// bad instruction or an invalid memory read. Report the enum name, not a GPU
// virtual address, so CI logs remain useful and privacy-conscious.
const char* DeviceFaultAddressKind(VkDeviceFaultAddressTypeEXT type) {
    switch (type) {
        case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read_invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write_invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute_invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
            return "instruction_pointer_unknown";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
            return "instruction_pointer_invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
            return "instruction_pointer_fault";
        default: return "unrecognized";
    }
}
// Preserve the actual Vulkan error and timeline state: an unspecified
// EXIT_NOT_IMPLEMENTED only reports source:line, masking device-lost causes.
// Never treat a failed wait as completed GPU work.
void ReportSemaphoreError(const char* operation, vk::Result result, uint64_t wanted,
                          uint64_t submitted, uint64_t completed) {
    const auto name = vk::to_string(result);
    LOGF("GPU semaphore %s failed: %s (%d), waited=%" PRIu64
         ", submitted=%" PRIu64 ", completed=%" PRIu64 "\n",
         operation, name.c_str(), static_cast<int>(result),
         wanted, submitted, completed);
    std::fprintf(stderr,
                 "XXII_GPU_SEMAPHORE_FAILURE operation=%s result=%s code=%d"
                 " waited=%" PRIu64 " submitted=%" PRIu64
                 " completed=%" PRIu64 "\n",
                 operation, name.c_str(), static_cast<int>(result),
                 wanted, submitted, completed);
    std::fflush(stderr);
    // Persist the real Vulkan error alongside the flight command trace.
    // Frontend queues are bounded and may omit the terminal error.
    const char* probe = std::getenv("XXII_COMPAT_PROBE");
    const char* report_path = std::getenv("XXII_COMPAT_GPU_FLIGHT_LOG");
    if (probe && probe[0] == '1' && probe[1] == '\0' &&
        report_path && *report_path != '\0') {
        if (FILE* report = std::fopen(report_path, "ab")) {
            std::fprintf(report,
                         "XXII_GPU_FLIGHT_VULKAN_ERROR operation=%s result=%s code=%d"
                         " wanted=%" PRIu64 " submitted=%" PRIu64 " completed=%" PRIu64 "\n",
                         operation, name.c_str(), static_cast<int>(result),
                         wanted, submitted, completed);
            std::fclose(report);
        }
    }
}
} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
    const char* probe = std::getenv("XXII_COMPAT_PROBE");
    m_traceEnabled = probe && probe[0] == '1' && probe[1] == '\0';
    const char* flight = std::getenv("XXII_COMPAT_GPU_FLIGHT_RECORDER");
    if (m_traceEnabled && flight && flight[0] == '1' && flight[1] == '\0')
        m_flight_recorder = std::make_unique<GpuTrace::FlightRecorder>();
#if KYTY_DIAGNOSTIC_RUNTIME
    const char* resource_trace = std::getenv("XXII_COMPAT_GPU_RESOURCE_LIFETIME");
    m_resource_trace_enabled = m_traceEnabled && resource_trace &&
        resource_trace[0] == '1' && resource_trace[1] == '\0';
#endif
    if (m_traceEnabled) {
        std::fprintf(stderr, "XXII_GPU_BREADCRUMB_INIT capacity=%zu\n", TraceCapacity);
        std::fflush(stderr);
    }
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
    if (result != vk::Result::eSuccess || m_semaphore == nullptr) {
        ReportSemaphoreError("create", result, 0, CurrentTick(), KnownGpuTick());
    }
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

void MasterSemaphore::RecordSubmission(uint64_t tick, uint32_t operation,
                                       uint64_t submitId, uint32_t a, uint32_t b,
                                       uint32_t c, uint32_t d, uint64_t e,
                                       uint64_t checkpoint_marker,
                                       const GpuTrace::CommandTrace& commands) {
    if (!m_traceEnabled) return;
    // Observe device-memory pressure without changing queue submission behavior.
    // Sample once per 2048 completed CPU-side submissions; private probe only.
    const char* memory_trace = std::getenv("XXII_COMPAT_GPU_MEMORY_TRACE");
    if (memory_trace && memory_trace[0] == '1' && memory_trace[1] == '\0' &&
        (tick % 2048u) == 0) {
        m_graphics.LogMemoryBudget();
    }
    std::lock_guard guard(m_traceMutex);
    m_trace[m_traceHead] = {tick, operation, submitId, a, b, c, d, e,
                            checkpoint_marker};
    m_traceHead = (m_traceHead + 1) % TraceCapacity;
    m_traceCount = std::min(m_traceCount + 1, TraceCapacity);
    if (m_flight_recorder && commands.Total() != 0)
        m_flight_recorder->Add(tick, commands);
}

#if KYTY_DIAGNOSTIC_RUNTIME
void MasterSemaphore::RecordResourceEvent(GpuTrace::ResourceAction action, uint64_t tick,
                                         uint64_t retire_tick, uint32_t slot,
                                         uint32_t generation, uint32_t format,
                                         uint32_t flags, uint64_t bytes) {
    if (!m_resource_trace_enabled) return;
    const GpuTrace::ResourceEvent event {
        action, tick, KnownGpuTick(), retire_tick, bytes, slot, generation, format, flags
    };
    std::lock_guard guard(m_traceMutex);
    m_resource_history.Push(event);
}
#endif

void MasterSemaphore::DumpDeviceFault() const {
    if (!m_graphics.device_fault_ext_enabled || m_graphics.device == nullptr) return;
    // After VK_ERROR_DEVICE_LOST only: query limited numeric diagnostics.
    // Never extract vendor crash binaries or print GPU virtual addresses.
    const VkDevice device = static_cast<VkDevice>(m_graphics.device);
    const auto get_fault = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkGetDeviceFaultInfoEXT"));
    if (!get_fault) {
        std::fprintf(stderr, "XXII_DEVICE_FAULT_INFO unavailable=missing_entrypoint\n");
        return;
    }

    VkDeviceFaultCountsEXT counts {};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    VkResult result = get_fault(device, &counts, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "XXII_DEVICE_FAULT_INFO query_result=%d\n", int(result));
        std::fflush(stderr);
        return;
    }

    constexpr uint32_t max_faults = 8;
    std::array<VkDeviceFaultAddressInfoEXT, max_faults> addresses {};
    std::array<VkDeviceFaultVendorInfoEXT, max_faults> vendors {};
    const uint32_t available_addresses = counts.addressInfoCount;
    const uint32_t available_vendors = counts.vendorInfoCount;
    const uint32_t addresses_to_query = std::min(counts.addressInfoCount, max_faults);
    const uint32_t vendors_to_query = std::min(counts.vendorInfoCount, max_faults);
    const auto vendor_binary_size = counts.vendorBinarySize;
    counts.addressInfoCount = addresses_to_query;
    counts.vendorInfoCount = vendors_to_query;
    counts.vendorBinarySize = 0; // No binary driver crash dumps are requested.
    VkDeviceFaultInfoEXT info {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = addresses_to_query ? addresses.data() : nullptr;
    info.pVendorInfos = vendors_to_query ? vendors.data() : nullptr;
    result = get_fault(device, &counts, &info);
    std::fprintf(stderr,
        "XXII_DEVICE_FAULT_INFO result=%d addresses=%u available_addresses=%u"
        " vendors=%u available_vendors=%u binary_available=%d\n",
        int(result), counts.addressInfoCount, available_addresses,
        counts.vendorInfoCount, available_vendors,
        vendor_binary_size > 0 ? 1 : 0);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        for (uint32_t i = 0; i < std::min(counts.addressInfoCount, max_faults); ++i) {
            std::fprintf(stderr,
                "XXII_DEVICE_FAULT_ADDRESS index=%u type=%d kind=%s precision=%" PRIu64 "\n",
                i, int(addresses[i].addressType),
                DeviceFaultAddressKind(addresses[i].addressType),
                static_cast<uint64_t>(addresses[i].addressPrecision));
        }
        for (uint32_t i = 0; i < std::min(counts.vendorInfoCount, max_faults); ++i) {
            std::fprintf(stderr,
                "XXII_DEVICE_FAULT_VENDOR index=%u code=%" PRIu64 "\n",
                i, static_cast<uint64_t>(vendors[i].vendorFaultCode));
        }
    }
    std::fflush(stderr);
}

void MasterSemaphore::DumpCheckpoints() const {
    // vkGetQueueCheckpointDataNV is valid only after VK_ERROR_DEVICE_LOST.
    // Callers enforce that condition. Markers are opaque numeric IDs assigned
    // by the scheduler, never raw guest pointers or GPU virtual addresses.
    if (!m_graphics.device_checkpoints_enabled ||
        m_graphics.device == nullptr || m_graphics.queue == nullptr) return;
    const VkDevice device = static_cast<VkDevice>(m_graphics.device);
    const auto get_checkpoints = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(
            device, "vkGetQueueCheckpointDataNV"));
    if (!get_checkpoints) {
        std::fprintf(stderr, "XXII_GPU_CHECKPOINT unavailable=entrypoint\n");
        std::fflush(stderr);
        return;
    }
    const VkQueue queue = static_cast<VkQueue>(m_graphics.queue);
    uint32_t available = 0;
    get_checkpoints(queue, &available, nullptr);
    constexpr uint32_t limit = 16;
    std::array<VkCheckpointDataNV, limit> checkpoints {};
    uint32_t count = std::min(available, limit);
    for (uint32_t i = 0; i < count; ++i)
        checkpoints[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
    if (count != 0) get_checkpoints(queue, &count, checkpoints.data());
    count = std::min(count, limit);
    std::fprintf(stderr, "XXII_GPU_CHECKPOINT_COUNT available=%u recorded=%u\n",
                 available, count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t marker = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(checkpoints[i].pCheckpointMarker));
        constexpr uint64_t DrawPhaseTag = UINT64_C(1) << 43;
        constexpr uint64_t SubmitMask = (UINT64_C(1) << 39) - 1;
        constexpr std::array<const char*, 10> phase_names {
            "before_bindings", "vertex_buffers", "descriptors", "index_buffer",
            "dynamic_state", "begin_rendering", "pipeline_bound",
            "before_primitive", "after_primitive", "draw_complete"
        };
        if ((marker & DrawPhaseTag) != 0 &&
            (marker & UINT64_C(0xFFFF000000000000)) == 0) {
            const uint32_t phase = static_cast<uint32_t>(marker & 15);
            std::fprintf(stderr,
                         "XXII_GPU_CHECKPOINT stage=%u marker=%" PRIu64
                         " kind=watched_draw_phase phase=%u name=%s submit=%" PRIu64 "\n",
                         static_cast<unsigned>(checkpoints[i].stage), marker, phase,
                         phase < phase_names.size() ? phase_names[phase] : "unknown",
                         (marker >> 4) & SubmitMask);
        } else if ((marker & (UINT64_C(1) << 42)) != 0 &&
                   (marker & DrawPhaseTag) == 0) {
            constexpr uint64_t BeginTag = UINT64_C(1) << 42;
            std::fprintf(stderr,
                         "XXII_GPU_CHECKPOINT stage=%u marker=%" PRIu64
                         " kind=command_buffer_begin tick=%" PRIu64 "\n",
                         static_cast<unsigned>(checkpoints[i].stage), marker,
                         marker & (BeginTag - 1));
        } else if ((marker & (UINT64_C(1) << 41)) != 0 &&
                   (marker & ((UINT64_C(1) << 42) | DrawPhaseTag)) == 0) {
            constexpr uint64_t SubmitMask = (UINT64_C(1) << 23) - 1;
            constexpr uint64_t ArgMask = (UINT64_C(1) << 12) - 1;
            std::fprintf(stderr,
                         "XXII_GPU_CHECKPOINT stage=%u marker=%" PRIu64
                         " kind=guest_op_tag operation=%u arg0=%u submit_low23=%" PRIu64 "\n",
                         static_cast<unsigned>(checkpoints[i].stage), marker,
                         static_cast<unsigned>(marker & 63),
                         static_cast<unsigned>((marker >> 6) & ArgMask),
                         (marker >> 18) & SubmitMask);
        } else {
            std::fprintf(stderr,
                         "XXII_GPU_CHECKPOINT stage=%u marker=%" PRIu64
                         " kind=command_buffer_tail\n",
                         static_cast<unsigned>(checkpoints[i].stage), marker);
        }
    }
    std::fflush(stderr);
}

void MasterSemaphore::SampleFlightRecorder(uint64_t submitted_tick) const {
    // Submitted commands form reference snapshots, even during startup:
    // observe without a GPU wait; initial powers of 2, then every 2048 submits.
    if (!m_flight_recorder || !GpuTrace::ShouldSampleTick(submitted_tick)) return;
    const char* report_path = std::getenv("XXII_COMPAT_GPU_FLIGHT_LOG");
    if (!report_path || !*report_path) return;
    auto snapshot = std::make_unique<GpuTrace::FlightRecorder>();
#if KYTY_DIAGNOSTIC_RUNTIME
    GpuTrace::ResourceLifetimeHistory resource_snapshot;
#endif
    {
        std::lock_guard guard(m_traceMutex);
        *snapshot = *m_flight_recorder;
#if KYTY_DIAGNOSTIC_RUNTIME
        if (m_resource_trace_enabled) resource_snapshot = m_resource_history;
#endif
    }
    if (FILE* report = std::fopen(report_path, "ab")) {
        std::fprintf(report,
                     "XXII_GPU_FLIGHT_HEALTHY_SAMPLE submitted=%" PRIu64
                     " known_completed=%" PRIu64 "\n",
                     submitted_tick, KnownGpuTick());
        snapshot->Dump(report, KnownGpuTick(), false);
#if KYTY_DIAGNOSTIC_RUNTIME
        if (m_resource_trace_enabled) resource_snapshot.Dump(report, false);
#endif
        std::fclose(report);
    }
}

void MasterSemaphore::DumpRecentSubmissions() const {
    if (!m_traceEnabled) return;
    std::array<SubmittedOperation, TraceCapacity> snapshot {};
    auto flight_snapshot = m_flight_recorder
                               ? std::make_unique<GpuTrace::FlightRecorder>()
                               : nullptr;
#if KYTY_DIAGNOSTIC_RUNTIME
    GpuTrace::ResourceLifetimeHistory resource_snapshot;
#endif
    size_t count = 0;
    {
        std::lock_guard guard(m_traceMutex);
        count = m_traceCount;
        if (flight_snapshot) *flight_snapshot = *m_flight_recorder;
#if KYTY_DIAGNOSTIC_RUNTIME
        resource_snapshot = m_resource_history;
#endif
        const size_t oldest = (m_traceHead + TraceCapacity - count) % TraceCapacity;
        for (size_t i = 0; i < count; ++i)
            snapshot[i] = m_trace[(oldest + i) % TraceCapacity];
    }
    std::fprintf(stderr, "XXII_GPU_BREADCRUMB_COUNT count=%zu\n", count);
    for (size_t i = 0; i < count; ++i) {
        const auto& op = snapshot[i];
        std::fprintf(stderr,
                     "XXII_GPU_BREADCRUMB tick=%" PRIu64 " operation=%u submit=%" PRIu64
                     " args=%u,%u,%u,%u,0x%016" PRIx64
                     " checkpoint=%" PRIu64 "\n",
                     op.tick, op.operation, op.submitId,
                     op.arg0, op.arg1, op.arg2, op.arg3, op.arg4,
                     op.checkpoint_marker);
    }
    if (flight_snapshot && flight_snapshot->Count() != 0) {
        flight_snapshot->Dump(stderr, KnownGpuTick());
#if KYTY_DIAGNOSTIC_RUNTIME
        if (m_resource_trace_enabled) resource_snapshot.Dump(stderr, false);
#endif
        // Project-local opt-in postmortem log, immune to UI marker truncation.
        const char* report_path = std::getenv("XXII_COMPAT_GPU_FLIGHT_LOG");
        if (report_path != nullptr && report_path[0] != '\0') {
            if (FILE* report = std::fopen(report_path, "ab")) {
                flight_snapshot->Dump(report, KnownGpuTick(), true, true);
#if KYTY_DIAGNOSTIC_RUNTIME
                if (m_resource_trace_enabled) resource_snapshot.Dump(report, true);
#endif
                std::fclose(report);
            }
        }
    }
    std::fflush(stderr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
    if (result != vk::Result::eSuccess) {
        ReportSemaphoreError("getCounter", result, 0, CurrentTick(), KnownGpuTick());
        if (result == vk::Result::eErrorDeviceLost) {
            DumpDeviceFault();
            DumpCheckpoints();
        }
        DumpRecentSubmissions();
    }
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
    if (result != vk::Result::eSuccess) {
        ReportSemaphoreError("wait", result, tick, CurrentTick(), KnownGpuTick());
        if (result == vk::Result::eErrorDeviceLost) {
            DumpDeviceFault();
            DumpCheckpoints();
        }
        DumpRecentSubmissions();
    }
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics

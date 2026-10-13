#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>

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
}
} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
    const char* probe = std::getenv("XXII_COMPAT_PROBE");
    m_traceEnabled = probe && probe[0] == '1' && probe[1] == '\0';
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
                                       uint32_t c, uint32_t d, uint64_t e) {
    if (!m_traceEnabled) return;
    std::lock_guard guard(m_traceMutex);
    m_trace[m_traceHead] = {tick, operation, submitId, a, b, c, d, e};
    m_traceHead = (m_traceHead + 1) % TraceCapacity;
    m_traceCount = std::min(m_traceCount + 1, TraceCapacity);
}

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

void MasterSemaphore::DumpRecentSubmissions() const {
    if (!m_traceEnabled) return;
    std::array<SubmittedOperation, TraceCapacity> snapshot {};
    size_t count = 0;
    {
        std::lock_guard guard(m_traceMutex);
        count = m_traceCount;
        const size_t oldest = (m_traceHead + TraceCapacity - count) % TraceCapacity;
        for (size_t i = 0; i < count; ++i)
            snapshot[i] = m_trace[(oldest + i) % TraceCapacity];
    }
    std::fprintf(stderr, "XXII_GPU_BREADCRUMB_COUNT count=%zu\n", count);
    for (size_t i = 0; i < count; ++i) {
        const auto& op = snapshot[i];
        std::fprintf(stderr,
                     "XXII_GPU_BREADCRUMB tick=%" PRIu64 " operation=%u submit=%" PRIu64
                     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
                     op.tick, op.operation, op.submitId,
                     op.arg0, op.arg1, op.arg2, op.arg3, op.arg4);
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
        if (result == vk::Result::eErrorDeviceLost) DumpDeviceFault();
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
        if (result == vk::Result::eErrorDeviceLost) DumpDeviceFault();
        DumpRecentSubmissions();
    }
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics

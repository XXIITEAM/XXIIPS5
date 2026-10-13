#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace Libs::Graphics {

namespace {
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
        DumpRecentSubmissions();
    }
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics

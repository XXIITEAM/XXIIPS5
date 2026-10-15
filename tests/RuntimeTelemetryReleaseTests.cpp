#include "common/diagnostics/hleTrace.h"
#include "graphics/host_gpu/renderer/gpuResourceLifetime.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <cstdio>
#include <cstdlib>

template <typename T>
concept HasResourceCapture = requires { &T::RecordResourceEvent; };
static_assert(!HasResourceCapture<Libs::Graphics::MasterSemaphore>);
template <typename T>
concept HasSubmissionHistory = requires { &T::RecordSubmission; };
template <typename T>
concept HasFlightSampler = requires { &T::SampleFlightRecorder; };
template <typename T>
concept HasBreadcrumbDump = requires { &T::DumpRecentSubmissions; };
template <typename T>
concept HasCommandTrace = requires { &T::RecordGpuTrace; };
template <typename T>
concept HasDiagnosticImageId = requires { &T::SetDiagnosticId; };
template <typename T>
concept HasGpuCheckpoints = requires(T& value) { value.device_checkpoints_enabled; };
template <typename T>
concept HasCheckpointDump = requires { &T::DumpCheckpoints; };
static_assert(!HasSubmissionHistory<Libs::Graphics::MasterSemaphore>);
static_assert(!HasFlightSampler<Libs::Graphics::MasterSemaphore>);
static_assert(!HasBreadcrumbDump<Libs::Graphics::MasterSemaphore>);
static_assert(!HasCommandTrace<Libs::Graphics::CommandBuffer>);
static_assert(!HasDiagnosticImageId<Libs::Graphics::Image>);
static_assert(!HasGpuCheckpoints<Libs::Graphics::GraphicContext>);
static_assert(!HasCheckpointDump<Libs::Graphics::MasterSemaphore>);

namespace Libs::Graphics {
struct ProductionSemaphoreStorage {
	GraphicContext& graphics;
	vk::Semaphore semaphore;
	std::atomic<uint64_t> gpu_tick;
	std::atomic<uint64_t> current_tick;
};
struct ProductionCommandStorage {
	RenderContext& context;
	GraphicContext& graphics;
	vk::CommandBuffer buffer;
	uint32_t operation;
	uint64_t submit;
	uint32_t arg0, arg1, arg2, arg3;
	uint64_t arg4;
	RenderState render_state;
	bool rendering;
	HW::Context* registers;
	HW::UserConfig* user_config;
	HW::Shader* shaders;
};
static_assert(sizeof(MasterSemaphore) == sizeof(ProductionSemaphoreStorage));
static_assert(sizeof(CommandBuffer) == sizeof(ProductionCommandStorage));
} // namespace Libs::Graphics

int main() {
#if defined(_WIN32)
	_putenv_s("XXII_COMPAT_HLE_TRIAGE", "1");
	_putenv_s("XXII_COMPAT_HLE_WAIT_MAP", "Local\\PSVXXII-WAITS-release-test");
	if (Common::HleWaitSnapshot::RuntimeTable() != nullptr) return 1;
#else
	setenv("XXII_COMPAT_HLE_TRIAGE", "1", 1);
#endif
	if (Common::HleTrace::Enabled()) return 2;
	Common::HleTrace::Span span("release_must_not_trace", "infinite");
	span.Finish(0);
	std::printf("RuntimeTelemetryReleaseTests PASS master_bytes=%zu command_bytes=%zu image_bytes=%zu\n",
	            sizeof(Libs::Graphics::MasterSemaphore),
	            sizeof(Libs::Graphics::CommandBuffer), sizeof(Libs::Graphics::Image));
}

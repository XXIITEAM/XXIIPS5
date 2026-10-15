#include "common/diagnostics/hleTrace.h"
#include "graphics/host_gpu/renderer/gpuResourceLifetime.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <cstdio>
#include <cstdlib>

template <typename T>
concept HasResourceCapture = requires { &T::RecordResourceEvent; };
static_assert(!HasResourceCapture<Libs::Graphics::MasterSemaphore>);

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
	std::printf("RuntimeTelemetryReleaseTests PASS master_bytes=%zu\n",
	            sizeof(Libs::Graphics::MasterSemaphore));
}

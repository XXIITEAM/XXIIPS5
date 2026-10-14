#include "common/diagnostics/hleTrace.h"

#include <cstdio>
#include <cstdlib>

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
	std::puts("RuntimeTelemetryReleaseTests PASS");
}

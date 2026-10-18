// PSVXXII J1a: XXII_CRASH_V1 formatting, sanitization and fatal-path emission.
#include "common/diagnostics/crashReceipt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "CrashReceiptTests: failed: %s\n", what);
		++g_failures;
	}
}
void Probe() {}
} // namespace

int main(int argc, char** argv) {
	if (argc == 2 && std::strncmp(argv[1], "--crash=", 8) == 0) {
#if defined(_WIN32)
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
		Common::Diagnostics::InstallCrashReceipts();
		const char* mode = argv[1] + 8;
		if (std::strcmp(mode, "abort") == 0) std::abort();
		if (std::strcmp(mode, "terminate") == 0) std::terminate();
#if defined(_WIN32)
		if (std::strcmp(mode, "unhandled") == 0) RaiseException(0xE0001234u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
		return 2;
	}
	char line[192];
	Common::Diagnostics::FormatCrashReceipt(line, sizeof(line), "guest_fault", 0xC0000005u, "write", "eboot.bin", 0x1234);
	Check(std::strcmp(line, "XXII_CRASH_V1 class=guest_fault code=0xC0000005 access=write module=eboot.bin offset=0x1234\n") == 0,
		  "receipt format");
	char name[49];
	Common::Diagnostics::SanitizeModuleName("C:\\Users\\someone\\My Game/eboot bin.prx", name, sizeof(name));
	Check(std::strcmp(name, "eboot_bin.prx") == 0, "base name only, unsafe characters replaced");
	Common::Diagnostics::SanitizeModuleName(nullptr, name, sizeof(name));
	Check(std::strcmp(name, "unknown") == 0, "null name");
	Common::Diagnostics::SanitizeModuleName("dir/", name, sizeof(name));
	Check(std::strcmp(name, "unknown") == 0, "empty base name");
	const std::string long_name(200, 'a');
	Common::Diagnostics::SanitizeModuleName(long_name.c_str(), name, sizeof(name));
	Check(std::strlen(name) == 48, "bounded module name");
#if defined(_WIN32)
	uint64_t offset = 0;
	Check(Common::Diagnostics::HostModuleOf(reinterpret_cast<uint64_t>(&Probe), name, sizeof(name), &offset) &&
			  std::strstr(name, "crash_receipt_tests") != nullptr && offset != 0,
		  "host module and offset of the test's own code");
#endif
	if (g_failures == 0) std::puts("CrashReceiptTests: all cases passed");
	return g_failures == 0 ? 0 : 1;
}

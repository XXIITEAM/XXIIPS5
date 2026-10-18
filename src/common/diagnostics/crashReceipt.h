#pragma once
// PSVXXII J1a: XXII_CRASH_V1 is one bounded, sanitized line emitted before an
// abnormal runtime end, only in explicit Developer captures
// (KYTY_DIAGNOSTIC_RUNTIME and XXII_COMPAT_PROBE=1). It carries module base
// names and module-relative offsets only: no path, user name, guest data or
// absolute address. Installed handlers chain to the previous behavior.

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Common::Diagnostics {

inline void SanitizeModuleName(const char* raw, char* out, size_t size) {
	if (out == nullptr || size == 0) return;
	const char* base = raw;
	for (const char* p = raw; p != nullptr && *p != '\0'; ++p) {
		if (*p == '/' || *p == '\\') base = p + 1;
	}
	size_t n = 0;
	for (const char* p = base; p != nullptr && *p != '\0' && n + 1 < size; ++p) {
		const char c	= *p;
		const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
						  c == '_' || c == '.' || c == '-';
		out[n++] = safe ? c : '_';
	}
	if (n == 0) {
		std::snprintf(out, size, "unknown");
		return;
	}
	out[n] = '\0';
}

inline int FormatCrashReceipt(char* out, size_t size, const char* crash_class, uint32_t code, const char* access,
							  const char* module, uint64_t offset) {
	return std::snprintf(out, size, "XXII_CRASH_V1 class=%s code=0x%08" PRIX32 " access=%s module=%s offset=0x%" PRIx64 "\n",
						 crash_class, code, access, module, offset);
}

inline bool CrashReceiptsEnabled() {
#if KYTY_DIAGNOSTIC_RUNTIME
	const char* probe = std::getenv("XXII_COMPAT_PROBE");
	return probe != nullptr && probe[0] == '1' && probe[1] == '\0';
#else
	return false;
#endif
}

// The first receipt wins: later fatal paths (EXIT, abort) never repeat it.
inline void EmitCrashReceipt(const char* crash_class, uint32_t code, const char* access, const char* module,
							 uint64_t offset) {
	static std::atomic_bool emitted {false};
	if (!CrashReceiptsEnabled() || emitted.exchange(true)) return;
	char line[192];
	if (FormatCrashReceipt(line, sizeof(line), crash_class, code, access, module, offset) > 0) {
		std::fputs("\n", stderr);
		std::fputs(line, stderr);
		std::fflush(stderr);
	}
}

// Sanitized base name and module-relative offset of a host address (Windows).
bool HostModuleOf(uint64_t address, char* name, size_t size, uint64_t* offset);

// Unhandled-exception filter (Windows), terminate and SIGABRT receipts, installed
// once and chained to the previous handlers; no-op unless receipts are enabled.
void InstallCrashReceipts();

} // namespace Common::Diagnostics

#include "common/assert.h"

#include "common/logging/log.h"
#include "common/diagnostics/hleTrace.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

#include "common/diagnostics/crashReceipt.h"

namespace Common {

// Only explicit Developer compatibility captures carry this fixed fatal
// breadcrumb. The existing HLE trace may be disabled or its ordinary tail
// evicted; report the assert site *before* cleanup/_Exit(321). No guest
// strings, shader contents, memory addresses or source directory paths.
static void ReportFatalExitSite(const char* kind, const char* file, int line) noexcept {
#if KYTY_DIAGNOSTIC_RUNTIME
    const char* probe = std::getenv("XXII_COMPAT_PROBE");
    if (!probe || probe[0] != '1' || probe[1] != '\0') return;
    char source[49] {};
    Diagnostics::ReportCrashContext();
    std::fprintf(stderr, "\nXXII_FATAL_EXIT_V1 code=321 kind=%s source=%s line=%d\n",
                 kind, HleTrace::SourceBase(file, source), line);
    std::fflush(stderr);
#else
    (void)kind; (void)file; (void)line;
#endif
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL, title, text, file,
	                   line);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
    ReportFatalExitSite("assert_condition", file, line);
    HleTrace::Failure("assert_condition", file, line);
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
    ReportFatalExitSite("not_implemented", file, line);
    HleTrace::Failure("not_implemented", file, line);
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
    ReportFatalExitSite("fatal_exit", file, line);
    HleTrace::Failure("fatal_exit", file, line);
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
    ReportFatalExitSite("fatal_exit", file, line);
    HleTrace::Failure("fatal_exit", file, line);
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common

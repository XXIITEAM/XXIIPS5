#include "common/diagnostics/crashReceipt.h"

#include <csignal>
#include <exception>

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

namespace Common::Diagnostics {
namespace {
std::terminate_handler g_previous_terminate = nullptr;

#if defined(_WIN32)
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

LONG WINAPI UnhandledFilter(PEXCEPTION_POINTERS info) {
	if (info != nullptr && info->ExceptionRecord != nullptr) {
		const auto address	= reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress);
		char	   module[49] = "unknown";
		uint64_t   offset	 = 0;
		HostModuleOf(address, module, sizeof(module), &offset);
		EmitCrashReceipt("unhandled_exception", static_cast<uint32_t>(info->ExceptionRecord->ExceptionCode), "none",
						 module, offset);
	}
	return g_previous_filter != nullptr ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

void OnTerminate() {
	EmitCrashReceipt("terminate", 0, "none", "unknown", 0);
	if (g_previous_terminate != nullptr) g_previous_terminate();
	std::abort();
}

void OnAbort(int /*signal*/) {
	EmitCrashReceipt("abort", 0, "none", "unknown", 0);
	std::signal(SIGABRT, SIG_DFL);
	std::raise(SIGABRT);
}
} // namespace

bool HostModuleOf(uint64_t address, char* name, size_t size, uint64_t* offset) {
#if defined(_WIN32)
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   reinterpret_cast<LPCWSTR>(address), &module) == 0 ||
		module == nullptr) {
		return false;
	}
	char path[MAX_PATH] {};
	if (GetModuleFileNameA(module, path, MAX_PATH) == 0) return false;
	SanitizeModuleName(path, name, size);
	*offset = address - reinterpret_cast<uint64_t>(module);
	return true;
#else
	(void)address;
	(void)name;
	(void)size;
	(void)offset;
	return false;
#endif
}

void InstallCrashReceipts() {
	static std::atomic_bool installed {false};
	if (!CrashReceiptsEnabled() || installed.exchange(true)) return;
#if defined(_WIN32)
	g_previous_filter = SetUnhandledExceptionFilter(UnhandledFilter);
#endif
	g_previous_terminate = std::set_terminate(OnTerminate);
	std::signal(SIGABRT, OnAbort);
}

} // namespace Common::Diagnostics

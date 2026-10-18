#include "common/diagnostics/crashReceipt.h"

#include <csignal>
#include <exception>
#include <cwchar>

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

namespace Common::Diagnostics {
namespace {
std::terminate_handler g_previous_terminate = nullptr;

#if defined(_WIN32)
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
HANDLE g_parent_dump_ack = nullptr;

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

void ReportHostStack() {
#if defined(_WIN32)
	void*        frames[kMaxStackFrames] {};
	const USHORT count = RtlCaptureStackBackTrace(1, kMaxStackFrames, frames, nullptr);
	char         line[160];
	for (USHORT i = 0; i < count; ++i) {
		char     module[49];
		uint64_t offset = 0;
		if (!HostModuleOf(reinterpret_cast<uint64_t>(frames[i]), module, sizeof(module), &offset)) break;
		if (FormatStackFrame(line, sizeof(line), i, module, offset) > 0) std::fputs(line, stderr);
	}
	std::fflush(stderr);
#endif
}

void WaitForParentCrashCapture() noexcept {
#if defined(_WIN32)
    // This is a crash-only bounded wait, not an idle/session synchronization.
    if (g_parent_dump_ack) (void)WaitForSingleObject(g_parent_dump_ack, 9000);
#endif
}

void InstallCrashReceipts() {
	static std::atomic_bool installed {false};
	if (!CrashReceiptsEnabled() || installed.exchange(true)) return;
#if defined(_WIN32)
    wchar_t ack_name[128] {};
    const DWORD chars = GetEnvironmentVariableW(L"XXII_CRASH_DUMP_EVENT", ack_name, 128);
    constexpr wchar_t prefix[] = L"Local\\XXII_CrashDump_";
    if (chars > sizeof(prefix) / sizeof(wchar_t) - 1 && chars < 128 &&
        std::wcsncmp(ack_name, prefix, sizeof(prefix) / sizeof(wchar_t) - 1) == 0)
        g_parent_dump_ack = OpenEventW(SYNCHRONIZE, FALSE, ack_name);
	g_previous_filter = SetUnhandledExceptionFilter(UnhandledFilter);
#endif
	g_previous_terminate = std::set_terminate(OnTerminate);
	std::signal(SIGABRT, OnAbort);
}

} // namespace Common::Diagnostics

// PSVXXII J2: unresolved-import thunks record calls and the first guest caller.
#include "common/common.h"
#include "common/diagnostics/crashReceipt.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/runtimeLinker.h"

#include <cstdio>
#include <cstring>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "UnresolvedImportLedgerTests: failed: %s\n", what);
		++g_failures;
	}
}
void InitSubsystems() {
	static Common::Subsystems subsystems;
	Common::VirtualMemory::Init();
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	// Unresolved-import thunks live in guest runtime memory.
	Libs::LibKernel::Memory::SetFlexibleMemorySize(3221225472ULL);
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
}
// Guest code calls thunks with the System V ABI.
using GuestThunk = uint64_t(KYTY_SYSV_ABI*)();
} // namespace

int main(int argc, char** argv) {
	InitSubsystems();
	const uint64_t thunk = Loader::RegisterUnresolvedImportForTest("AbCdEfGhIjK#A#B", "C:/private/games/eboot.bin");
	auto call = reinterpret_cast<GuestThunk>(thunk);
	const uint64_t results = call() + call() + call();
	uint64_t calls = 0;
	uint64_t first = 0;
	Check(results == 0, "thunk keeps returning zero");
	Check(Loader::UnresolvedImportLedgerForTest(thunk, &calls, &first) && calls == 3, "per-record call count");
#if defined(_WIN32)
	char module[49];
	uint64_t offset = 0;
	Check(Common::Diagnostics::HostModuleOf(first, module, sizeof(module), &offset) &&
	          std::strstr(module, "unresolved_import_ledger_tests") != nullptr,
	      "first caller is the calling code");
#endif
	char symbol[65];
	Common::Diagnostics::SanitizeSymbolName("AbC+-#A#B/../x y", symbol, sizeof(symbol));
	Check(std::strcmp(symbol, "AbC+-#A#B_.._x_y") == 0, "symbol sanitization keeps the NID alphabet");
	if (argc == 2 && std::strcmp(argv[1], "--report") == 0) {
		Loader::ReportUnresolvedImportsForTest();
	}
	if (g_failures == 0) std::puts("UnresolvedImportLedgerTests: all cases passed");
	return g_failures == 0 ? 0 : 1;
}

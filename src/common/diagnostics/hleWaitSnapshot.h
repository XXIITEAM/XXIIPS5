#ifndef XXII_COMMON_DIAGNOSTICS_HLE_WAIT_SNAPSHOT_H_
#define XXII_COMMON_DIAGNOSTICS_HLE_WAIT_SNAPSHOT_H_

// Opt-in host-side cross-process snapshot of in-flight HLE waits.
// Only operation, call ID, thread ID, mode and monotonic age are retained.
// No guest addresses, parameters, files, stacks or shader contents.
// Windows Interlocked provides cross-process synchronization of this mapping.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include "common/diagnostics/runtimeTelemetry.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common::HleWaitSnapshot {
constexpr uint32_t Magic = 0x57583232;
constexpr uint32_t Version = 1;
constexpr uint32_t Capacity = 256;
constexpr uint32_t TopWaits = 12;

enum class Operation : uint32_t {
    Unknown, WaitEqueue, Semaphore, PthreadSemaphore, Condition, Address32, Address64
};
enum class Mode : uint32_t { Unknown, Infinite, Timed };

inline Operation DecodeOperation(const char* name) noexcept {
    if (!name) return Operation::Unknown;
    if (std::strcmp(name, "wait_equeue") == 0) return Operation::WaitEqueue;
    if (std::strcmp(name, "sem_wait") == 0) return Operation::Semaphore;
    if (std::strcmp(name, "pthread_sem_wait") == 0) return Operation::PthreadSemaphore;
    if (std::strcmp(name, "pthread_cond_wait") == 0) return Operation::Condition;
    if (std::strcmp(name, "sync_wait32") == 0) return Operation::Address32;
    if (std::strcmp(name, "sync_wait64") == 0) return Operation::Address64;
    return Operation::Unknown;
}
inline const char* OperationName(Operation op) noexcept {
    switch (op) {
        case Operation::WaitEqueue: return "wait_equeue";
        case Operation::Semaphore: return "sem_wait";
        case Operation::PthreadSemaphore: return "pthread_sem_wait";
        case Operation::Condition: return "pthread_cond_wait";
        case Operation::Address32: return "sync_wait32";
        case Operation::Address64: return "sync_wait64";
        default: return "unknown";
    }
}
inline Mode DecodeMode(const char* mode) noexcept {
    return mode && std::strcmp(mode, "infinite") == 0 ? Mode::Infinite :
           mode && std::strcmp(mode, "timed") == 0 ? Mode::Timed : Mode::Unknown;
}
inline const char* ModeName(Mode mode) noexcept {
    return mode == Mode::Infinite ? "infinite" :
           mode == Mode::Timed ? "timed" : "unknown";
}

#if defined(_WIN32)
struct alignas(8) Slot {
    volatile LONG state; // 0 free / 1 writing / 2 active / 3 closing
    volatile LONG operation;
    volatile LONG mode;
    volatile LONG thread;
    alignas(8) volatile LONG64 call;
    alignas(8) volatile LONG64 startedMs;
};
struct alignas(8) Table {
    uint32_t magic, version, capacity;
    volatile LONG guestConnected;
    volatile LONG cursor, dropped;
    Slot records[Capacity];
};
static_assert(std::is_trivially_copyable_v<Table> && std::is_standard_layout_v<Table>);
static_assert(sizeof(Table) < 65536);
inline void Initialize(Table* table) noexcept {
    if (!table) return;
    std::memset(table, 0, sizeof(Table)); // before child opens view
    table->magic = Magic;
    table->version = Version;
    table->capacity = Capacity;
}
inline bool Valid(const Table* table) noexcept {
    return table && table->magic == Magic && table->version == Version &&
           table->capacity == Capacity;
}
struct Ticket { Slot* slot = nullptr; };

inline Table* OpenNamed(const wchar_t* name) noexcept {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!mapping) return nullptr;
    auto* table = static_cast<Table*>(MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Table)));
    CloseHandle(mapping);
    if (!Valid(table)) {
        if (table) UnmapViewOfFile(table);
        return nullptr;
    }
    return table; // valid until guest exit; the OS unmaps the view
}
inline Table* RuntimeTable() noexcept {
#if !KYTY_DIAGNOSTIC_RUNTIME
    return nullptr;
#else
    static Table* table = []() -> Table* {
        char* name = nullptr;
        size_t length = 0;
        if (_dupenv_s(&name, &length, "XXII_COMPAT_HLE_WAIT_MAP") != 0 ||
            !name) return nullptr;
        constexpr char prefix[] = "Local\\PSVXXII-WAITS-";
        const size_t prefixLen = sizeof(prefix) - 1;
        Table* result = nullptr;
        if (length >= prefixLen + 8 && length < 115 &&
            std::strncmp(name, prefix, prefixLen) == 0) {
            wchar_t wide[128] {};
            size_t i = 0;
            for (; name[i] && i < 127; ++i) {
                const char c = name[i];
                const bool safe =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '\\';
                if (!safe) break;
                wide[i] = static_cast<unsigned char>(c);
            }
            if (name[i] == '\0') {
                result = OpenNamed(wide);
                if (result) InterlockedExchange(&result->guestConnected, 1);
            }
        }
        std::free(name);
        return result;
    }();
    return table;
#endif
}
inline Ticket Enter(Table* table, uint64_t call, Operation op, Mode mode,
                    uint64_t tickMs, uint32_t tid) noexcept {
    if (!Valid(table) || !call || op == Operation::Unknown) return {};
    const auto cursor = static_cast<uint32_t>(InterlockedIncrement(&table->cursor) - 1);
    // Usually a free slot is found in the first few contiguous probes.
    // When long-lived waits form a dense cluster, search another 32 widely
    // distributed slots (stride 37, coprime with 256). This reduces false
    // acquisition misses without scanning the entire table on the hot path.
    // All paths stay bounded and lock-free; worst case 56 CAS attempts.
    const auto claim = [&](uint32_t index) noexcept -> Ticket {
        auto* slot = &table->records[index % Capacity];
        if (InterlockedCompareExchange(&slot->state, 1, 0) != 0) return {};
        InterlockedExchange(&slot->operation, static_cast<LONG>(op));
        InterlockedExchange(&slot->mode, static_cast<LONG>(mode));
        InterlockedExchange(&slot->thread, static_cast<LONG>(tid));
        InterlockedExchange64(&slot->call, static_cast<LONG64>(call));
        InterlockedExchange64(&slot->startedMs, static_cast<LONG64>(tickMs));
        InterlockedExchange(&slot->state, 2);
        return {slot};
    };
    for (uint32_t attempt = 0; attempt < 24; ++attempt)
        if (auto ticket = claim(cursor + attempt); ticket.slot) return ticket;
    for (uint32_t attempt = 1; attempt <= 32; ++attempt)
        if (auto ticket = claim(cursor + attempt * 37u); ticket.slot) return ticket;
    InterlockedIncrement(&table->dropped);
    return {};
}
inline void Leave(Ticket ticket) noexcept {
    if (!ticket.slot) return;
    InterlockedExchange(&ticket.slot->state, 3);
    InterlockedExchange(&ticket.slot->state, 0);
}
struct Sample {
    Operation operation = Operation::Unknown;
    Mode mode = Mode::Unknown;
    uint32_t thread = 0;
    uint64_t call = 0;
    uint64_t ageMs = 0;
};
struct Snapshot {
    uint32_t outstanding = 0, omitted = 0, dropped = 0, count = 0;
    bool connected = false;
    std::array<Sample, TopWaits> oldest {};
};
inline Snapshot Inspect(const Table* table, uint64_t tickMs) noexcept {
    Snapshot result {};
    if (!Valid(table)) return result;
    const auto r32 = [](const volatile LONG* p) {
        return InterlockedCompareExchange(const_cast<volatile LONG*>(p), 0, 0);
    };
    const auto r64 = [](const volatile LONG64* p) {
        return InterlockedCompareExchange64(const_cast<volatile LONG64*>(p), 0, 0);
    };
    result.dropped = static_cast<uint32_t>(r32(&table->dropped));
    result.connected = r32(&table->guestConnected) != 0;
    for (const auto& slot : table->records) {
        if (r32(&slot.state) != 2) continue;
        const auto call = static_cast<uint64_t>(r64(&slot.call));
        const auto op = static_cast<Operation>(r32(&slot.operation));
        const auto mode = static_cast<Mode>(r32(&slot.mode));
        const auto thread = static_cast<uint32_t>(r32(&slot.thread));
        const auto started = static_cast<uint64_t>(r64(&slot.startedMs));
        if (call == 0 || r32(&slot.state) != 2 ||
            static_cast<uint64_t>(r64(&slot.call)) != call ||
            op == Operation::Unknown || started > tickMs) continue;
        const Sample sample {op, mode, thread, call, tickMs - started};
        ++result.outstanding;
        if (result.count == TopWaits &&
            sample.ageMs <= result.oldest[result.count - 1].ageMs) {
            ++result.omitted;
            continue;
        }
        uint32_t pos = 0;
        while (pos < result.count && result.oldest[pos].ageMs >= sample.ageMs) ++pos;
        if (result.count < TopWaits) ++result.count;
        else ++result.omitted;
        for (uint32_t n = result.count - 1; n > pos; --n)
            result.oldest[n] = result.oldest[n - 1];
        result.oldest[pos] = sample;
    }
    return result;
}
#else
struct Table {};
struct Ticket {};
#endif
} // namespace Common::HleWaitSnapshot
#endif // XXII_COMMON_DIAGNOSTICS_HLE_WAIT_SNAPSHOT_H_

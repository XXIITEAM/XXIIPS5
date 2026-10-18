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
// V3: nonce/PID-bound, bounded per-equeue event delivery counters.
constexpr uint32_t Version = 3;
constexpr uint32_t Capacity = 256;
constexpr uint32_t TopWaits = 12;
constexpr uint32_t QueueCapacity = 64;
constexpr uint32_t TopQueues = 16;

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
    volatile LONG queueSlot; // session-only ordinal, 0 means unattributed
    alignas(8) volatile LONG64 call;
    alignas(8) volatile LONG64 startedMs;
    alignas(8) volatile LONG64 generation; // detect slot reuse during a snapshot
};
// The ordinal is local to the current guest process, never a guest queue
// handle, name, address or kernel event key. Slots are NEVER reused.
struct alignas(8) QueueCounters {
    volatile LONG registered;
    alignas(8) volatile LONG64 addSignals;
    alignas(8) volatile LONG64 triggerSignals;
    alignas(8) volatile LONG64 waitCalls;
    alignas(8) volatile LONG64 waitEventReturns;
    alignas(8) volatile LONG64 waitEmptyReturns;
    alignas(8) volatile LONG64 deliveredEvents;
    alignas(8) volatile LONG64 timerExpirations;
};
enum class QueueEvent : uint32_t {
    AddSignal, TriggerSignal, WaitCall, WaitEvents, WaitEmpty, Delivered, TimerExpired
};
struct alignas(8) Table {
    uint32_t magic, version, capacity;
    uint32_t reserved = 0;
    alignas(8) uint64_t nonceHigh, nonceLow; // parent-minted, never exported
    volatile LONG guestPid;                 // claimed by nonce-bound child
    volatile LONG guestAuthenticated;
    volatile LONG guestConnected;           // published LAST by guest
    volatile LONG cursor, dropped;
    volatile LONG queueCursor, queueOverflow;
    Slot records[Capacity];
    QueueCounters queues[QueueCapacity];
};
static_assert(std::is_trivially_copyable_v<Table> && std::is_standard_layout_v<Table>);
static_assert(sizeof(Table) < 65536);
inline void Initialize(Table* table, uint64_t nonceHigh, uint64_t nonceLow) noexcept {
    if (!table) return;
    std::memset(table, 0, sizeof(Table)); // before child opens view
    table->nonceHigh = nonceHigh;
    table->nonceLow = nonceLow;
    table->magic = Magic;
    table->version = Version;
    table->capacity = Capacity;
}
// Exactly 128 bits, hexadecimal; the token itself must never enter a log,
// evidence archive, Knowledge Core or UI view.
inline bool ParseNonce(const char* text, uint64_t* high, uint64_t* low) noexcept {
    if (!text || !high || !low || std::strlen(text) != 32) return false;
    uint64_t a = 0, b = 0;
    for (int i = 0; i < 32; ++i) {
        const char c = text[i];
        const unsigned value = c >= '0' && c <= '9' ? unsigned(c - '0') :
                               c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10) :
                               c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10) : 16u;
        if (value == 16u) return false;
        if (i < 16) a = (a << 4) | value;
        else b = (b << 4) | value;
    }
    if ((a | b) == 0) return false;
    *high = a;
    *low = b;
    return true;
}
inline bool Valid(const Table* table) noexcept {
    return table && table->magic == Magic && table->version == Version &&
           table->capacity == Capacity;
}
// Binding asserts possession of this session's launch challenge. This is
// NOT cryptographic attestation against arbitrary same-user processes.
inline bool BindGuest(Table* table, uint64_t high, uint64_t low,
                      uint32_t pid) noexcept {
    if (!Valid(table) || !pid || ((high | low) == 0) ||
        ((table->nonceHigh ^ high) | (table->nonceLow ^ low))) return false;
    if (InterlockedCompareExchange(&table->guestPid, static_cast<LONG>(pid), 0) != 0)
        return false;
    InterlockedExchange(&table->guestAuthenticated, 1);
    InterlockedExchange(&table->guestConnected, 1);
    return true;
}
struct Ticket { Slot* slot = nullptr; };
// The producer owns this anonymous ordinal. A full table must return 0,
// NEVER alias another live queue or wrap a 32-bit index.
inline uint32_t RegisterQueue(Table* table) noexcept {
    if (!Valid(table)) return 0;
    const LONG next = InterlockedIncrement(&table->queueCursor);
    if (next < 1 || next > static_cast<LONG>(QueueCapacity)) {
        InterlockedIncrement(&table->queueOverflow);
        return 0;
    }
    InterlockedExchange(&table->queues[next - 1].registered, 1);
    return static_cast<uint32_t>(next);
}
inline void CountQueue(Table* table, uint32_t ordinal, QueueEvent event,
                       uint32_t amount = 1) noexcept {
    if (!Valid(table) || ordinal == 0 || ordinal > QueueCapacity ||
        amount == 0 || amount > 4096) return;
    auto* q = &table->queues[ordinal - 1];
    if (InterlockedCompareExchange(&q->registered, 0, 0) != 1) return;
    volatile LONG64* field = nullptr;
    switch (event) {
        case QueueEvent::AddSignal: field = &q->addSignals; break;
        case QueueEvent::TriggerSignal: field = &q->triggerSignals; break;
        case QueueEvent::WaitCall: field = &q->waitCalls; break;
        case QueueEvent::WaitEvents: field = &q->waitEventReturns; break;
        case QueueEvent::WaitEmpty: field = &q->waitEmptyReturns; break;
        case QueueEvent::Delivered: field = &q->deliveredEvents; break;
        case QueueEvent::TimerExpired: field = &q->timerExpirations; break;
    }
    if (field) InterlockedExchangeAdd64(field, static_cast<LONG64>(amount));
}
inline void AnnotateQueue(Ticket ticket, uint32_t ordinal) noexcept {
    if (!ticket.slot || ordinal == 0 || ordinal > QueueCapacity) return;
    if (InterlockedCompareExchange(&ticket.slot->state, 0, 0) != 2) return;
    if (InterlockedCompareExchange(&ticket.slot->operation, 0, 0) !=
        static_cast<LONG>(Operation::WaitEqueue)) return;
    InterlockedExchange(&ticket.slot->queueSlot, static_cast<LONG>(ordinal));
}

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
                if (result) {
                    char* launchNonce = nullptr;
                    size_t nonceLen = 0;
                    uint64_t high = 0, low = 0;
                    const bool ok =
                        _dupenv_s(&launchNonce, &nonceLen,
                                  "XXII_COMPAT_HLE_WAIT_NONCE") == 0 &&
                        ParseNonce(launchNonce, &high, &low) &&
                        BindGuest(result, high, low, GetCurrentProcessId());
                    std::free(launchNonce);
                    if (!ok) { UnmapViewOfFile(result); result = nullptr; }
                }
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
        InterlockedExchange(&slot->queueSlot, 0);
        InterlockedExchange64(&slot->call, static_cast<LONG64>(call));
        InterlockedExchange64(&slot->startedMs, static_cast<LONG64>(tickMs));
        // A reused slot can transition active->free->active while a reader
        // copies fields. A generation change makes such ABA races visible.
        InterlockedIncrement64(&slot->generation);
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
    uint32_t queueSlot = 0;
    uint64_t call = 0;
    uint64_t ageMs = 0;
};
struct Snapshot {
    uint32_t outstanding = 0, omitted = 0, dropped = 0, count = 0;
    uint32_t unstable = 0, invalid = 0;
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
    result.connected = r32(&table->guestConnected) == 1 &&
                       r32(&table->guestAuthenticated) == 1 &&
                       r32(&table->guestPid) != 0;
    for (const auto& slot : table->records) {
        if (r32(&slot.state) != 2) continue;
        const auto epoch = r64(&slot.generation);
        const auto call = static_cast<uint64_t>(r64(&slot.call));
        const auto op = static_cast<Operation>(r32(&slot.operation));
        const auto mode = static_cast<Mode>(r32(&slot.mode));
        const auto thread = static_cast<uint32_t>(r32(&slot.thread));
        const auto queueSlot = static_cast<uint32_t>(r32(&slot.queueSlot));
        const auto started = static_cast<uint64_t>(r64(&slot.startedMs));
        if (r32(&slot.state) != 2 || r64(&slot.generation) != epoch) {
            ++result.unstable;
            continue;
        }
        if (call == 0 || op == Operation::Unknown ||
            (mode != Mode::Infinite && mode != Mode::Timed) ||
            queueSlot > QueueCapacity ||
            (queueSlot != 0 && op != Operation::WaitEqueue) ||
            started > tickMs) {
            ++result.invalid;
            continue;
        }
        const Sample sample {op, mode, thread, queueSlot, call, tickMs - started};
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
// Read with Interlocked operations only; values can advance between fields.
// Never interpret two counters as an atomic guest-wide point in time.
struct QueueSample {
    uint32_t ordinal = 0;
    uint64_t addSignals = 0, triggerSignals = 0, waitCalls = 0;
    uint64_t waitEventReturns = 0, waitEmptyReturns = 0;
    uint64_t deliveredEvents = 0, timerExpirations = 0;
};
struct QueueSnapshot {
    uint32_t created = 0, overflow = 0, omitted = 0, invalid = 0, count = 0;
    std::array<QueueSample, TopQueues> busiest {};
};
inline QueueSnapshot InspectQueues(const Table* table) noexcept {
    QueueSnapshot result {};
    if (!Valid(table)) return result;
    const auto r32 = [](const volatile LONG* p) -> LONG {
        return InterlockedCompareExchange(const_cast<volatile LONG*>(p), 0, 0);
    };
    const auto r64 = [](const volatile LONG64* p) -> LONG64 {
        return InterlockedCompareExchange64(const_cast<volatile LONG64*>(p), 0, 0);
    };
    const LONG count = r32(&table->queueCursor);
    const LONG overflow = r32(&table->queueOverflow);
    if (count < 0 || overflow < 0) { ++result.invalid; return result; }
    result.created = static_cast<uint32_t>(count);
    result.overflow = static_cast<uint32_t>(overflow);
    for (uint32_t i = 0; i < QueueCapacity; ++i) {
        const auto& source = table->queues[i];
        if (r32(&source.registered) != 1) continue;
        const LONG64 fields[] {r64(&source.addSignals), r64(&source.triggerSignals),
            r64(&source.waitCalls), r64(&source.waitEventReturns),
            r64(&source.waitEmptyReturns), r64(&source.deliveredEvents),
            r64(&source.timerExpirations)};
        bool valid = true;
        for (const LONG64 v : fields) if (v < 0) valid = false;
        if (!valid) { ++result.invalid; continue; }
        const QueueSample row {i + 1,
            static_cast<uint64_t>(fields[0]), static_cast<uint64_t>(fields[1]),
            static_cast<uint64_t>(fields[2]), static_cast<uint64_t>(fields[3]),
            static_cast<uint64_t>(fields[4]), static_cast<uint64_t>(fields[5]),
            static_cast<uint64_t>(fields[6])};
        // Prefer active wait queues to high-volume unrelated event producers.
        // This is an observation priority only, never cause attribution.
        const auto rank = [](const QueueSample& q) {
            return q.waitCalls;
        };
        if (result.count == TopQueues &&
            rank(row) <= rank(result.busiest[result.count - 1])) {
            ++result.omitted;
            continue;
        }
        uint32_t at = 0;
        while (at < result.count &&
               rank(result.busiest[at]) >= rank(row)) ++at;
        if (result.count < TopQueues) ++result.count;
        else ++result.omitted;
        for (uint32_t n = result.count - 1; n > at; --n)
            result.busiest[n] = result.busiest[n - 1];
        result.busiest[at] = row;
    }
    return result;
}
#else
struct Table {};
struct Ticket {};
#endif
} // namespace Common::HleWaitSnapshot
#endif // XXII_COMMON_DIAGNOSTICS_HLE_WAIT_SNAPSHOT_H_

#ifndef XXII_COMMON_DIAGNOSTICS_HLE_TRACE_H_
#define XXII_COMMON_DIAGNOSTICS_HLE_TRACE_H_

// Opt-in, host-side PS5 HLE diagnostic metadata. No guest memory is copied.
// A matching enter/exit pair shows a wrapper returned, not whether a guest
// thread made useful progress. Missing exit can also mean intentional wait.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <thread>
#include "common/diagnostics/hleWaitSnapshot.h"
#include "common/diagnostics/runtimeTelemetry.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common::HleTrace {

inline bool Enabled() noexcept {
#if !KYTY_DIAGNOSTIC_RUNTIME
    return false;
#else
    static const bool enabled = [] {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "XXII_COMPAT_HLE_TRIAGE") != 0)
            return false;
        const bool result = value && value[0] == '1' && value[1] == '\0';
        std::free(value);
        return result;
#else
        const char* value = std::getenv("XXII_COMPAT_HLE_TRIAGE");
        return value && value[0] == '1' && value[1] == '\0';
#endif
    }();
    return enabled;
#endif
}

inline uint32_t ThreadToken() noexcept {
    // A process-local thread correlation token, not a guest CPU address.
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

inline uint64_t NextCall() noexcept {
    static std::atomic<uint64_t> sequence {0};
    return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Requested diagnostic verbosity is frozen once at process launch.
// 0=ERROR, 1=WARNING(default), 2=INFO, 3=TRACE. Keep the value host-only.
inline int DetailLevel() noexcept {
    static const int level = [] {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "XXII_COMPAT_HLE_LEVEL") != 0) return 1;
        const int result = value && value[0] >= '0' && value[0] <= '3' &&
                           value[1] == '\0' ? value[0] - '0' : 1;
        std::free(value);
        return result;
#else
        const char* value = std::getenv("XXII_COMPAT_HLE_LEVEL");
        return value && value[0] >= '0' && value[0] <= '3' && value[1] == '\0'
                   ? value[0] - '0' : 1;
#endif
    }();
    return level;
}

// High-frequency synchronization wrappers must not flood the QProcess pipe.
// All wrappers are still timed, even when they were not sampled for enter/exit.
// Thus an unsampled slow return remains observable as a compact outlier.
inline bool SampleCall(uint64_t call, int detail = 1) noexcept {
    if (call == 0) return false;
    switch (detail) {
        case 0: return call <= 24 || call % 1024 == 0;
        case 1: return call <= 96 || call % 512 == 0;
        case 2: return call <= 512 || call % 128 == 0;
        default: return call <= 4096 || call % 32 == 0;
    }
}

// The UI owns a manual-reset Windows event and signals it only when the
// diagnostic detector sees a serious error or stalled presentation. Checking
// it every 32 HLE wrappers avoids an additional kernel call per guest call.
inline bool FocusBoostActive() noexcept {
#if defined(_WIN32)
    static HANDLE focus = []() -> HANDLE {
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "XXII_COMPAT_HLE_FOCUS_EVENT") != 0 ||
            value == nullptr) return nullptr;
        constexpr char prefix[] = "Local\\PSVXXII-HLE-";
        const size_t prefixLen = sizeof(prefix) - 1;
        HANDLE result = nullptr;
        if (length >= prefixLen + 8 && length < 115 &&
            std::strncmp(value, prefix, prefixLen) == 0) {
            wchar_t name[128] {};
            size_t i = 0;
            for (; value[i] && i < 127; ++i) {
                const char c = value[i];
                const bool allowed =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                    c == '\\';
                if (!allowed) break;
                name[i] = static_cast<unsigned char>(c);
            }
            if (value[i] == '\0') result = OpenEventW(SYNCHRONIZE, FALSE, name);
        }
        std::free(value);
        return result;
    }();
    if (!focus) return false;
    thread_local unsigned int probeCount = 0;
    thread_local bool cached = false;
    if ((++probeCount & 31u) == 0) {
        const bool wasActive = cached;
        cached = WaitForSingleObject(focus, 0) == WAIT_OBJECT_0;
        if (cached && !wasActive) {
            // A few acknowledgements establish that the *child* saw the
            // requested focus event; parent signalling alone is not proof.
            static std::atomic<unsigned int> acknowledgments {0};
            if (acknowledgments.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::fprintf(stderr, "XXII_HLE_FOCUS state=on detail=3 tid=%u\n", ThreadToken());
                std::fflush(stderr);
            }
        }
    }
    return cached;
#else
    return false;
#endif
}

inline bool SampleOutlier() noexcept {
    static std::atomic<uint64_t> outliers {0};
    const auto count = outliers.fetch_add(1, std::memory_order_relaxed) + 1;
    return count <= 64 || count % 128 == 0;
}

#if KYTY_DIAGNOSTIC_RUNTIME
class Span {
public:
    Span(const char* operation, const char* mode) noexcept: m_operation(operation), m_mode(mode) {
        if (!Enabled()) return;
        m_call = NextCall();
        m_started = std::chrono::steady_clock::now();
#if defined(_WIN32)
        // A tiny shared slot tracks every in-flight opt-in HLE wrapper,
        // even if its stderr enter/exit pair is not sampled.
        m_waitTicket = HleWaitSnapshot::Enter(
            HleWaitSnapshot::RuntimeTable(), m_call,
            HleWaitSnapshot::DecodeOperation(m_operation),
            HleWaitSnapshot::DecodeMode(m_mode), GetTickCount64(), GetCurrentThreadId());
#endif
        m_active = SampleCall(m_call, FocusBoostActive() ? 3 : DetailLevel());
        if (!m_active) return;
        m_thread = ThreadToken();
        std::fprintf(stderr, "XXII_HLE_CALL phase=enter op=%s mode=%s call=%" PRIu64
                             " tid=%u\n", m_operation, m_mode, m_call, m_thread);
        std::fflush(stderr);
    }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    void Finish(int64_t code) noexcept {
        m_result = code;
        m_finished = true;
    }
    ~Span() noexcept {
        if (m_call == 0) return;
#if defined(_WIN32)
        HleWaitSnapshot::Leave(m_waitTicket);
#endif
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_started).count();
        if (!m_active) {
            // Do not mistake a slow but successful indefinite wait for proof
            // of a deadlock; correlate it with presentation, audio and GPU.
            if (m_finished && (elapsed >= 3000000 ||
                               (DetailLevel() >= 2 && m_result != 0)) &&
                SampleOutlier()) {
                std::fprintf(stderr, "XXII_HLE_OUTLIER op=%s mode=%s call=%" PRIu64
                                     " tid=%u result=%" PRId64 " elapsed_us=%" PRId64 "\n",
                             m_operation, m_mode, m_call, ThreadToken(), m_result, elapsed);
                std::fflush(stderr);
            }
            return;
        }
        if (m_finished) {
            std::fprintf(stderr, "XXII_HLE_CALL phase=exit op=%s mode=%s call=%" PRIu64
                                 " tid=%u result=%" PRId64 " elapsed_us=%" PRId64 "\n",
                         m_operation, m_mode, m_call, m_thread, m_result, elapsed);
        } else {
            std::fprintf(stderr, "XXII_HLE_CALL phase=exit op=%s mode=%s call=%" PRIu64
                                 " tid=%u result_unknown=1 elapsed_us=%" PRId64 "\n",
                         m_operation, m_mode, m_call, m_thread, elapsed);
        }
        std::fflush(stderr);
    }
private:
    const char* m_operation;
    const char* m_mode;
    uint64_t m_call = 0;
    uint32_t m_thread = 0;
#if defined(_WIN32)
    HleWaitSnapshot::Ticket m_waitTicket {};
#endif
    std::chrono::steady_clock::time_point m_started {};
    int64_t m_result = 0;
    bool m_finished = false;
    bool m_active = false;
};
#else
class Span {
public:
    Span(const char*, const char*) noexcept {}
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    void Finish(int64_t) noexcept {}
};
#endif

inline const char* SourceBase(const char* file, char (&output)[49]) noexcept {
    if (!file) return "unknown";
    const char* base = file;
    for (const char* it = file; *it; ++it)
        if (*it == '/' || *it == '\\') base = it + 1;
    size_t size = 0;
    for (const char* it = base; *it && *it != '.' && size < 48; ++it) {
        const char c = *it;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            break;
        output[size++] = c;
    }
    output[size] = '\0';
    return size ? output : "unknown";
}

inline void Failure(const char* category, const char* file, int line) noexcept {
    if (!Enabled()) return;
    char source[49] {};
    std::fprintf(stderr, "XXII_HLE_FAILURE kind=%s source=%s line=%d tid=%u\n",
                 category, SourceBase(file, source), line, ThreadToken());
    std::fflush(stderr);
}

inline uint64_t SymbolFingerprint(std::string_view value) noexcept {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char c: value)
        hash = (hash ^ c) * UINT64_C(1099511628211);
    return hash;
}

inline void UnresolvedImport(std::string_view symbol, uint32_t index) noexcept {
    if (!Enabled()) return;
    // Preserve only validated NID characters and a stable opaque fingerprint.
    // File paths, relocation addresses and arbitrary symbol text are omitted.
    char nid[14] {};
    size_t n = 0;
    for (const char c: symbol) {
        if (n >= 11 || c == '#' || c == '[') break;
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                          c == '_' || c == '/';
        if (!safe) { n = 0; break; }
        nid[n++] = c;
    }
    if (n != 11) { nid[0] = 'n'; nid[1] = 'a'; nid[2] = '\0'; }
    std::fprintf(stderr, "XXII_HLE_UNRESOLVED nid=%s symbol_id=0x%016" PRIx64
                         " relocation=%u tid=%u return=zero_stub\n",
                 nid, SymbolFingerprint(symbol), index, ThreadToken());
    std::fflush(stderr);
}

// A failed dlsym query is often optional capability discovery, not a fault.
// Keep rare examples and an identifiable global sampling sequence, without
// emitting hundreds of harmless individual lines per second.
inline bool SampleOptionalLookup(uint64_t sequence, int detail) noexcept {
    if (sequence == 0) return false;
    switch (detail) {
        case 0: return sequence <= 8 || sequence % 256 == 0;
        case 1: return sequence <= 24 || sequence % 128 == 0;
        case 2: return sequence <= 64 || sequence % 32 == 0;
        default: return sequence <= 128 || sequence % 8 == 0;
    }
}

inline void DynamicSymbolFailure(std::string_view symbol, int32_t module,
                                 const char* reason) noexcept {
    if (!Enabled()) return;
    static std::atomic<uint64_t> lookups {0};
    const uint64_t sequence = lookups.fetch_add(1, std::memory_order_relaxed) + 1;
    const int detail = FocusBoostActive() ? 3 : DetailLevel();
    if (!SampleOptionalLookup(sequence, detail)) return;
    std::fprintf(stderr, "XXII_HLE_DLSYM_FAILURE reason=%s symbol_id=0x%016" PRIx64
                         " handle=%" PRId32 " tid=%u sample_seq=%" PRIu64 "\n",
                 reason, SymbolFingerprint(symbol), module, ThreadToken(), sequence);
    std::fflush(stderr);
}

} // namespace Common::HleTrace

#endif // XXII_COMMON_DIAGNOSTICS_HLE_TRACE_H_
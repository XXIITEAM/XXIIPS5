#pragma once

// PSVXXII's per-runtime output gain, applied before SDL playback begins.
// This does NOT modify the Windows master mixer, default endpoint or other
// processes. Only an explicitly armed Debug Center launch sets the env var.
#include <cstdint>
#include <cstdlib>

namespace Common::DebugAudio {
constexpr int ParsePercent(const char* value) noexcept {
    if (!value || !*value) return 100;
    int parsed = 0, digits = 0;
    for (const char* it = value; *it; ++it) {
        if (*it < '0' || *it > '9' || ++digits > 3) return 100;
        parsed = parsed * 10 + (*it - '0');
    }
    return parsed <= 100 ? parsed : 100;
}
inline int OutputPercent() noexcept {
    static const int percent = []() noexcept {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, "XXII_DEBUG_AUDIO_PERCENT") != 0) return 100;
        const int parsed = ParsePercent(value);
        std::free(value);
        return parsed;
#else
        return ParsePercent(std::getenv("XXII_DEBUG_AUDIO_PERCENT"));
#endif
    }();
    return percent;
}
inline float OutputScale() noexcept {
    return static_cast<float>(OutputPercent()) / 100.0f;
}
} // namespace Common::DebugAudio

#pragma once
// PSVXXII Developer: bounded private owner-allocation generation tracker.
// All calls must hold the kernel's memory-operation mutex. Base addresses
// are used as internal comparison keys ONLY; Event/Summary contain no
// address, pointer, name, path, memory bytes, shader or game information.
// This is deliberately not a full PS5 or Win32 allocation tracker.
#include <array>
#include <cstdint>

namespace Common::Diagnostics {

class RuntimeAllocationLedger final {
public:
    static constexpr uint32_t MaxSlots = 64;
    static constexpr uint32_t MaxReceipts = 128;

    enum class Kind : uint8_t { None, Code, Runtime, Stack };
    enum class Action : uint8_t { None, Allocate, Free, Partial, Limit };

    struct Event {
        Action action = Action::None;
        uint32_t slot = 0; // sequence-local pseudonym, NEVER a pointer
        uint32_t id = 0;   // unique lifetime, not stable across runs
        uint32_t generation = 0;
        uint32_t pages = 0;
        Kind kind = Kind::None;
        bool reused = false;
    };
    struct Summary {
        uint32_t allocations = 0;
        uint32_t releases = 0;
        uint32_t reuses = 0;
        uint32_t active = 0;
        uint32_t highest_generation = 0;
        uint32_t receipts = 0;
        bool capped = false;
        bool partial_or_unknown = false;
    };

    void reset() noexcept { *this = RuntimeAllocationLedger{}; }
    Event allocated(uint64_t address, uint64_t bytes, Kind kind) noexcept {
        if (m_stopped || address == 0 || bytes == 0 || bytes > UINT64_MAX - address ||
            (bytes & 0x3fffu) != 0 || kind == Kind::None)
            return {};
        if (bytes / 0x4000u > 999999999u) return limit();
        // Reject overlapping active records rather than pretending a new
        // lifetime is independent. Guest allocation should have ruled this out.
        for (const auto& item : m_slots) {
            if (item.used && item.active && overlaps(item.address, item.bytes, address, bytes))
                return limit();
        }
        uint32_t index = MaxSlots;
        bool reused = false;
        for (uint32_t i = 0; i < MaxSlots; ++i) {
            auto& item = m_slots[i];
            if (item.used && !item.active &&
                item.address == address &&
                !item.invalidated) {
                index = i;
                reused = true;
                break;
            }
        }
        if (index == MaxSlots) {
            for (uint32_t i = 0; i < MaxSlots; ++i) {
                if (!m_slots[i].used) { index = i; break; }
            }
        }
        if (index == MaxSlots || m_nextId >= 2147483647u) return limit();
        auto& item = m_slots[index];
        if (item.invalidated) return limit();
        if (reused && item.generation >= 2147483647u) return limit();
        Event event;
        event.action = Action::Allocate;
        event.slot = index + 1;
        event.id = ++m_nextId;
        event.generation = reused ? item.generation + 1 : 1;
        event.pages = static_cast<uint32_t>(bytes / 0x4000u);
        event.kind = kind;
        event.reused = reused;
        if (!takeSlot()) return limit();
        item.used = true;
        item.active = true;
        item.address = address;
        item.bytes = bytes;
        item.kind = kind;
        item.id = event.id;
        item.generation = event.generation;
        ++m_allocations;
        if (reused) ++m_reuses;
        return event;
    }
    Event released(uint64_t address, uint64_t bytes) noexcept {
        if (m_stopped || address == 0 || bytes == 0 || bytes > UINT64_MAX - address)
            return {};
        for (uint32_t i = 0; i < MaxSlots; ++i) {
            auto& item = m_slots[i];
            if (!item.used || !item.active ||
                !overlaps(item.address, item.bytes, address, bytes))
                continue;
            Event event;
            event.slot = i + 1;
            event.id = item.id;
            event.generation = item.generation;
            event.kind = item.kind;
            if (item.address != address || item.bytes != bytes) {
                // A partially released owner no longer has a verified
                // full-allocation lifetime. Stop at this boundary.
                event.action = Action::Partial;
                item.invalidated = true;
                item.active = false;
                m_partial = true;
                m_stopped = true;
                return event;
            }
            if (!takeSlot()) return limit();
            item.active = false;
            ++m_releases;
            event.action = Action::Free;
            return event;
        }
        return {}; // Untracked owner; do not claim a successful FREE.
    }
    Summary summary() const noexcept {
        Summary s;
        s.allocations = m_allocations;
        s.releases = m_releases;
        s.reuses = m_reuses;
        s.receipts = m_receipts;
        s.capped = m_capped;
        s.partial_or_unknown = m_partial;
        for (const auto& item : m_slots) {
            if (item.active) ++s.active;
            if (item.generation > s.highest_generation)
                s.highest_generation = item.generation;
        }
        return s;
    }

private:
    struct Slot {
        uint64_t address = 0;
        uint64_t bytes = 0;
        uint32_t id = 0;
        uint32_t generation = 0;
        Kind kind = Kind::None;
        bool used = false;
        bool active = false;
        bool invalidated = false;
    };
    static bool overlaps(uint64_t a, uint64_t asize, uint64_t b, uint64_t bsize) noexcept {
        return a < b + bsize && b < a + asize; // both ranges validated above
    }
    bool takeSlot() noexcept {
        if (m_receipts == MaxReceipts) return false;
        ++m_receipts;
        return true;
    }
    Event limit() noexcept {
        if (m_stopped) return {};
        m_stopped = true;
        m_capped = true;
        return {Action::Limit};
    }

    std::array<Slot, MaxSlots> m_slots {};
    uint32_t m_nextId = 0;
    uint32_t m_allocations = 0;
    uint32_t m_releases = 0;
    uint32_t m_reuses = 0;
    uint32_t m_receipts = 0;
    bool m_stopped = false;
    bool m_capped = false;
    bool m_partial = false;
};

} // namespace Common::Diagnostics

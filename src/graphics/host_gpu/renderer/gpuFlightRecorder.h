#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUFLIGHTRECORDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUFLIGHTRECORDER_H_

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics::GpuTrace {

// Record commands on the CPU as they are ENCODED. These markers never claim
// that the GPU executed or completed the command.
enum class Kind : uint32_t {
    GuestBegin = 1,
    RenderingBegin,
    RenderingEnd,
    GraphicsPipeline,
    ComputePipeline,
    VertexShader,
    PixelShader,
    ComputeShader,
    DescriptorWrites,
    DescriptorBind,
    DescriptorImage,
    DescriptorImageIdentity,
    ImageLookup,
    ImageViewBound,
    DescriptorBinding,
    ImageBarrier,
    ImageBarrierId,
    IndexBuffer,
    Draw,
    DrawIndexed,
    DrawMesh,
    Dispatch,
    DispatchIndirect,
    KnownAttachmentWriteFix,
    // Opt-in CPU snapshots of complete VkImageMemoryBarrier2 dependencies.
    ImageBarrierStages,
    ImageBarrierAccess,
    ImageBarrierSubresource,
    ShaderResourceShape,
};

inline const char* Name(Kind kind) noexcept {
    switch (kind) {
        case Kind::GuestBegin: return "guest_begin";
        case Kind::RenderingBegin: return "render_begin";
        case Kind::RenderingEnd: return "render_end";
        case Kind::GraphicsPipeline: return "graphics_pipeline";
        case Kind::ComputePipeline: return "compute_pipeline";
        case Kind::VertexShader: return "vertex_shader";
        case Kind::PixelShader: return "pixel_shader";
        case Kind::ComputeShader: return "compute_shader";
        case Kind::DescriptorWrites: return "descriptor_writes";
        case Kind::DescriptorBind: return "descriptor_bind";
        case Kind::DescriptorImage: return "descriptor_image";
        case Kind::DescriptorImageIdentity: return "descriptor_image_id";
        case Kind::ImageLookup: return "image_lookup";
        case Kind::ImageViewBound: return "image_view_bound";
        case Kind::DescriptorBinding: return "descriptor_binding";
        case Kind::ImageBarrier: return "image_barrier";
        case Kind::ImageBarrierId: return "image_barrier_id";
        case Kind::IndexBuffer: return "index_buffer";
        case Kind::Draw: return "draw";
        case Kind::DrawIndexed: return "draw_indexed";
        case Kind::DrawMesh: return "draw_mesh";
        case Kind::Dispatch: return "dispatch";
        case Kind::DispatchIndirect: return "dispatch_indirect";
        case Kind::KnownAttachmentWriteFix: return "known_attachment_write_fix";
        case Kind::ImageBarrierStages: return "image_barrier_stages";
        case Kind::ImageBarrierAccess: return "image_barrier_access";
        case Kind::ImageBarrierSubresource: return "image_barrier_subresource";
        case Kind::ShaderResourceShape: return "shader_resource_shape";
    }
    return "unknown";
}

// Values are counts, Vulkan enum values, or shader hashes. Never put CPU/GPU
// pointers, raw game content, or descriptor-handle addresses into this type.
struct Event {
    Kind kind = Kind::GuestBegin;
    uint32_t a = 0, b = 0, c = 0;
    uint64_t identity = 0;
    uint64_t extra = 0;
};

class CommandTrace {
public:
    static constexpr size_t Capacity = 1024;
    // Keep the first guest commands AND the latest GPU commands even when
    // an encoded command buffer exceeds Capacity (Syberia has 424).
    static constexpr size_t PrefixCapacity = 96;
    static constexpr size_t TailCapacity = Capacity - PrefixCapacity;

    void Reset() noexcept { m_total = 0; }
    void Push(Kind kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
              uint64_t identity = 0, uint64_t extra = 0) noexcept {
        const size_t slot = m_total < PrefixCapacity
                          ? static_cast<size_t>(m_total)
                          : PrefixCapacity + static_cast<size_t>(
                                (m_total - PrefixCapacity) % TailCapacity);
        m_events[slot] = {kind, a, b, c, identity, extra};
        ++m_total;
    }
    [[nodiscard]] uint64_t Total() const noexcept { return m_total; }
    [[nodiscard]] size_t Count() const noexcept {
        return static_cast<size_t>(std::min<uint64_t>(m_total, Capacity));
    }
    [[nodiscard]] uint64_t Dropped() const noexcept { return m_total - Count(); }
    // Retained events have a gap when the fixed tail ring wraps. Report the
    // ORIGINAL CPU encoding index so users cannot mistake event 104 for 64.
    [[nodiscard]] uint64_t OriginalIndex(size_t retained_index) const noexcept {
        const uint64_t prefix = std::min<uint64_t>(m_total, PrefixCapacity);
        if (retained_index < prefix) return retained_index;
        const uint64_t tail_total = m_total > prefix ? m_total - prefix : 0;
        const uint64_t retained_tail = std::min<uint64_t>(tail_total, TailCapacity);
        return prefix + tail_total - retained_tail + (retained_index - prefix);
    }
    // Stable under guest submit-ID changes. This is a diagnostic grouping
    // signature, NOT permission to change rendering or replay a lost device.
    [[nodiscard]] uint64_t Signature() const noexcept {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t i = 0; i < Count(); ++i) {
            const auto& e = At(i);
            if (e.kind == Kind::GuestBegin) continue;
            // Include extra: descriptor layouts, GPU access masks and
            // subresource data must distinguish otherwise-identical commands.
            const std::array<uint64_t, 6> values {
                static_cast<uint32_t>(e.kind), e.a, e.b, e.c, e.identity, e.extra
            };
            for (const auto value: values)
                hash = (hash ^ value) * UINT64_C(1099511628211);
        }
        return hash;
    }
    [[nodiscard]] const Event& At(size_t oldest_index) const noexcept {
        const size_t prefix_count = static_cast<size_t>(
            std::min<uint64_t>(m_total, PrefixCapacity));
        if (oldest_index < prefix_count) return m_events[oldest_index];
        const uint64_t tail_seen = m_total > PrefixCapacity ? m_total - PrefixCapacity : 0;
        const uint64_t tail_retained = std::min<uint64_t>(tail_seen, TailCapacity);
        const uint64_t tail_oldest = tail_seen - tail_retained;
        const size_t tail_index = oldest_index - prefix_count;
        return m_events[PrefixCapacity + static_cast<size_t>(
            (tail_oldest + tail_index) % TailCapacity)];
    }
private:
    std::array<Event, Capacity> m_events {};
    uint64_t m_total = 0;
};

struct SubmittedTrace {
    uint64_t tick = 0;
    CommandTrace commands {};
};

// Initial checkpoints cover start-up stalls; steady state is sparse.
// Called only when diagnostics are opt-in. Never changes queue order.
[[nodiscard]] constexpr bool ShouldSampleTick(uint64_t tick) noexcept {
    if (tick == 0) return false;
    if (tick <= 2048) return (tick & (tick - 1)) == 0;
    return tick % 2048 == 0;
}

class FlightRecorder {
public:
    static constexpr size_t Capacity = 24;

    void Add(uint64_t tick, const CommandTrace& commands) noexcept {
        m_history[static_cast<size_t>(m_total % Capacity)] = {tick, commands};
        ++m_total;
    }
    [[nodiscard]] size_t Count() const noexcept {
        return static_cast<size_t>(std::min<uint64_t>(m_total, Capacity));
    }
    [[nodiscard]] const SubmittedTrace& At(size_t oldest_index) const noexcept {
        return m_history[static_cast<size_t>((m_total - Count() + oldest_index) % Capacity)];
    }
    // The first UNCONFIRMED tick is a boundary, not a guilty GPU command.
    void Dump(FILE* output, uint64_t completed_tick, bool fault = true, bool full = false) const noexcept {
        if (output == nullptr) return;
        const auto first = completed_tick == UINT64_MAX ? completed_tick : completed_tick + 1;
        std::fprintf(output, "XXII_GPU_FLIGHT_SUMMARY completed=%" PRIu64
                             " first_unconfirmed=%" PRIu64 " buffers=%zu\n",
                     completed_tick, first, Count());
        // Include preceding and next in-flight buffers; avoid huge fault logs.
        size_t printed = 0;
        for (size_t i = 0; i < Count() && printed < 12; ++i) {
            const auto& item = At(i);
            if ((item.tick < completed_tick || item.tick - completed_tick > 20) &&
                !(completed_tick > 0 && item.tick == completed_tick - 1)) continue;
            ++printed;
            std::fprintf(output, "XXII_GPU_FLIGHT_BUFFER tick=%" PRIu64
                                 " completed=%u events=%" PRIu64 " dropped=%" PRIu64 " signature=0x%016" PRIx64 "\n",
                         item.tick, item.tick <= completed_tick ? 1u : 0u,
                         item.commands.Total(), item.commands.Dropped(), item.commands.Signature());
            const auto end = item.commands.Count();
            const bool around_fault = full && item.tick + 1 >= completed_tick &&
                                      item.tick <= completed_tick + 2;
            const auto begin = around_fault ? size_t {0} : (end > 20 ? end - 20 : 0);
            for (size_t j = begin; j < end; ++j) {
                const auto& e = item.commands.At(j);
                std::fprintf(output, "XXII_GPU_FLIGHT_EVENT tick=%" PRIu64
                                     " index=%" PRIu64 " kind=%s a=%u b=%u c=%u"
                                     " identity=0x%016" PRIx64 " extra=0x%016" PRIx64 "\n",
                             item.tick, item.commands.OriginalIndex(j), Name(e.kind), e.a, e.b, e.c,
                             e.identity, e.extra);
            }
        }
        // Put the first non-completed buffer's fingerprint and trailing
        // commands at the END, so bounded frontend logs retain them.
        const SubmittedTrace* first_unconfirmed = nullptr;
        for (size_t i = 0; i < Count(); ++i) {
            const auto& item = At(i);
            if (item.tick > completed_tick &&
                (first_unconfirmed == nullptr || item.tick < first_unconfirmed->tick))
                first_unconfirmed = &item;
        }
        if (fault && first_unconfirmed != nullptr) {
            std::fprintf(output, "XXII_GPU_FLIGHT_PRIMARY tick=%" PRIu64
                                 " first_unconfirmed=1 signature=0x%016" PRIx64
                                 " total=%" PRIu64 " dropped=%" PRIu64 "\n",
                         first_unconfirmed->tick, first_unconfirmed->commands.Signature(),
                         first_unconfirmed->commands.Total(),
                         first_unconfirmed->commands.Dropped());
            const auto n = first_unconfirmed->commands.Count();
            for (size_t i = full ? 0 : n > 12 ? n - 12 : 0; i < n; ++i) {
                const auto& e = first_unconfirmed->commands.At(i);
                std::fprintf(output, "XXII_GPU_FLIGHT_PRIMARY_EVENT tick=%" PRIu64
                                     " index=%" PRIu64 " kind=%s a=%u b=%u c=%u"
                                     " identity=0x%016" PRIx64 " extra=0x%016" PRIx64 "\n",
                             first_unconfirmed->tick, first_unconfirmed->commands.OriginalIndex(i), Name(e.kind),
                             e.a, e.b, e.c, e.identity, e.extra);
            }
        }
        if (printed == 0) {
            std::fprintf(output, "XXII_GPU_FLIGHT_NO_MATCH completed=%" PRIu64 "\n",
                         completed_tick);
        }
        std::fflush(output);
    }
private:
    std::array<SubmittedTrace, Capacity> m_history {};
    uint64_t m_total = 0;
};

} // namespace Libs::Graphics::GpuTrace

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUFLIGHTRECORDER_H_
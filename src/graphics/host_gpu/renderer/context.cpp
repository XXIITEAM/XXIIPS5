#include "common/assert.h"
#include "common/common.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdlib>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	return m_buffer;
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	m_gpu_trace.Reset();
	static const bool flight_enabled = [] {
		const char* probe = std::getenv("XXII_COMPAT_PROBE");
		const char* mode = std::getenv("XXII_COMPAT_GPU_FLIGHT_RECORDER");
		return probe && probe[0] == '1' && probe[1] == '\0' &&
		       mode && mode[0] == '1' && mode[1] == '\0';
	}();
	m_gpu_trace_enabled = flight_enabled;
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
	RecordGpuTrace(GpuTrace::Kind::GuestBegin, op, arg0, arg1, submit_id, arg4);
    // Optional per-operation GPU execution marker. The ordinary game path
    // remains unchanged unless both an explicit compatibility probe and a
    // numeric start tick are supplied. This marker is recorded BEFORE the
    // guest op adds its Vulkan commands to this command buffer.
    static const uint64_t op_checkpoint_start = [] {
        if (std::getenv("XXII_COMPAT_PROBE") == nullptr) return uint64_t {0};
        const char* value = std::getenv("XXII_GPU_OP_CHECKPOINT_START");
        if (!value || !*value) return uint64_t {0};
        char* end = nullptr;
        const auto tick = std::strtoull(value, &end, 10);
        return end != value && *end == '\0' && tick >= 1000 && tick <= 10000000
                   ? static_cast<uint64_t>(tick) : uint64_t {0};
    }();
    if (op_checkpoint_start && m_graphics.device_checkpoints_enabled &&
        m_buffer != nullptr &&
        m_context.GetCommandScheduler().CurrentTick() >= op_checkpoint_start) {
        const auto checkpoint = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(
                static_cast<VkDevice>(m_graphics.device), "vkCmdSetCheckpointNV"));
        if (checkpoint) {
            constexpr uint64_t OpTag = UINT64_C(1) << 41;
            // Preserve arg0 to distinguish a DrawIndex entry (e.g. 600)
            // from subsequent SetDrawDebugPhase calls (e.g. 0x100).
            // Submit IDs are truncated to their low 23 bits in this marker.
            constexpr uint64_t SubmitMask = (UINT64_C(1) << 23) - 1;
            constexpr uint64_t ArgMask = (UINT64_C(1) << 12) - 1;
            const uint64_t marker = OpTag | ((submit_id & SubmitMask) << 18) |
                                    ((static_cast<uint64_t>(arg0) & ArgMask) << 6) |
                                    (static_cast<uint64_t>(op) & 63);
            checkpoint(static_cast<VkCommandBuffer>(m_buffer),
                       reinterpret_cast<const void*>(
                           static_cast<uintptr_t>(marker)));
        }
    }
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	if (m_rendering && m_render_state == state) {
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering();

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	RecordGpuTrace(GpuTrace::Kind::RenderingBegin, state.width, state.height,
	               state.num_color_attachments, state.num_layers,
	               depth_stencil.has_depth ? 1u : 0u);
	m_render_state = state;
	m_rendering    = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	Handle().endRendering();
	RecordGpuTrace(GpuTrace::Kind::RenderingEnd);
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics

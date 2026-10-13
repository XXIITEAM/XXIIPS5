#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEBARRIERPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEBARRIERPOLICY_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics::ImageOps {

// Reusing an unchanged layout and access mask does not eliminate a write
// dependency. In particular, dynamic-rendering color and depth/stencil
// attachment writes must be ordered before subsequent attachment writes.
[[nodiscard]] constexpr bool RequiresImageBarrier(vk::ImageLayout old_layout,
                                                   vk::AccessFlags2 old_access,
                                                   vk::ImageLayout new_layout,
                                                   vk::AccessFlags2 new_access) noexcept {
    constexpr vk::AccessFlags2 writes =
        vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eShaderWrite |
        vk::AccessFlagBits2::eMemoryWrite |
        vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    return old_layout != new_layout || old_access != new_access ||
           static_cast<bool>(old_access & writes);
}

} // namespace Libs::Graphics::ImageOps

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEBARRIERPOLICY_H_

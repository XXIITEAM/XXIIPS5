#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NULLTEXTUREFORMAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NULLTEXTUREFORMAT_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

// The guest may legally supply a null descriptor for a depth-comparison
// lookup. The host must still bind a depth-comparison-capable Vulkan image.
// Reusing the ordinary R32_SFLOAT null texture violates VUID 06479.
struct NullTextureFormatPolicy {
    vk::Format image_format = vk::Format::eUndefined;
    vk::Format view_format = vk::Format::eUndefined;
    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
};

[[nodiscard]] constexpr NullTextureFormatPolicy
SelectNullTextureFormat(vk::Format guest_format, bool depth_compare, bool atomic64) noexcept {
    if (depth_compare) {
        if (atomic64 || guest_format != vk::Format::eR32Sfloat) return {};
        return {vk::Format::eD32Sfloat, vk::Format::eD32Sfloat,
                vk::ImageAspectFlagBits::eDepth};
    }
    return {guest_format, atomic64 ? vk::Format::eR64Uint : guest_format,
            vk::ImageAspectFlagBits::eColor};
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NULLTEXTUREFORMAT_H_

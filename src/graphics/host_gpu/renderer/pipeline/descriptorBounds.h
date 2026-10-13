#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DESCRIPTORBOUNDS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DESCRIPTORBOUNDS_H_

#include <cstddef>

namespace Libs::Graphics {

// A compiled guest shader supplies descriptor-array offsets. Revalidate the
// final host span before materializing Vulkan image descriptors. All checks
// use subtraction after verifying order, avoiding integer wraparound.
// This protects against malformed compiler metadata; it does not substitute
// for GPU shader access validation or change any valid resource binding.
[[nodiscard]] constexpr bool FitsImageDescriptorSpan(
    size_t descriptor_array_size, size_t binding_start,
    size_t binding_count, size_t resource_offset, size_t resource_count) noexcept {
    return resource_count != 0 &&
           binding_start <= descriptor_array_size &&
           binding_count <= descriptor_array_size - binding_start &&
           resource_offset <= binding_count &&
           resource_count <= binding_count - resource_offset;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DESCRIPTORBOUNDS_H_

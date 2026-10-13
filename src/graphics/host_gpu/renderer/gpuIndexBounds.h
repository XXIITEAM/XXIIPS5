#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPU_INDEX_BOUNDS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPU_INDEX_BOUNDS_H_

#include <cstdint>

namespace Libs::Graphics::GpuTrace {

// Validate actual Vulkan buffer capacity after guest buffer ranges have been
// merged or copied by BufferCache. This is a CPU host bound check, not an
// assertion that shader vertex-index fetches are themselves safe.
[[nodiscard]] constexpr bool FitsIndexBufferSpan(uint64_t capacity, uint64_t offset,
                                                 uint64_t bytes, uint64_t index_alignment) noexcept {
    return bytes != 0 && index_alignment != 0 &&
           (index_alignment & (index_alignment - 1)) == 0 &&
           offset % index_alignment == 0 && bytes % index_alignment == 0 &&
           offset <= capacity && bytes <= capacity - offset;
}

} // namespace Libs::Graphics::GpuTrace

#endif

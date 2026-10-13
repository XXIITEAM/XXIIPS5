#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NULLTEXTUREFORMAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NULLTEXTUREFORMAT_H_

namespace Libs::Graphics {

// The guest can supply a null shadow-map descriptor. Color and depth-compare
// float images need different Vulkan formats, but this decision deliberately
// has no Vulkan-header dependency so the standalone IR tests can verify it.
enum class NullTextureKind {
    Color,
    DepthComparison,
    Unsupported
};

[[nodiscard]] constexpr NullTextureKind
SelectNullTextureKind(bool float32_format, bool depth_compare, bool atomic64) noexcept {
    if (!depth_compare) return NullTextureKind::Color;
    return float32_format && !atomic64 ? NullTextureKind::DepthComparison
                                      : NullTextureKind::Unsupported;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NULLTEXTUREFORMAT_H_

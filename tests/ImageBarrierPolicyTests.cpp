#include "graphics/host_gpu/renderer/image/imageBarrierPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
using namespace Libs::Graphics;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "ImageBarrierPolicyTests FAILED: %s\n", label);
        std::exit(1);
    }
}

constexpr auto General = vk::ImageLayout::eGeneral;
constexpr auto Color = vk::ImageLayout::eColorAttachmentOptimal;
constexpr auto Depth = vk::ImageLayout::eDepthStencilAttachmentOptimal;
constexpr auto Sampled = vk::AccessFlagBits2::eShaderRead;

static_assert(ImageOps::RequiresImageBarrier(
    Color, vk::AccessFlagBits2::eColorAttachmentWrite,
    Color, vk::AccessFlagBits2::eColorAttachmentWrite));
static_assert(ImageOps::RequiresImageBarrier(
    Depth, vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
    Depth, vk::AccessFlagBits2::eDepthStencilAttachmentWrite));
static_assert(!ImageOps::RequiresImageBarrier(General, Sampled, General, Sampled));
}

int main() {
    // The same rule is called by whole-image and per-subresource state paths.
    using ImageOps::RequiresImageBarrier;

    for (const auto access : {
             vk::AccessFlags2 {vk::AccessFlagBits2::eShaderWrite},
             vk::AccessFlags2 {vk::AccessFlagBits2::eTransferWrite},
             vk::AccessFlags2 {vk::AccessFlagBits2::eMemoryWrite},
             vk::AccessFlags2 {vk::AccessFlagBits2::eColorAttachmentWrite},
             vk::AccessFlags2 {vk::AccessFlagBits2::eDepthStencilAttachmentWrite}}) {
        Check(RequiresImageBarrier(General, access, General, access),
              "repeated writes need a barrier even with no layout change");
    }

    Check(!RequiresImageBarrier(General, Sampled, General, Sampled),
          "repeated reads may reuse the layout without a barrier");
    Check(RequiresImageBarrier(General, Sampled, General,
                               vk::AccessFlagBits2::eShaderWrite),
          "read-to-write access change needs a barrier");
    Check(RequiresImageBarrier(General, Sampled, Color, Sampled),
          "layout transition needs a barrier");
    Check(RequiresImageBarrier(Color, vk::AccessFlagBits2::eColorAttachmentWrite,
                               Color, vk::AccessFlagBits2::eColorAttachmentRead),
          "color write to color read needs a barrier");
    Check(RequiresImageBarrier(Depth, vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                               Depth, vk::AccessFlagBits2::eDepthStencilAttachmentRead),
          "depth write to depth read needs a barrier");
    std::printf("ImageBarrierPolicyTests: all cases passed\n");
    return 0;
}

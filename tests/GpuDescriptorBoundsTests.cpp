#include "graphics/host_gpu/renderer/pipeline/descriptorBounds.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using Libs::Graphics::FitsImageDescriptorSpan;

static_assert(FitsImageDescriptorSpan(32, 8, 12, 0, 12));
static_assert(FitsImageDescriptorSpan(32, 8, 12, 11, 1));
static_assert(!FitsImageDescriptorSpan(32, 8, 12, 11, 2));
static_assert(!FitsImageDescriptorSpan(32, 8, 12, 12, 0));
static_assert(!FitsImageDescriptorSpan(32, 33, 1, 0, 1));
static_assert(!FitsImageDescriptorSpan(32, 30, 4, 0, 1));

static void Require(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "GpuDescriptorBoundsTests: FAILED %s\n", label);
        std::exit(1);
    }
}

int main() {
    constexpr auto maximum = std::numeric_limits<size_t>::max();
    Require(FitsImageDescriptorSpan(8, 0, 8, 0, 8), "exact image range");
    Require(FitsImageDescriptorSpan(10, 6, 4, 3, 1), "last valid element");
    Require(!FitsImageDescriptorSpan(10, 6, 4, 3, 2), "mip span exceeds binding");
    Require(!FitsImageDescriptorSpan(10, 6, 4, 5, 1), "descriptor offset exceeds binding");
    Require(!FitsImageDescriptorSpan(10, 9, 2, 0, 1), "binding exceeds array");
    Require(!FitsImageDescriptorSpan(10, maximum, 1, 0, 1), "binding start integer overflow");
    Require(!FitsImageDescriptorSpan(10, 6, maximum, 0, 1), "binding count overflow");
    Require(!FitsImageDescriptorSpan(10, 6, 4, maximum, 1), "resource offset overflow");
    Require(!FitsImageDescriptorSpan(10, 6, 4, 1, maximum), "resource count overflow");
    Require(!FitsImageDescriptorSpan(0, 0, 0, 0, 1), "empty image array");
    std::puts("GPU descriptor bounds tests passed");
    return 0;
}

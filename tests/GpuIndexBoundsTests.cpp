#include "graphics/host_gpu/renderer/gpuIndexBounds.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using Libs::Graphics::GpuTrace::FitsIndexBufferSpan;

void Require(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "GpuIndexBoundsTests: FAILED %s\n", label);
        std::exit(1);
    }
}
int main() {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    static_assert(FitsIndexBufferSpan(4096, 4, 1200, 4));
    static_assert(FitsIndexBufferSpan(1024, 8, 600, 2));
    static_assert(!FitsIndexBufferSpan(1024, 1016, 16, 4));
    Require(FitsIndexBufferSpan(1200, 0, 1200, 2), "exact index range");
    Require(FitsIndexBufferSpan(1300, 100, 1200, 4), "end of allocation");
    Require(!FitsIndexBufferSpan(1300, 101, 1200, 4), "unaligned index offset");
    Require(!FitsIndexBufferSpan(1300, 100, 1201, 4), "unaligned index span");
    Require(!FitsIndexBufferSpan(1300, 0, 0, 2), "zero index span");
    Require(!FitsIndexBufferSpan(0, 0, 2, 2), "empty allocation");
    Require(!FitsIndexBufferSpan(1300, maximum, 2, 2), "offset overflow");
    Require(!FitsIndexBufferSpan(maximum, maximum - 2, 8, 4), "near max span");
    Require(!FitsIndexBufferSpan(1300, 0, 2, 3), "non power two alignment");
    std::puts("GPU index buffer bounds tests passed");
}

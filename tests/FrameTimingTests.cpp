#include "common/diagnostics/frameTiming.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {
void Require(bool condition, const char* name) {
	if (!condition) {
		std::fprintf(stderr, "FrameTimingTests FAILED: %s\n", name);
		std::exit(1);
	}
}
} // namespace

int main() {
	using Common::Diagnostics::FrameTiming;
	auto timing = std::make_unique<FrameTiming>();
	Require(timing->Drain().count == 0, "empty has no percentile");
	timing->Present(0);
	Require(timing->Drain().count == 0, "first successful present is only an anchor");
	timing->Present(1);
	auto single = timing->Drain();
	Require(single.count == 1 && single.p95Ns == 1 && single.p99Ns == 1,
	        "single interval nearest rank");
	timing = std::make_unique<FrameTiming>();
	timing->Present(0);
	uint64_t now = 0;
	for (uint64_t i = 100; i != 0; --i)
		timing->Present(now += i * 1000000);
	auto result = timing->Drain();
	Require(result.count == 100 && result.p95Ns == 95000000 && result.p99Ns == 99000000 &&
	            result.maxNs == 100000000,
	        "nearest rank percentiles of actual intervals");
	timing->Present(now);
	timing->Present(now - 1);
	timing->Present(now + 5000000000);
	result = timing->Drain();
	Require(result.invalid == 2 && result.count == 101 && result.maxNs == 5000000000 &&
	            result.p99Ns == 100000000,
	        "invalid clocks excluded, long inter-present stall retained");
	Require(timing->Drain().count == result.count, "drain does not duplicate samples");
	timing = std::make_unique<FrameTiming>();
	timing->Present(0);
	for (uint64_t i = 1; i <= FrameTiming::QueueCapacity + 1; ++i)
		timing->Present(i);
	result = timing->Drain();
	Require(result.count == FrameTiming::QueueCapacity && result.dropped == 1,
	        "producer queue saturation is bounded and visible");
	timing->Present(FrameTiming::QueueCapacity + 2);
	result = timing->Drain();
	Require(result.maxNs == 1, "dropped sample does not distort following interval");
	for (size_t batch = 0; batch < 5; ++batch) {
		for (size_t i = 0; i < FrameTiming::QueueCapacity; ++i)
			timing->Present(++now + 6000000000);
		timing->Drain();
	}
	result = timing->Drain();
	Require(result.count == FrameTiming::SampleCapacity && result.dropped > 1,
	        "capture capacity saturates without silently replacing samples");
	timing = std::make_unique<FrameTiming>();
	std::atomic<bool> done {false};
	std::thread       producer([&] {
		for (uint64_t i = 0; i <= 100000; ++i)
			timing->Present(i * 17);
		done.store(true, std::memory_order_release);
	});
	while (!done.load(std::memory_order_acquire))
		timing->Drain();
	producer.join();
	result = timing->Drain();
	Require(result.count + result.dropped == 100000 && result.invalid == 0 && result.p95Ns == 17 &&
	            result.p99Ns == 17,
	        "concurrent SPSC publication accounts for every interval");
	std::printf("FrameTimingTests PASS storage_bytes=%zu\n", sizeof(FrameTiming));
}

#ifndef XXII_COMMON_DIAGNOSTICS_FRAME_TIMING_H_
#define XXII_COMMON_DIAGNOSTICS_FRAME_TIMING_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Common::Diagnostics {

// One presentation producer, one window-thread consumer. No allocation, waits,
// clocks or logging here. Timestamps are monotonic nanoseconds supplied by the
// caller after successful queuePresent returns, NOT display/GPU completion.
class FrameTiming {
public:
	static constexpr size_t QueueCapacity  = 4096;
	static constexpr size_t SampleCapacity = 16384;
	static_assert(std::atomic<uint64_t>::is_always_lock_free);

	struct Summary {
		size_t   count   = 0;
		uint64_t dropped = 0;
		uint64_t invalid = 0;
		uint64_t p95Ns = 0, p99Ns = 0, maxNs = 0;
	};

	void Present(uint64_t nowNs) noexcept {
		if (!m_started) {
			m_previous = nowNs;
			m_started  = true;
			return;
		}
		if (nowNs <= m_previous) {
			m_invalid.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const auto duration = nowNs - m_previous;
		m_previous          = nowNs;
		const auto write    = m_write.load(std::memory_order_relaxed);
		if (write - m_read.load(std::memory_order_acquire) == QueueCapacity) {
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		m_queue[write % QueueCapacity] = duration;
		m_write.store(write + 1, std::memory_order_release);
	}

	Summary Drain() noexcept {
		auto       read  = m_read.load(std::memory_order_relaxed);
		const auto write = m_write.load(std::memory_order_acquire);
		for (; read != write; ++read) {
			if (m_count < SampleCapacity)
				m_samples[m_count++] = m_queue[read % QueueCapacity];
			else
				++m_storageDropped;
		}
		m_read.store(read, std::memory_order_release);
		Summary result {m_count, m_dropped.load(std::memory_order_relaxed) + m_storageDropped,
		                m_invalid.load(std::memory_order_relaxed)};
		if (m_count == 0) return result;
		// Only the window thread sorts a copy. Retain the entire bounded
		// capture, not a percentile-of-percentiles or an FPS-derived estimate.
		std::copy_n(m_samples.begin(), m_count, m_sorted.begin());
		std::sort(m_sorted.begin(), m_sorted.begin() + m_count);
		result.p95Ns = m_sorted[(m_count * 95 + 99) / 100 - 1];
		result.p99Ns = m_sorted[(m_count * 99 + 99) / 100 - 1];
		result.maxNs = m_sorted[m_count - 1];
		return result;
	}

private:
	std::array<uint64_t, QueueCapacity>  m_queue {};
	std::array<uint64_t, SampleCapacity> m_samples {}, m_sorted {};
	std::atomic<uint64_t>                m_write {0}, m_read {0}, m_dropped {0}, m_invalid {0};
	uint64_t                             m_previous = 0, m_storageDropped = 0;
	size_t                               m_count   = 0;
	bool                                 m_started = false;
};

} // namespace Common::Diagnostics

#endif
